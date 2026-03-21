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

#ifndef CACHE_CACHE_H
#define CACHE_CACHE_H

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <aleph.H>
#include <dlink.H>
#include <tpl_olhash.H>

#include <cache/cache_result.H>
#include <cache/entry_state.H>

namespace CppCache {

/**
 * @brief Statistics for cache performance monitoring.
 */
struct CacheStats
{
  size_t hits = 0;           /**< Successful lookups found in cache. */
  size_t misses = 0;         /**< Lookups that required computing the value. */
  size_t negative_hits = 0;  /**< Successful lookups of previously cached failures. */
  size_t evictions = 0;      /**< Number of entries evicted by LRU policy. */
  size_t saturations = 0;    /**< Failures due to all entries being in Computing state. */
};

/**
 * @brief A high-performance concurrent cache with single-flight thundering herd prevention.
 *
 * This cache is designed for scenarios where many threads may request the same
 * expensive-to-compute resource simultaneously. Key features:
 *
 * - **Single-Flight:** Only one thread computes a missing key; others wait for the result.
 * - **Negative Caching:** Supports caching failures (nullptr results) with a separate TTL.
 * - **LRU Eviction:** Automatically evicts least recently used entries when capacity is reached.
 * - **Saturation Protection:** Does not evict entries currently being computed, preventing
 *   infinite recursion or resource exhaustion.
 * - **Thread-Safe:** Uses a combination of a global mutex for structural changes and
 *   per-entry mutexes for fine-grained coordination.
 *
 * @tparam Key The type used for lookup. Must be equality-comparable and hashable.
 * @tparam Value The type of cached data. Managed via std::shared_ptr for safe lifetime.
 * @tparam Cmp Equality comparator for keys (default: std::equal_to<Key>).
 */
template <typename Key, typename Value, typename Cmp = std::equal_to<Key>>
class Cache
{
public:
  /**
   * @brief Type for the miss solver callback.
   *
   * Receives a key and returns a shared_ptr<Value>.
   * If the solver returns nullptr, it is considered a cacheable failure.
   */
  using MissSolver = std::function<std::shared_ptr<Value>(const Key &)>;

  /** @brief Type for key hashing function. */
  using HashFct = std::function<size_t(const Key &)>;

private:
  /**
   * @brief Internal representation of a cache slot.
   *
   * Each entry handles its own synchronization via a mutex and condition variable.
   * This design allows concurrent access to different keys while ensuring that
   * multiple requests for the *same* key wait for a single computation.
   */
  class CacheEntry
  {
    friend class Cache;

    Key _key = Key();
    std::shared_ptr<Value> _value;
    EntryState _state = EntryState::Empty;

    Dlink _dlink_lru;  /**< Embedded link for the LRU doubly-linked list. */

    /**
     * @brief Protects the entry state and value during state transitions.
     *
     * We use a per-entry mutex to avoid holding the global cache lock while
     * running the (potentially slow) miss solver.
     */
    std::mutex _mtx;

    /** @brief Used to signal waiting threads when the miss solver finishes. */
    std::condition_variable _cv;

    std::chrono::time_point<std::chrono::steady_clock> _expiration;

  public:
    CacheEntry() = default;

    explicit CacheEntry(const Key &k) : _key(k) {}

    explicit CacheEntry(Key &&k) : _key(std::move(k)) {}

    CacheEntry(const CacheEntry &other)
      : _key(other._key), _value(other._value), _state(other._state), _expiration(other._expiration)
    {
      // mutex and cv are not copyable; Dlink default-constructed
    }

    CacheEntry(CacheEntry &&other) noexcept
      : _key(std::move(other._key)), _value(std::move(other._value)), _state(other._state),
        _expiration(other._expiration)
    {
      // empty
    }

    CacheEntry &operator=(const CacheEntry &other)
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

    CacheEntry &operator=(CacheEntry &&other) noexcept
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

    bool operator==(const CacheEntry &other) const
    {
      return Cmp()(_key, other._key);
    }

    const Key &key() const noexcept
    {
      return _key;
    }

    Dlink *link_lru() noexcept
    {
      return &_dlink_lru;
    }

    std::mutex &mtx() noexcept
    {
      return _mtx;
    }

    std::condition_variable &cv() noexcept
    {
      return _cv;
    }

    EntryState state() const noexcept
    {
      return _state;
    }

    void set_state(EntryState s) noexcept
    {
      _state = s;
    }

    const std::shared_ptr<Value> &value() const noexcept
    {
      return _value;
    }

    void set_value(std::shared_ptr<Value> v) noexcept
    {
      _value = std::move(v);
    }

    void clear_value() noexcept
    {
      _value.reset();
    }

    bool has_expired(const std::chrono::steady_clock::time_point &now) const noexcept
    {
      return now > _expiration;
    }

    void set_expiration(const std::chrono::steady_clock::time_point &exp) noexcept
    {
      _expiration = exp;
    }

    // Reset entry to Empty state for reuse
    void reset() noexcept
    {
      _state = EntryState::Empty;
      _value.reset();
    }

    void swap(CacheEntry &other) noexcept
    {
      using std::swap;
      swap(_key, other._key);
      swap(_value, other._value);
      swap(_state, other._state);
      swap(_expiration, other._expiration);
    }

  };  // end class CacheEntry

  // ================================================================
  // Comparator adapter for hash table
  // ================================================================
  struct EntryCmp
  {
    bool operator()(const CacheEntry &a, const CacheEntry &b) const
    {
      return Cmp()(a.key(), b.key());
    }
  };

  // ================================================================
  // Data members
  // ================================================================

  Dlink _lru_list;   /**< Sentinel for the LRU list (head is MRU, tail is LRU). */
  size_t _capacity;  /**< Maximum number of entries allowed. */

  using HashTable = OLhashTable<CacheEntry, EntryCmp>;
  HashTable _table;  /**< Hash table for O(1) key lookups. */

  std::chrono::seconds _positive_ttl; /**< Validity duration for successful results. */
  std::chrono::seconds _negative_ttl; /**< Validity duration for failed results. */

  /**
   * @brief Global mutex protecting the hash table structure and LRU list.
   *
   * This mutex is held only for brief structural operations like searches,
   * inserts, or moving an entry to the front of the LRU list. It is never
   * held while the user's miss solver is running.
   */
  mutable std::mutex _mtx;

  MissSolver _solver; /**< User-provided callback to fetch missing values. */

  CacheStats _stats;  /**< Thread-safe statistics (requires _mtx for updates). */

  // ================================================================
  // LRU helpers (must be called with _mtx held)
  // ================================================================

  LINKNAME_TO_TYPE(CacheEntry, _dlink_lru);

  /**
   * @brief Adds a new entry to the most-recently-used (MRU) position.
   * @note Requires _mtx to be held by the caller.
   */
  void lru_insert_as_mru(CacheEntry *entry)
  {
    _lru_list.insert(entry->link_lru());
  }

  /**
   * @brief Moves an existing entry to the MRU position.
   * @note Requires _mtx to be held by the caller.
   */
  void lru_move_to_mru(CacheEntry *entry)
  {
    entry->link_lru()->del();
    _lru_list.insert(entry->link_lru());
  }

  /**
   * @brief Safely detaches an entry from the LRU list.
   * @note Requires _mtx to be held by the caller.
   */
  static void lru_remove(CacheEntry *entry)
  {
    entry->link_lru()->del();
  }

  /**
   * @brief Identifies the best candidate for eviction.
   *
   * We walk the LRU list starting from the oldest entry. Crucially, we skip any
   * entry that is currently Computing. This prevents "saturation" issues where
   * an entry we are currently waiting on is evicted before it can finish.
   *
   * @return A pointer to the victim entry, or nullptr if all entries are busy.
   * @note Requires _mtx to be held by the caller.
   */
  CacheEntry *find_evictable_lru() const
  {
    for (Dlink *p = _lru_list.get_prev(); p != const_cast<Dlink *>(&_lru_list); p = p->get_prev())
      {
        CacheEntry *entry = _dlink_lru_to_CacheEntry(p);
        if (entry->state() != EntryState::Computing)
          return entry;
      }
    return nullptr;
  }

  /**
   * @brief Remove entry from hash table and LRU list.
   * @note Requires _mtx to be held by the caller.
   */
  void remove_entry(CacheEntry *entry)
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
  static typename HashTable::Hash_Fct make_entry_hash(HashFct key_hash)
  {
    return [key_hash = std::move(key_hash)](const CacheEntry &entry) -> size_t
    {
      return key_hash(entry.key());
    };
  }

  // Default hash function for Key using Aleph's dft_hash_fct
  static size_t default_key_hash(const Key &key) noexcept
  {
    return Aleph::dft_hash_fct(key);
  }

  /**
   * @brief High-level lookup/insertion logic with eviction support.
   *
   * This is the only place where the cache structure (hash table and LRU) is modified.
   *
   * @param key The key to find or reserve.
   * @return {entry_ptr, existed} where existed=true means the key was already present.
   *         Returns {nullptr, false} if the cache is full and cannot evict (saturated).
   * @note Requires _mtx to be held by the caller.
   */
  std::pair<CacheEntry *, bool> contains_or_insert_locked(const Key &key)
  {
    assert(_table.size() <= _capacity);

    // First try: the key might already be in the table
    CacheEntry *existing = _table.search(CacheEntry(key));
    if (existing != nullptr)
      {
        // On hit, we move it to MRU to refresh its LRU priority
        lru_move_to_mru(existing);
        return {existing, true};
      }

    // Key not found -- we need to insert. Evict first if at capacity.
    if (_table.size() == _capacity)
      {
        CacheEntry *victim = find_evictable_lru();
        if (victim == nullptr)
          {
            // Critical case: all entries are currently Computing.
            // We cannot evict them because it would break the single-flight guarantee.
            return {nullptr, false};
          }
        remove_entry(victim);
      }

    assert(_table.size() < _capacity);

    // Reserve the slot in the hash table
    auto [entry_ptr, existed] = _table.contains_or_insert(CacheEntry(key));

    if (entry_ptr == nullptr)
      return {nullptr, false};  // Hash table internal failure (e.g. out of memory)

    // Should not already exist as we searched for it just above
    assert(not existed);

    lru_insert_as_mru(entry_ptr);
    return {entry_ptr, false};
  }

  /**
   * @brief Checks if a cache entry is currently usable.
   */
  static bool is_valid_hit(CacheEntry *entry, const std::chrono::steady_clock::time_point &now)
  {
    // Invalidated entries must be treated as misses
    if (entry->state() == EntryState::Invalidated)
      return false;

    // Empty entries are transition states that need computation
    if (entry->state() == EntryState::Empty)
      return false;

    // Computing entries are hits! The caller just needs to wait for completion.
    if (entry->state() == EntryState::Computing)
      return true;

    // For Ready or Failed entries, we must check if the TTL has expired.
    return not entry->has_expired(now);
  }

  /**
   * @brief Handles coordination for keys found in the hash table.
   *
   * If the entry is valid but Computing, this method blocks until the solver finishes.
   * If the entry is expired, it clears it to allow re-computation.
   */
  std::optional<CacheResult<Value>> resolve_hit(CacheEntry *entry,
                                                const std::chrono::steady_clock::time_point &now)
  {
    // Per-entry lock ensures we don't block other keys while waiting for this one.
    std::unique_lock entry_lock(entry->mtx());

    if (not is_valid_hit(entry, now))
      {
        // Entry is no longer valid: prepare it for a fresh solver run.
        entry->set_state(EntryState::Empty);
        entry->clear_value();
        return std::nullopt;
      }

    // Single-flight: if another thread is already computing, wait for it.
    if (entry->state() == EntryState::Computing)
      {
        entry->cv().wait(entry_lock,
                         [entry]
        {
          return entry->state() != EntryState::Computing;
        });
      }

    // Now that computation is finished (or was already finished), check outcome.
    if (entry->state() == EntryState::Ready)
      {
        std::scoped_lock glock(_mtx);
        ++_stats.hits;
        return CacheResult<Value>::hit_positive(entry->value());
      }

    if (entry->state() == EntryState::Failed)
      {
        std::scoped_lock glock(_mtx);
        ++_stats.negative_hits;
        return CacheResult<Value>::hit_negative();
      }

    // Edge case: entry was invalidated or reset while we were waiting.
    return std::nullopt;
  }

  /**
   * @brief Executes the miss solver and updates the cache entry.
   *
   * This method implements the single-flight logic by marking the entry as
   * Computing before dropping the lock and running the solver.
   */
  CacheResult<Value> resolve_miss(CacheEntry *entry, const std::chrono::steady_clock::time_point &now)
  {
    std::unique_lock entry_lock(entry->mtx());

    // Double-check: another thread might have started computing between our initial
    // check and acquiring the entry lock (Double-Checked Locking pattern).
    if (entry->state() == EntryState::Computing)
      {
        entry->cv().wait(entry_lock,
                         [entry]
        {
          return entry->state() != EntryState::Computing;
        });

        // Re-check after waiting
        if (entry->state() == EntryState::Ready)
          return CacheResult<Value>::hit_positive(entry->value());

        if (entry->state() == EntryState::Failed)
          return CacheResult<Value>::hit_negative();
      }

    // If entry is already Ready/Failed (e.g., race where two threads saw miss,
    // one finished while the other was still entering resolve_miss).
    if (entry->state() == EntryState::Ready)
      return CacheResult<Value>::hit_positive(entry->value());

    if (entry->state() == EntryState::Failed)
      return CacheResult<Value>::hit_negative();

    // We are the winning thread: mark as Computing and release lock before calling solver.
    // Releasing the lock is vital to allow other threads to access other keys.
    entry->set_state(EntryState::Computing);
    entry_lock.unlock();

    std::shared_ptr<Value> result;
    bool solver_succeeded = false;
    try
      {
        // Run user code without holding any mutexes to avoid deadlocks
        // and allow maximum concurrency.
        result = _solver(entry->key());
        solver_succeeded = (result != nullptr);
      }
    catch (...)
      {
        // Exceptions from solver are treated as negative results (cached failures)
        solver_succeeded = false;
      }

    entry_lock.lock();

    // Fix: recapture completion time to ensure TTL starts from the moment
    // the value is available, not from the moment the lookup started.
    const auto completion_time = std::chrono::steady_clock::now();

    CacheResult<Value> cache_result = solver_succeeded
                                      ? CacheResult<Value>::computed_positive(result)
                                      : CacheResult<Value>::computed_negative();

    if (solver_succeeded)
      {
        entry->set_value(std::move(result));
        entry->set_state(EntryState::Ready);
        entry->set_expiration(completion_time + _positive_ttl);
      }
    else
      {
        entry->clear_value();
        entry->set_state(EntryState::Failed);
        entry->set_expiration(completion_time + _negative_ttl);
      }

    entry_lock.unlock();

    // Structural update: move the refreshed/new entry to MRU position.
    {
      std::scoped_lock glock(_mtx);
      lru_move_to_mru(entry);
      ++_stats.misses;
    }

    // Signal all waiting threads that the value is now available.
    entry->cv().notify_all();

    return cache_result;
  }

public:
  static constexpr float table_ratio = 1.3f;

  /**
   * @brief Constructs a new Cache.
   *
   * @param capacity     Maximum number of entries. Must be > 0.
   * @param positive_ttl Time-to-live for successful (non-null) results.
   * @param negative_ttl Time-to-live for failed (null) results.
   * @param solver       Callable invoked on cache misses to compute values.
   * @param hash_fct     Hash function for keys (default: Aleph::dft_hash_fct).
   */
  Cache(size_t capacity,
        const std::chrono::seconds &positive_ttl,
        const std::chrono::seconds &negative_ttl,
        MissSolver solver,
        HashFct hash_fct = default_key_hash)
    : _capacity(capacity), _table(static_cast<size_t>(table_ratio * capacity),
                                  make_entry_hash(hash_fct),
                                  EntryCmp(),
                                  hash_default_lower_alpha,
                                  hash_default_upper_alpha,
                                  false),
      _positive_ttl(positive_ttl), _negative_ttl(negative_ttl), _solver(std::move(solver))
  {
    assert(capacity > 0);
  }

  /// @name Non-copyable/Non-movable
  /// Cache contains mutexes and condition variables which are not copyable or movable.
  /// @{
  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;
  Cache(Cache &&) = delete;
  Cache &operator=(Cache &&) = delete;
  /// @}

  /**
   * @brief Primary API for obtaining a value from the cache.
   *
   * 1. Looks up the key in the hash table.
   * 2. If the key exists and is valid (not expired/invalidated), returns the result.
   * 3. If the key is being computed by another thread, waits for it.
   * 4. If the key is missing or expired, executes the miss solver and caches the result.
   *
   * Ensures that for any given key, only one solver is running at a time.
   *
   * @param key The key to look up.
   * @return A CacheResult containing the value or failure status.
   */
  CacheResult<Value> get_or_compute(const Key &key)
  {
    std::pair<CacheEntry *, bool> p;
    {
      // First phase: structural check with global lock
      std::scoped_lock glock(_mtx);
      p = contains_or_insert_locked(key);
    }

    CacheEntry *entry = p.first;
    const bool existed = p.second;

    if (entry == nullptr)
      {
        // Handle cache saturation: all slots are busy with long-running computations.
        {
          std::scoped_lock glock(_mtx);
          ++_stats.saturations;
        }
        return CacheResult<Value>::saturated();
      }

    const auto now = std::chrono::steady_clock::now();

    if (existed)
      {
        // Try to handle as a hit (handles waiting if Computing)
        if (auto hit_result = resolve_hit(entry, now); hit_result.has_value())
          return hit_result.value();
      }

    // Otherwise, it's a new entry or an expired one: resolve as a miss
    return resolve_miss(entry, now);
  }

  /**
   * @brief Performs a non-blocking lookup for an existing value.
   *
   * This method will NOT trigger a miss solver if the key is missing.
   * If the key is currently being computed by another thread, it WILL wait
   * for that computation to finish.
   *
   * @param key The key to look up.
   * @return The result if found and valid, or std::nullopt on miss.
   */
  std::optional<CacheResult<Value>> find(const Key &key)
  {
    CacheEntry *entry;
    {
      std::scoped_lock glock(_mtx);
      entry = _table.search(CacheEntry(key));
      if (entry == nullptr)
        return std::nullopt;
      lru_move_to_mru(entry);
    }

    const auto now = std::chrono::steady_clock::now();
    std::unique_lock entry_lock(entry->mtx());

    // Check validity before potentially waiting
    if (not is_valid_hit(entry, now))
      return std::nullopt;

    // Wait for ongoing computations (single-flight semantics)
    if (entry->state() == EntryState::Computing)
      {
        entry->cv().wait(entry_lock,
                         [entry]
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

  /**
   * @brief Marks an entry as invalid without immediate physical removal.
   *
   * Logical invalidation is faster than physical removal and allows the LRU
   * policy to decide when to actually reuse the slot.
   *
   * @param key The key to invalidate.
   * @return true if the key was found and invalidated; false otherwise.
   */
  bool invalidate(const Key &key)
  {
    CacheEntry *entry;
    {
      std::scoped_lock glock(_mtx);
      entry = _table.search(CacheEntry(key));
      if (entry == nullptr)
        return false;
    }

    std::scoped_lock entry_lock(entry->mtx());

    // Safety: don't invalidate entries while they are being computed
    if (entry->state() == EntryState::Computing)
      return false;

    if (entry->state() == EntryState::Invalidated or entry->state() == EntryState::Empty)
      return false;

    entry->set_state(EntryState::Invalidated);
    entry->set_expiration(std::chrono::steady_clock::now());

    {
      std::scoped_lock glock(_mtx);
      // Move to tail (LRU) to ensure it's among the first to be evicted
      Dlink *link = entry->link_lru();
      link->del();
      _lru_list.append(link);
    }

    return true;
  }

  /**
   * @brief Refreshes the TTL of a valid entry and moves it to MRU.
   *
   * @param key The key to touch.
   * @return true if the key was found, valid, and refreshed; false otherwise.
   */
  bool touch(const Key &key)
  {
    CacheEntry *entry;
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

  /**
   * @brief Fast check for key presence and validity.
   * @param key The key to check.
   * @return true if the key is present and currently valid.
   */
  bool has(const Key &key)
  {
    CacheEntry *entry;
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
  // Queries
  // =============================================================

  /** @brief Returns the current number of entries in the cache. */
  size_t size() const
  {
    std::scoped_lock glock(const_cast<std::mutex &>(_mtx));
    return _table.size();
  }

  /** @brief Returns the maximum capacity of the cache. */
  size_t capacity() const noexcept
  {
    return _capacity;
  }

  /** @brief Returns a snapshot of the cache statistics. */
  CacheStats stats() const
  {
    std::scoped_lock glock(const_cast<std::mutex &>(_mtx));
    return _stats;
  }

};  // end class Cache

}  // namespace CppCache

#endif  // CACHE_CACHE_H
