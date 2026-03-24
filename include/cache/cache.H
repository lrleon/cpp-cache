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
// Reuses Aleph-w: LhashTableVtl for hash table, Dlink for LRU list
//

#ifndef CACHE_CACHE_H
#define CACHE_CACHE_H

#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>

#include <aleph.H>
#include <dlink.H>
#include <tpl_lhash.H>

#include <cache/cache_result.H>
#include <cache/entry_state.H>

namespace CppCache {

/**
 * @brief Statistics for cache performance monitoring.
 */
struct CacheStats
{
  size_t hits = 0;          /**< Successful lookups found in cache. */
  size_t misses = 0;        /**< Lookups that required computing the value. */
  size_t negative_hits = 0; /**< Successful lookups of previously cached failures. */
  size_t evictions = 0;     /**< Number of entries evicted by LRU policy. */
  size_t saturations = 0;   /**< Failures due to all entries being in Computing state. */
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
 * @warning **Value Thread-Safety Responsibility:** The cache stores values via `shared_ptr<Value>`,
 * which can be accessed concurrently by multiple threads. If `Value` is mutable, the user must
 * ensure thread-safety either by: (1) making `Value` internally thread-safe (e.g., with internal
 * mutexes), or (2) treating it as immutable after caching. The cache does NOT protect against data
 * races on the `Value` itself—only the cache structure is thread-safe.
 *
 * @tparam Key The type used for lookup. Must be equality-comparable and hashable.
 * @tparam Value The type of cached data. Managed via std::shared_ptr for safe lifetime.
 * @tparam Cmp Equality comparator for keys (default: std::equal_to<Key>).
 */
template <typename Key, typename Value, typename Cmp = std::equal_to<Key>, typename... Args>
  requires std::equality_comparable<Key>
class Cache
{
public:
  /**
   * @brief Type for the miss solver callback.
   *
   * Receives a key and returns a shared_ptr<Value>.
   * If the solver returns nullptr, it is considered a cacheable failure.
   */
  using MissSolver = std::function<std::shared_ptr<Value>(const Key &, Args...)>;

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
  class CacheEntry : public Aleph::LhashBucketVtl<Key>
  {
    friend class Cache;

    using BucketBase = Aleph::LhashBucketVtl<Key>;

    std::shared_ptr<Value> _value;
    std::atomic<EntryState> _state{EntryState::Empty};

    Dlink _dlink_lru; /**< Embedded link for the LRU doubly-linked list. */

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

    /**
     * @brief Reference count for safe pointer lifetime across lock boundaries.
     *
     * Starts at 1 (table ownership). Incremented when a thread acquires a
     * pointer under _mtx, decremented when done. Entry is deleted when the
     * count reaches zero. This eliminates the TOCTOU window where an entry
     * could be evicted while a thread operates on it without _mtx held.
     */
    std::atomic<unsigned> _refcount{1};

  public:
    CacheEntry() = delete;

    explicit CacheEntry(const Key &k) : BucketBase(k) {}

    explicit CacheEntry(Key &&k) : BucketBase(std::move(k)) {}

    // Non-copyable, non-movable (contains mutex, cv, atomic refcount)
    CacheEntry(const CacheEntry &) = delete;
    CacheEntry &operator=(const CacheEntry &) = delete;
    CacheEntry(CacheEntry &&) = delete;
    CacheEntry &operator=(CacheEntry &&) = delete;

    ~CacheEntry() override = default;

    /** @brief Increment refcount (must be called under _mtx). */
    void acquire() noexcept
    {
      _refcount.fetch_add(1, std::memory_order_relaxed);
    }

    /** @brief Decrement refcount. Returns true if it reached zero (caller must delete). */
    bool release() noexcept
    {
      return _refcount.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }

    const Key &key() const noexcept
    {
      return this->get_key();
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
      return _state.load(std::memory_order_acquire);
    }

    void set_state(EntryState s) noexcept
    {
      _state.store(s, std::memory_order_release);
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

  };  // end class CacheEntry

  // ================================================================
  // Data members
  // ================================================================

  Dlink _lru_list;  /**< Sentinel for the LRU list (head is MRU, tail is LRU). */
  size_t _capacity; /**< Maximum number of entries allowed. */

  using HashTable = Aleph::LhashTableVtl<Key, Cmp>;
  using Bucket = typename HashTable::Bucket;
  HashTable _table; /**< Hash table for O(1) key lookups. */

  /**
   * @brief RAII guard that releases an entry's refcount on scope exit.
   *
   * If the refcount reaches zero, the entry is deleted. This ensures
   * entries outlive any thread operating on them even after eviction.
   */
  struct EntryGuard
  {
    CacheEntry *entry;
    explicit EntryGuard(CacheEntry *e) noexcept : entry(e) {}
    ~EntryGuard()
    {
      if (entry and entry->release())
        delete entry;
    }
    EntryGuard(const EntryGuard &) = delete;
    EntryGuard &operator=(const EntryGuard &) = delete;
  };

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

  alignas(64) std::atomic<size_t> _stat_hits{0};
  alignas(64) std::atomic<size_t> _stat_misses{0};
  alignas(64) std::atomic<size_t> _stat_negative_hits{0};
  alignas(64) std::atomic<size_t> _stat_evictions{0};
  alignas(64) std::atomic<size_t> _stat_saturations{0};

  alignas(64) std::atomic<size_t> _computing_count{0}; /**< Fast track for saturation check (audit fix 3.4). */

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
      if (CacheEntry *entry = _dlink_lru_to_CacheEntry(p);
          entry->state() != EntryState::Computing and
          entry->_refcount.load(std::memory_order_acquire) <= 1)
        return entry;
    return nullptr;
  }

  /**
   * @brief Remove entry from hash table and LRU list.
   *
   * Releases the table's ownership reference. If no thread holds
   * an active reference, the entry is deleted immediately.
   *
   * @note Requires _mtx to be held by the caller.
   */
  void remove_entry(CacheEntry *entry)
  {
    lru_remove(entry);
    _table.remove(entry);
    _stat_evictions.fetch_add(1, std::memory_order_relaxed);
    if (entry->release())
      delete entry;
  }

  // ================================================================
  // Hash table helpers
  // ================================================================

  /** @brief Cast a table bucket to CacheEntry. */
  static CacheEntry *to_entry(Bucket *bucket) noexcept
  {
    return static_cast<CacheEntry *>(bucket);
  }

  /**
   * @brief Search by key — no temporary CacheEntry construction needed.
   *
   * This eliminates the audit bug 3.1 pattern where a temporary CacheEntry
   * (with its mutex and condition_variable) was constructed just for lookup.
   */
  CacheEntry *search_entry(const Key &key) const
  {
    return to_entry(_table.search(key));
  }

  // Default hash function for Key using Aleph's dft_hash_fct
  static size_t default_key_hash(const Key &key) noexcept
  {
    return Aleph::dft_hash_fct(key);
  }

  // Placeholder hash for constructor — immediately replaced by set_hash_fct.
  // Avoids instantiating dft_hash_ptr_fct<Key> for types without Aleph hashing.
  static size_t placeholder_hash(const Key &) noexcept
  {
    return 0;
  }

  /**
   * @brief High-level lookup/insertion logic with eviction support.
   *
   * This is the only place where the cache structure (hash table and LRU) is modified.
   * The returned entry has its refcount incremented for the caller.
   *
   * @param key The key to find or reserve.
   * @return {entry_ptr, existed} where existed=true means the key was already present.
   *         Returns {nullptr, false} if the cache is full and cannot evict (saturated).
   * @note Requires _mtx to be held by the caller.
   */
  std::pair<CacheEntry *, bool> contains_or_insert_locked(const Key &key)
  {
    assert(_table.size() <= _capacity);

    // Search by key — no temporary CacheEntry needed (fixes audit bug 3.1)
    CacheEntry *existing = search_entry(key);
    if (existing != nullptr)
      {
        lru_move_to_mru(existing);
        existing->acquire();
        return {existing, true};
      }

    // Key not found -- we need to insert. Evict first if at capacity.
    if (_table.size() == _capacity)
      {
        if (_computing_count >= _capacity)
          return {nullptr, false};  // Fast path: avoid O(n) scan when fully saturated (audit fix 3.4)

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

    // Heap-allocate the new entry (refcount starts at 1 for table ownership)
    auto *entry = new CacheEntry(key);
    auto *inserted = _table.insert(entry);

    if (inserted == nullptr)
      {
        delete entry;
        return {nullptr, false};
      }

    // Mark as Computing immediately so the entry is never evictable
    // between insertion and the solver run (fixes audit bug 1.1).
    entry->set_state(EntryState::Computing);
    ++_computing_count;

    // Acquire for caller (refcount: 1 table + 1 caller = 2)
    entry->acquire();

    lru_insert_as_mru(entry);
    return {entry, false};
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
      entry->cv().wait(entry_lock,
                       [entry]
      {
        return entry->state() != EntryState::Computing;
      });

    // Now that computation is finished (or was already finished), check outcome.
    if (entry->state() == EntryState::Ready)
      {
        // Refresh TTL on hit (fixes audit design issue 4.1)
        entry->set_expiration(std::chrono::steady_clock::now() + _positive_ttl);

        _stat_hits.fetch_add(1, std::memory_order_relaxed);
        return CacheResult<Value>::hit_positive(entry->value());
      }

    if (entry->state() == EntryState::Failed)
      {
        entry->set_expiration(std::chrono::steady_clock::now() + _negative_ttl);

        _stat_negative_hits.fetch_add(1, std::memory_order_relaxed);
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
  CacheResult<Value> resolve_miss(CacheEntry *entry,
                                  const std::chrono::steady_clock::time_point &now,
                                  const bool owner,
                                  Args... args)
  {
    std::unique_lock entry_lock(entry->mtx());

    if (not owner)
      {
        // Double-check: another thread might have started computing between our initial
        // check and acquiring the entry lock (Double-Checked Locking pattern).
        if (entry->state() == EntryState::Computing)
          {
            entry->cv().wait(entry_lock,
                             [entry]
            {
              return entry->state() != EntryState::Computing;
            });

            // Re-check after waiting — count these as hits since the value came from cache
            if (entry->state() == EntryState::Ready)
              {
                _stat_hits.fetch_add(1, std::memory_order_relaxed);
                return CacheResult<Value>::hit_positive(entry->value());
              }

            if (entry->state() == EntryState::Failed)
              {
                _stat_negative_hits.fetch_add(1, std::memory_order_relaxed);
                return CacheResult<Value>::hit_negative();
              }
          }

        // If entry is already Ready/Failed (e.g., race where two threads saw miss,
        // one finished while the other was still entering resolve_miss).
        if (entry->state() == EntryState::Ready)
          {
            _stat_hits.fetch_add(1, std::memory_order_relaxed);
            return CacheResult<Value>::hit_positive(entry->value());
          }

        if (entry->state() == EntryState::Failed)
          {
            _stat_negative_hits.fetch_add(1, std::memory_order_relaxed);
            return CacheResult<Value>::hit_negative();
          }

        // We are the winning thread: mark as Computing and release lock before calling solver.
        // Releasing the lock is vital to allow other threads to access other keys.
        entry->set_state(EntryState::Computing);

        // _computing_count is an atomic hint. Here it's updated outside the global _mtx
        // so a small imprecision is acceptable. This atomic value is only used for the 
        // saturation fast-path check.
        ++_computing_count;
      }

    // owner == true: entry is already Computing (set in contains_or_insert_locked
    // under global lock, fixing audit bug 1.1). Skip straight to solver.
    entry_lock.unlock();

    std::shared_ptr<Value> result;
    bool solver_succeeded = false;
    try
      {
        // Run user code without holding any mutexes to avoid deadlocks
        // and allow maximum concurrency.
        result = _solver(entry->key(), std::forward<Args>(args)...);
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

    --_computing_count;

    // Signal all waiting threads that the value is now available.
    // Done while holding the entry lock to ensure waiters wake up immediately
    // after unlock, without being delayed by the global _mtx acquisition (audit fix 3.2).
    entry->cv().notify_all();

    entry_lock.unlock();

    // Structural update: move the refreshed/new entry to MRU position.
    // Guard against a concurrent invalidate() that may have occurred after we
    // released entry_lock — only promote terminal (Ready/Failed) entries.
    {
      std::scoped_lock glock(_mtx);
      auto s = entry->state();
      if (s == EntryState::Ready or s == EntryState::Failed)
        lru_move_to_mru(entry);
    }
    _stat_misses.fetch_add(1, std::memory_order_relaxed);

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
  Cache(const size_t capacity,
        const std::chrono::seconds &positive_ttl,
        const std::chrono::seconds &negative_ttl,
        MissSolver solver,
        HashFct hash_fct = default_key_hash)
    : _capacity(capacity), _table(static_cast<size_t>(table_ratio * capacity),
                                  placeholder_hash,
                                  Cmp(),
                                  0.0f,    // lower_alpha — never shrink
                                  10.0f,   // upper_alpha — effectively disable resize trigger
                                  true,    // remove_all_buckets — table deletes remaining entries
                                  false),  // with_resize — preallocated, no dynamic resize
      _positive_ttl(positive_ttl), _negative_ttl(negative_ttl), _solver(std::move(solver))
  {
    assert(capacity > 0);
    _table.set_hash_fct(std::move(hash_fct));
  }

  /**
   * @brief Destructor.
   *
   * @warning The Cache must not be destroyed while any thread is calling
   * get_or_compute(), find(), or any other member function. Destroying
   * the cache during active operations results in undefined behavior.
   * The caller is responsible for ensuring all operations have completed
   * (e.g., via thread join or barrier) before destruction.
   */
  ~Cache()
  {
#ifndef NDEBUG
    std::scoped_lock glock(_mtx);
    for (typename HashTable::Iterator it(_table); it.has_curr(); it.next_ne())
      {
        auto *entry = static_cast<CacheEntry *>(it.get_curr_ne());
        // Table owns 1 ref. Anything > 1 means an active external reference!
        assert(entry->_refcount.load(std::memory_order_relaxed) == 1 and
               "Cache destroyed with active external operations in flight!");
      }
#endif
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
   * @param args Additional arguments to pass to the miss solver if computation is needed.
   * @return A CacheResult containing the value or failure status.
   */
  CacheResult<Value> get_or_compute(const Key &key, Args... args)
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
        _stat_saturations.fetch_add(1, std::memory_order_relaxed);
        return CacheResult<Value>::saturated();
      }

    // RAII guard releases caller's refcount when this method returns.
    // If the entry was evicted while we operated on it, the guard
    // will delete it once no other thread holds a reference.
    EntryGuard guard(entry);

    const auto now = std::chrono::steady_clock::now();

    if (existed)
      {
        // Try to handle as a hit (handles waiting if Computing)
        if (auto hit_result = resolve_hit(entry, now); hit_result.has_value())
          return hit_result.value();
      }

    // For new entries (!existed): we are the owner — entry is already Computing
    // (set in contains_or_insert_locked under global lock).
    // For expired entries (existed): entry was reset to Empty by resolve_hit.
    return resolve_miss(entry, now, !existed, std::forward<Args>(args)...);
  }

  /**
   * @brief Simplified API that returns only the value (nullable).
   *
   * Invokes get_or_compute internally but discards the origin metadata.
   * This is the recommended method for standard usage where telemetry is not needed.
   *
   * @param key The key to look up.
   * @param args Optional arguments to forward to the miss solver.
   * @return A valid shared_ptr if the result is positive, or nullptr if the result
   *         is negative, failed, or the cache is saturated.
   */
  std::shared_ptr<Value> get(const Key &key, Args... args)
  {
    return get_or_compute(key, std::forward<Args>(args)...).value();
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
      entry = search_entry(key);
      if (entry == nullptr)
        return std::nullopt;
      // Do NOT move to MRU here: we must first confirm the entry is
      // valid. Moving unconditionally would undo invalidate()'s LRU
      // tail placement and allow stale entries to survive eviction.
      entry->acquire();
    }

    EntryGuard guard(entry);

    const auto now = std::chrono::steady_clock::now();
    std::unique_lock entry_lock(entry->mtx());

    // Check validity before potentially waiting
    if (not is_valid_hit(entry, now))
      return std::nullopt;

    // Wait for ongoing computations (single-flight semantics)
    if (entry->state() == EntryState::Computing)
      entry->cv().wait(entry_lock,
                       [entry]
      {
        return entry->state() != EntryState::Computing;
      });

    if (entry->state() == EntryState::Ready)
      {
        // Refresh TTL on hit (fixes audit design issue 4.1)
        entry->set_expiration(std::chrono::steady_clock::now() + _positive_ttl);

        auto val = entry->value();
        entry_lock.unlock();

        {
          std::scoped_lock glock(_mtx);
          // Guard against concurrent invalidate() after entry_lock was released
          auto s = entry->state();
          if (s == EntryState::Ready or s == EntryState::Failed)
            lru_move_to_mru(entry);
        }
        return CacheResult<Value>::hit_positive(val);
      }

    if (entry->state() == EntryState::Failed)
      {
        // Refresh TTL on hit for negative values too
        entry->set_expiration(std::chrono::steady_clock::now() + _negative_ttl);

        entry_lock.unlock();

        {
          std::scoped_lock glock(_mtx);
          // Guard against concurrent invalidate() after entry_lock was released
          auto s = entry->state();
          if (s == EntryState::Ready or s == EntryState::Failed)
            lru_move_to_mru(entry);
        }
        return CacheResult<Value>::hit_negative();
      }

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
      entry = search_entry(key);
      if (entry == nullptr)
        return false;
      entry->acquire();
    }

    EntryGuard guard(entry);

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
   * @brief Refreshes the TTL of a valid entry (Ready or Failed) and moves it to MRU.
   *
   * @param key The key to touch.
   * @return true if the key was found, valid, and refreshed; false otherwise.
   */
  bool touch(const Key &key)
  {
    CacheEntry *entry;
    {
      std::scoped_lock glock(_mtx);
      entry = search_entry(key);
      if (entry == nullptr)
        return false;
      entry->acquire();
    }

    EntryGuard guard(entry);

    auto now = std::chrono::steady_clock::now();
    std::scoped_lock entry_lock(entry->mtx());

    if (entry->state() != EntryState::Ready and entry->state() != EntryState::Failed)
      return false;

    if (entry->has_expired(now))
      return false;

    if (entry->state() == EntryState::Ready)
      entry->set_expiration(now + _positive_ttl);
    else
      entry->set_expiration(now + _negative_ttl);

    {
      std::scoped_lock glock(_mtx);
      lru_move_to_mru(entry);
    }

    return true;
  }

  /**
   * @brief Truly non-blocking inspection of the cache.
   *
   * Unlike find(), this method never blocks. If the entry is being computed,
   * it returns std::nullopt immediately.
   *
   * @param key The key to peek at.
   * @return The result if found and Ready/Failed, or std::nullopt if missing,
   *         expired, or Computing.
   */
  std::optional<CacheResult<Value>> peek(const Key &key)
  {
    CacheEntry *entry;
    {
      std::scoped_lock glock(_mtx);
      entry = search_entry(key);
      if (entry == nullptr)
        return std::nullopt;
      entry->acquire();
    }

    EntryGuard guard(entry);

    auto now = std::chrono::steady_clock::now();
    std::unique_lock entry_lock(entry->mtx(), std::try_to_lock);

    if (not entry_lock.owns_lock())
      return std::nullopt; // Busy entry, don't wait

    if (not is_valid_hit(entry, now))
      return std::nullopt;

    if (entry->state() == EntryState::Ready)
      return CacheResult<Value>::hit_positive(entry->value());

    if (entry->state() == EntryState::Failed)
      return CacheResult<Value>::hit_negative();

    return std::nullopt;
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
      entry = search_entry(key);
      if (entry == nullptr)
        return false;
      entry->acquire();
    }

    EntryGuard guard(entry);

    auto now = std::chrono::steady_clock::now();
    std::scoped_lock entry_lock(entry->mtx());

    // has() reports only terminal/ready states — Computing is intentionally excluded
    // so it stays consistent with peek() and does not treat in-flight entries as hits.
    auto s = entry->state();
    if (s != EntryState::Ready and s != EntryState::Failed)
      return false;
    return not entry->has_expired(now);
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
    return CacheStats{
      _stat_hits.load(std::memory_order_relaxed),
      _stat_misses.load(std::memory_order_relaxed),
      _stat_negative_hits.load(std::memory_order_relaxed),
      _stat_evictions.load(std::memory_order_relaxed),
      _stat_saturations.load(std::memory_order_relaxed)
    };
  }

};  // end class Cache

}  // namespace CppCache

#endif  // CACHE_CACHE_H
