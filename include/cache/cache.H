//
// cache.H -- Concurrent cache with single-flight per key
//
// Part of cpp-cache v2
//
// A general-purpose concurrent cache that:
// - Prevents thundering herd via single-flight per key
// - Caches both positive and negative results with separate TTLs
// - Uses shared_ptr for safe value lifetime management
// - Supports lazy invalidation
// - Never evicts entries in Computing state
// - Uses LRU eviction policy
//
// Reuses Aleph-w: OLhashTable for hash table, Dlink for LRU list
//

# ifndef CACHE_CACHE_H
# define CACHE_CACHE_H

# include <chrono>
# include <memory>
# include <mutex>
# include <condition_variable>
# include <functional>
# include <optional>
# include <atomic>
# include <cassert>

# include <aleph.H>
# include <dlink.H>
# include <tpl_olhash.H>

# include <cache/entry_state.H>
# include <cache/cache_result.H>

namespace CppCache
{
  // Cache statistics
  struct CacheStats
  {
    size_t hits          = 0;
    size_t misses        = 0;
    size_t negative_hits = 0;
    size_t evictions     = 0;
  };

  // Main cache class template
  //
  // Key   - type used for lookup (must be equality-comparable and hashable)
  // Value - type of cached data (managed via shared_ptr)
  // Cmp   - equality comparator for keys (default: std::equal_to<Key>)
  template <typename Key, typename Value, typename Cmp = std::equal_to<Key>>
  class Cache
  {
   public:

    // The miss solver receives a key and returns:
    //   - non-null shared_ptr<Value> on success (positive result)
    //   - nullptr on failure (negative result, will be cached with negative TTL)
    using MissSolver = std::function<std::shared_ptr<Value>(const Key &)>;

    using HashFct = std::function<size_t(const Key &)>;

   private:

    // ================================================================
    // CacheEntry -- internal entry with per-key synchronization
    // ================================================================
    class CacheEntry
    {
      friend class Cache;

      Key _key = Key();
      std::shared_ptr<Value> _value;
      EntryState _state = EntryState::Empty;

      Dlink _dlink_lru;

      std::mutex _mtx;
      std::condition_variable _cv;

      std::chrono::time_point<std::chrono::steady_clock> _expiration;

     public:

      CacheEntry() = default;

      explicit CacheEntry(const Key & k) : _key(k) {}

      explicit CacheEntry(Key && k) : _key(std::move(k)) {}

      CacheEntry(const CacheEntry & other)
        : _key(other._key), _value(other._value),
          _state(other._state), _expiration(other._expiration)
      {
        // mutex and cv are not copyable; Dlink default-constructed
      }

      CacheEntry(CacheEntry && other) noexcept
        : _key(std::move(other._key)), _value(std::move(other._value)),
          _state(other._state), _expiration(other._expiration)
      {
        // empty
      }

      CacheEntry & operator=(const CacheEntry & other)
      {
        if (this != &other)
          {
            _key = other._key;
            _value = other._value;
            _state = other._state;
            _expiration = other._expiration;
          }
        return *this;
      }

      CacheEntry & operator=(CacheEntry && other) noexcept
      {
        if (this != &other)
          {
            _key = std::move(other._key);
            _value = std::move(other._value);
            _state = other._state;
            _expiration = other._expiration;
          }
        return *this;
      }

      bool operator==(const CacheEntry & other) const
      {
        return Cmp()(_key, other._key);
      }

      const Key & key() const noexcept { return _key; }

      Dlink * link_lru() noexcept { return &_dlink_lru; }

      std::mutex & mtx() noexcept { return _mtx; }

      std::condition_variable & cv() noexcept { return _cv; }

      EntryState state() const noexcept { return _state; }

      void set_state(EntryState s) noexcept { _state = s; }

      const std::shared_ptr<Value> & value() const noexcept { return _value; }

      void set_value(std::shared_ptr<Value> v) noexcept
      {
        _value = std::move(v);
      }

      void clear_value() noexcept { _value.reset(); }

      bool has_expired(const std::chrono::steady_clock::time_point & now)
        const noexcept
      {
        return now > _expiration;
      }

      void set_expiration(
        const std::chrono::steady_clock::time_point & exp) noexcept
      {
        _expiration = exp;
      }

      // Reset entry to Empty state for reuse
      void reset() noexcept
      {
        _state = EntryState::Empty;
        _value.reset();
      }

      void swap(CacheEntry & other) noexcept
      {
        using std::swap;
        swap(_key, other._key);
        swap(_value, other._value);
        swap(_state, other._state);
        swap(_expiration, other._expiration);
      }

    }; // end class CacheEntry

    // ================================================================
    // Comparator adapter for hash table
    // ================================================================
    struct EntryCmp
    {
      bool operator()(const CacheEntry & a, const CacheEntry & b) const
      {
        return Cmp()(a.key(), b.key());
      }
    };

    // ================================================================
    // Data members
    // ================================================================

    Dlink _lru_list;   // Sentinel for doubly-linked LRU list
    size_t _capacity;  // Maximum number of entries

    using HashTable = OLhashTable<CacheEntry, EntryCmp>;
    HashTable _table;

    std::chrono::seconds _positive_ttl;
    std::chrono::seconds _negative_ttl;

    std::mutex _mtx;   // Global mutex: protects hash table + LRU list

    MissSolver _solver;

    CacheStats _stats;

    // ================================================================
    // LRU helpers (must be called with _mtx held)
    // ================================================================

    LINKNAME_TO_TYPE(CacheEntry, _dlink_lru);

    void lru_insert_as_mru(CacheEntry * entry)
    {
      _lru_list.insert(entry->link_lru());
    }

    void lru_move_to_mru(CacheEntry * entry)
    {
      entry->link_lru()->del();
      _lru_list.insert(entry->link_lru());
    }

    void lru_remove(CacheEntry * entry)
    {
      entry->link_lru()->del();
    }

    // Find the least-recently-used entry that is NOT Computing.
    // Walks from LRU end (prev of sentinel) toward MRU.
    // Returns nullptr if all entries are Computing (saturated state).
    CacheEntry * find_evictable_lru() const
    {
      for (Dlink * p = _lru_list.get_prev();
           p != const_cast<Dlink *>(&_lru_list);
           p = p->get_prev())
        {
          CacheEntry * entry = _dlink_lru_to_CacheEntry(p);
          if (entry->state() != EntryState::Computing)
            return entry;
        }
      return nullptr;
    }

    // Remove entry from hash table and LRU list
    void remove_entry(CacheEntry * entry)
    {
      lru_remove(entry);
      entry->reset();
      _table.remove(*entry);
      ++_stats.evictions;
    }

    // ================================================================
    // Hash table adapter
    // ================================================================

    // Wraps a Key hash function into a CacheEntry hash function
    static typename HashTable::Hash_Fct
    make_entry_hash(HashFct key_hash)
    {
      return [key_hash = std::move(key_hash)](const CacheEntry & entry) -> size_t
      {
        return key_hash(entry.key());
      };
    }

    // Default hash function for Key using Aleph's dft_hash_fct
    static size_t default_key_hash(const Key & key) noexcept
    {
      return Aleph::dft_hash_fct(key);
    }

    // ================================================================
    // Core: contains_or_insert with eviction
    // Must be called with _mtx held.
    // Returns {pointer_to_entry, was_already_in_table}
    // Returns {nullptr, false} if cache is full and all entries are Computing
    // ================================================================
    std::pair<CacheEntry *, bool>
    contains_or_insert_locked(const Key & key)
    {
      assert(_table.size() <= _capacity);

      // First try: the key might already be in the table
      CacheEntry * existing = _table.search(CacheEntry(key));
      if (existing != nullptr)
        {
          lru_move_to_mru(existing);
          return {existing, true};
        }

      // Key not found -- we need to insert. Evict first if at capacity.
      if (_table.size() == _capacity)
        {
          CacheEntry * victim = find_evictable_lru();
          if (victim == nullptr)
            {
              // All entries are Computing -- saturated.
              return {nullptr, false};
            }
          remove_entry(victim);
        }

      assert(_table.size() < _capacity);

      auto [entry_ptr, existed] =
        _table.contains_or_insert(CacheEntry(key));

      if (entry_ptr == nullptr)
        return {nullptr, false}; // Hash table internal failure

      // Should not already exist (we just searched)
      assert(!existed);

      lru_insert_as_mru(entry_ptr);
      return {entry_ptr, false};
    }

    // ================================================================
    // Determines if an existing entry is a valid hit
    // ================================================================
    bool is_valid_hit(CacheEntry * entry,
                      const std::chrono::steady_clock::time_point & now)
    {
      if (entry->state() == EntryState::Invalidated)
        return false;

      if (entry->state() == EntryState::Empty)
        return false;

      if (entry->state() == EntryState::Computing)
        return true; // Hit on computing entry: caller will wait

      // Ready or Failed: check TTL
      return !entry->has_expired(now);
    }

    // ================================================================
    // resolve_hit: handles the case when key is found in hash table
    // Returns a CacheResult if the entry is valid, or nullopt if expired/invalid
    // ================================================================
    std::optional<CacheResult<Value>>
    resolve_hit(CacheEntry * entry,
                const std::chrono::steady_clock::time_point & now)
    {
      std::unique_lock entry_lock(entry->mtx());

      if (!is_valid_hit(entry, now))
        {
          // Expired or invalidated: reset for recomputation
          entry->set_state(EntryState::Empty);
          entry->clear_value();
          return std::nullopt;
        }

      // If Computing, wait for it to finish
      if (entry->state() == EntryState::Computing)
        {
          entry->cv().wait(entry_lock, [entry]
          {
            return entry->state() != EntryState::Computing;
          });
        }

      // Now entry is Ready, Failed, or was reset by another thread
      if (entry->state() == EntryState::Ready)
        {
          _mtx.lock();
          ++_stats.hits;
          _mtx.unlock();
          return CacheResult<Value>::hit_positive(entry->value());
        }

      if (entry->state() == EntryState::Failed)
        {
          _mtx.lock();
          ++_stats.negative_hits;
          _mtx.unlock();
          return CacheResult<Value>::hit_negative();
        }

      // Entry was reset while we waited (e.g., invalidated)
      return std::nullopt;
    }

    // ================================================================
    // resolve_miss: runs the miss solver on a new/reset entry
    // The entry is already in the hash table with state Empty.
    // ================================================================
    CacheResult<Value>
    resolve_miss(CacheEntry * entry,
                 const std::chrono::steady_clock::time_point & now)
    {
      std::unique_lock entry_lock(entry->mtx());

      // Double-check: another thread may have already started computing
      if (entry->state() == EntryState::Computing)
        {
          entry->cv().wait(entry_lock, [entry]
          {
            return entry->state() != EntryState::Computing;
          });

          if (entry->state() == EntryState::Ready)
            return CacheResult<Value>::hit_positive(entry->value());

          if (entry->state() == EntryState::Failed)
            return CacheResult<Value>::hit_negative();
        }

      // If entry is already Ready/Failed (race with another thread)
      if (entry->state() == EntryState::Ready)
        return CacheResult<Value>::hit_positive(entry->value());

      if (entry->state() == EntryState::Failed)
        return CacheResult<Value>::hit_negative();

      // We are the one to compute
      entry->set_state(EntryState::Computing);
      entry_lock.unlock();

      // Run the solver WITHOUT holding any locks
      std::shared_ptr<Value> result;
      bool solver_succeeded = false;
      try
        {
          result = _solver(entry->key());
          solver_succeeded = (result != nullptr);
        }
      catch (...)
        {
          solver_succeeded = false;
        }

      entry_lock.lock();

      CacheResult<Value> cache_result =
        solver_succeeded
          ? CacheResult<Value>::computed_positive(result)
          : CacheResult<Value>::computed_negative();

      if (solver_succeeded)
        {
          entry->set_value(std::move(result));
          entry->set_state(EntryState::Ready);
          entry->set_expiration(now + _positive_ttl);
        }
      else
        {
          entry->clear_value();
          entry->set_state(EntryState::Failed);
          entry->set_expiration(now + _negative_ttl);
        }

      entry_lock.unlock();

      // Update LRU position
      {
        std::scoped_lock glock(_mtx);
        lru_move_to_mru(entry);
        ++_stats.misses;
      }

      // Wake up all threads waiting on this key
      entry->cv().notify_all();

      return cache_result;
    }

   public:

    static constexpr float table_ratio = 1.3f;

    // Constructor
    //
    // capacity       - maximum number of entries in the cache
    // positive_ttl   - time-to-live for successful results
    // negative_ttl   - time-to-live for failed results
    // solver         - callable invoked on cache miss; returns shared_ptr<Value>
    //                  (non-null = success, nullptr = cacheable failure)
    // hash_fct       - hash function for keys (default: Aleph's dft_hash_fct)
    Cache(size_t capacity,
          const std::chrono::seconds & positive_ttl,
          const std::chrono::seconds & negative_ttl,
          MissSolver solver,
          HashFct hash_fct = default_key_hash)
      : _capacity(capacity),
        _table(static_cast<size_t>(table_ratio * capacity),
               make_entry_hash(hash_fct),
               EntryCmp(),
               hash_default_lower_alpha,
               hash_default_upper_alpha,
               false),
        _positive_ttl(positive_ttl),
        _negative_ttl(negative_ttl),
        _solver(std::move(solver))
    {
      assert(capacity > 0);
    }

    // Non-copyable, non-movable (contains mutex)
    Cache(const Cache &) = delete;
    Cache & operator=(const Cache &) = delete;
    Cache(Cache &&) = delete;
    Cache & operator=(Cache &&) = delete;

    // =============================================================
    // get_or_compute -- Main API
    //
    // Looks up the key. If found and valid, returns cached result.
    // If not found or expired, runs the miss solver exactly once
    // (single-flight per key). Concurrent requests for the same key
    // wait and share the same result.
    //
    // Requests for different keys proceed in parallel.
    // =============================================================
    CacheResult<Value> get_or_compute(const Key & key)
    {
      std::pair<CacheEntry *, bool> p;
      {
        std::scoped_lock glock(_mtx);
        p = contains_or_insert_locked(key);
      }

      CacheEntry * entry = p.first;
      const bool existed = p.second;

      if (entry == nullptr)
        {
          // Cache saturated (all entries Computing). Compute directly
          // without caching -- degrades gracefully.
          auto result = _solver(key);
          if (result)
            return CacheResult<Value>::computed_positive(std::move(result));
          else
            return CacheResult<Value>::computed_negative();
        }

      auto now = std::chrono::steady_clock::now();

      if (existed)
        {
          auto hit_result = resolve_hit(entry, now);
          if (hit_result.has_value())
            return hit_result.value();
          // Entry was expired/invalid -- fall through to resolve_miss
        }

      return resolve_miss(entry, now);
    }

    // =============================================================
    // find -- Non-computing lookup
    //
    // Returns the cached result if the key is present and valid.
    // Does NOT invoke the miss solver. Returns nullopt on miss.
    // =============================================================
    std::optional<CacheResult<Value>> find(const Key & key)
    {
      CacheEntry * entry;
      {
        std::scoped_lock glock(_mtx);
        entry = _table.search(CacheEntry(key));
        if (entry == nullptr)
          return std::nullopt;
        lru_move_to_mru(entry);
      }

      auto now = std::chrono::steady_clock::now();
      std::unique_lock entry_lock(entry->mtx());

      if (!is_valid_hit(entry, now))
        return std::nullopt;

      if (entry->state() == EntryState::Computing)
        {
          entry->cv().wait(entry_lock, [entry]
          {
            return entry->state() != EntryState::Computing;
          });
        }

      if (entry->state() == EntryState::Ready)
        return CacheResult<Value>::hit_positive(entry->value());

      if (entry->state() == EntryState::Failed)
        return CacheResult<Value>::hit_negative();

      return std::nullopt;
    }

    // =============================================================
    // invalidate -- Lazy invalidation
    //
    // Marks the entry as invalid. It will not be treated as a hit.
    // Physical removal is deferred to LRU eviction.
    // Returns true if the key was found and invalidated.
    // Returns false if the key was not found, already invalid/expired,
    // or currently Computing.
    // =============================================================
    bool invalidate(const Key & key)
    {
      CacheEntry * entry;
      {
        std::scoped_lock glock(_mtx);
        entry = _table.search(CacheEntry(key));
        if (entry == nullptr)
          return false;
      }

      std::scoped_lock entry_lock(entry->mtx());

      if (entry->state() == EntryState::Computing)
        return false; // Cannot invalidate while computing

      if (entry->state() == EntryState::Invalidated ||
          entry->state() == EntryState::Empty)
        return false;

      entry->set_state(EntryState::Invalidated);
      entry->set_expiration(std::chrono::steady_clock::now());

      // Move to LRU position to prioritize eviction
      {
        std::scoped_lock glock(_mtx);
        Dlink * link = entry->link_lru();
        link->del();
        _lru_list.append(link);
      }

      return true;
    }

    // =============================================================
    // touch -- Refresh TTL and move to MRU
    //
    // Returns true if the key was found, valid, and touched.
    // =============================================================
    bool touch(const Key & key)
    {
      CacheEntry * entry;
      {
        std::scoped_lock glock(_mtx);
        entry = _table.search(CacheEntry(key));
        if (entry == nullptr)
          return false;
      }

      auto now = std::chrono::steady_clock::now();
      std::scoped_lock entry_lock(entry->mtx());

      if (entry->state() != EntryState::Ready)
        return false;

      if (entry->has_expired(now))
        return false;

      entry->set_expiration(now + _positive_ttl);

      {
        std::scoped_lock glock(_mtx);
        lru_move_to_mru(entry);
      }

      return true;
    }

    // =============================================================
    // has -- Check if key is present and valid (non-blocking)
    // =============================================================
    bool has(const Key & key)
    {
      CacheEntry * entry;
      {
        std::scoped_lock glock(_mtx);
        entry = _table.search(CacheEntry(key));
        if (entry == nullptr)
          return false;
      }

      auto now = std::chrono::steady_clock::now();
      std::scoped_lock entry_lock(entry->mtx());

      return is_valid_hit(entry, now);
    }

    // =============================================================
    // remove -- Eagerly remove an entry
    //
    // Unlike invalidate(), this physically removes the entry.
    // Cannot remove an entry in Computing state.
    // =============================================================
    bool remove(const Key & key)
    {
      std::scoped_lock glock(_mtx);

      CacheEntry * entry = _table.search(CacheEntry(key));
      if (entry == nullptr)
        return false;

      std::scoped_lock entry_lock(entry->mtx());

      if (entry->state() == EntryState::Computing)
        return false;

      remove_entry(entry);
      return true;
    }

    // =============================================================
    // Queries
    // =============================================================

    size_t size() const
    {
      std::scoped_lock glock(const_cast<std::mutex &>(_mtx));
      return _table.size();
    }

    size_t capacity() const noexcept { return _capacity; }

    CacheStats stats() const
    {
      std::scoped_lock glock(const_cast<std::mutex &>(_mtx));
      return _stats;
    }

  }; // end class Cache

} // namespace CppCache

# endif // CACHE_CACHE_H
