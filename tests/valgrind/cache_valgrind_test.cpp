//
// cache_valgrind_test.cpp -- Valgrind-oriented concurrency test suite
//
// Designed for Helgrind (data races), DRD (data races + lock contention),
// and Memcheck (use-after-free, leaks from refcount bugs).
//
// NOT part of CI — run manually via scripts/run_valgrind.rb or directly:
//
//   valgrind --tool=helgrind  ./build-v2/cache_valgrind_test
//   valgrind --tool=drd       ./build-v2/cache_valgrind_test
//   valgrind --tool=memcheck --leak-check=full ./build-v2/cache_valgrind_test
//
// Thread counts and iteration counts are intentionally low to keep
// wall-clock time reasonable under Valgrind's ~20-50× slowdown.
//

#include <atomic>
#include <chrono>
#include <future>
#include <random>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include <cache/cache.H>

using namespace std;
using namespace std::chrono;
using namespace std::chrono_literals;
using namespace CppCache;

// ================================================================
// Helpers
// ================================================================

static constexpr int T = 8;   // thread count (moderate for Valgrind)
static constexpr int R = 20;  // rounds per thread

/// Barrier-like synchronization: all threads start together.
static void spin_until(const atomic<bool> &flag)
{
  while (not flag.load(memory_order_acquire))
    this_thread::yield();
}

// ================================================================
// 1. Thundering herd — many threads request the same key
//
// Exercises: single-flight wait/notify, cv, refcount acquire/release,
//            _state atomic transitions (Computing -> Ready).
// ================================================================

TEST(ValgrindConcurrency, thundering_herd_same_key)
{
  Cache<int, int> cache(8, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(50us);
      return make_shared<int>(key * 10);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  for (int i = 0; i < T; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          auto result = cache.get_or_compute(42);
          EXPECT_TRUE(result.is_positive());
          EXPECT_NE(result.value(), nullptr);
          EXPECT_EQ(*result.value(), 420);
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}

// ================================================================
// 2. Eviction pressure — more keys than capacity
//
// Exercises: find_evictable_lru, _state atomic read, refcount check,
//            LRU list manipulation under global _mtx, entry deletion.
// ================================================================

TEST(ValgrindConcurrency, eviction_pressure)
{
  constexpr int CAPACITY = 4;
  constexpr int NUM_KEYS = 12;

  Cache<int, int> cache(CAPACITY, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(10us);
      return make_shared<int>(key);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  for (int i = 0; i < T; ++i)
    threads.emplace_back([&, i]
    {
      mt19937 rng(i * 31 + 7);
      uniform_int_distribution<int> dist(1, NUM_KEYS);
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          int key = dist(rng);
          auto result = cache.get_or_compute(key);
          // Might be saturated under extreme pressure
          if (result.is_positive())
            EXPECT_EQ(*result.value(), key);
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}

// ================================================================
// 3. Concurrent invalidate + get_or_compute
//
// Exercises: invalidate() vs resolve_hit/resolve_miss race,
//            lru_move_to_mru guard against invalidated entries,
//            _state transitions under entry mutex.
// ================================================================

TEST(ValgrindConcurrency, invalidate_while_computing)
{
  Cache<int, int> cache(8, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(30us);
      return make_shared<int>(key * 10);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  // Compute threads
  for (int i = 0; i < T / 2; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          int key = r % 4 + 1;
          auto result = cache.get_or_compute(key);
          if (result.is_positive())
            EXPECT_EQ(*result.value(), key * 10);
        }
    });

  // Invalidate threads
  for (int i = 0; i < T / 2; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          int key = r % 4 + 1;
          cache.invalidate(key);
          this_thread::yield();
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}

// ================================================================
// 4. Concurrent find + get_or_compute
//
// Exercises: find() waiting on Computing entries, find() returning
//            nullopt for missing keys, refcount management across
//            find path, lru_move_to_mru after entry_lock release.
// ================================================================

TEST(ValgrindConcurrency, find_and_compute_interleaved)
{
  Cache<int, int> cache(8, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(20us);
      return make_shared<int>(key * 10);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  for (int i = 0; i < T / 2; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.get_or_compute(r % 6 + 1);
    });

  for (int i = 0; i < T / 2; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          auto result = cache.find(r % 6 + 1);
          if (result.has_value() and result->is_positive())
            EXPECT_EQ(*result->value(), (r % 6 + 1) * 10);
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}

// ================================================================
// 5. Concurrent touch + get_or_compute + invalidate
//
// Exercises: touch() TTL refresh under entry mutex, touch() acquiring
//            global _mtx for MRU promotion, three-way race on same keys.
// ================================================================

TEST(ValgrindConcurrency, touch_compute_invalidate_three_way)
{
  Cache<int, int> cache(6, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(10us);
      return make_shared<int>(key);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  // Compute
  for (int i = 0; i < 3; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.get_or_compute(r % 4 + 1);
    });

  // Touch
  for (int i = 0; i < 3; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.touch(r % 4 + 1);
    });

  // Invalidate
  for (int i = 0; i < 2; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.invalidate(r % 4 + 1);
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}

// ================================================================
// 6. Concurrent peek (non-blocking) + get_or_compute
//
// Exercises: try_to_lock path in peek(), nullopt for busy entries,
//            _state read without waiting, refcount across peek.
// ================================================================

TEST(ValgrindConcurrency, peek_during_computation)
{
  Cache<int, int> cache(8, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(40us);
      return make_shared<int>(key * 10);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  for (int i = 0; i < T / 2; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.get_or_compute(r % 6 + 1);
    });

  for (int i = 0; i < T / 2; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          auto result = cache.peek(r % 6 + 1);
          if (result.has_value() and result->is_positive())
            EXPECT_EQ(*result->value(), (r % 6 + 1) * 10);
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}

// ================================================================
// 7. TTL expiry race — short TTL causes recomputation mid-flight
//
// Exercises: has_expired() check under entry mutex, re-computation
//            of expired entries, resolve_hit detecting stale entry
//            and falling through to resolve_miss.
// ================================================================

TEST(ValgrindConcurrency, ttl_expiry_recompute_race)
{
  atomic<int> compute_count{0};

  Cache<int, int> cache(8, 1s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      compute_count.fetch_add(1, memory_order_relaxed);
      this_thread::sleep_for(10us);
      return make_shared<int>(key * 10);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  for (int i = 0; i < T; ++i)
    threads.emplace_back([&]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          auto result = cache.get_or_compute(1);
          if (result.is_positive())
            EXPECT_EQ(*result.value(), 10);
          // Force a full idle gap that exceeds the 1s TTL so the
          // next get_or_compute(1) must recompute at least once.
          if (r % 5 == 0)
            this_thread::sleep_for(1100ms);
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();

  // At least one recomputation must have happened
  EXPECT_GE(compute_count.load(), 2);
}

// ================================================================
// 8. Saturation — all entries in Computing state
//
// Exercises: saturation fast-path (_computing_count), saturated()
//            result, no eviction of Computing entries.
// ================================================================

TEST(ValgrindConcurrency, saturation_all_computing)
{
  constexpr int CAPACITY = 3;
  atomic<bool> solvers_may_finish{false};
  atomic<int> solvers_entered{0};

  Cache<int, int> cache(CAPACITY, 30s, 10s,
    [&](const int &key) -> shared_ptr<int>
    {
      solvers_entered.fetch_add(1, memory_order_release);
      // Block until released
      while (not solvers_may_finish.load(memory_order_acquire))
        this_thread::sleep_for(100us);
      return make_shared<int>(key);
    });

  // Fill all slots with blocked computations
  vector<future<CacheResult<int>>> blockers;
  for (int i = 1; i <= CAPACITY; ++i)
    blockers.push_back(async(launch::async, [&cache, i]
    {
      return cache.get_or_compute(i);
    }));

  // Wait deterministically until all solvers have entered
  while (solvers_entered.load(memory_order_acquire) < CAPACITY)
    this_thread::sleep_for(100us);

  // Now try a different key — should saturate
  vector<future<CacheResult<int>>> saturators;
  for (int i = 0; i < 4; ++i)
    saturators.push_back(async(launch::async, [&cache]
    {
      return cache.get_or_compute(999);
    }));

  for (auto &f : saturators)
    {
      auto result = f.get();
      EXPECT_TRUE(result.is_saturated());
    }

  solvers_may_finish.store(true, memory_order_release);
  for (auto &f : blockers)
    {
      auto result = f.get();
      EXPECT_TRUE(result.is_positive());
    }
}

// ================================================================
// 9. Solver exception — concurrent waiters see Failed
//
// Exercises: exception handling in resolve_miss, cv notification
//            after exception, state transition Computing -> Failed,
//            concurrent waiters returning hit_negative.
// ================================================================

TEST(ValgrindConcurrency, solver_exception_concurrent_waiters)
{
  atomic<int> call_count{0};

  Cache<int, int> cache(8, 30s, 10s,
    [&](const int &key) -> shared_ptr<int>
    {
      int n = call_count.fetch_add(1, memory_order_relaxed);
      this_thread::sleep_for(20us);
      if (n == 0)
        throw runtime_error("intentional solver failure");
      return make_shared<int>(key * 10);
    });

  atomic<bool> go{false};
  vector<future<CacheResult<int>>> futures;

  for (int i = 0; i < T; ++i)
    futures.push_back(async(launch::async, [&]
    {
      spin_until(go);
      return cache.get_or_compute(1);
    }));

  go.store(true, memory_order_release);

  int computed_negative = 0;
  int hit_negative = 0;
  for (auto &f : futures)
    {
      auto status = f.wait_for(10s);
      ASSERT_EQ(status, future_status::ready)
        << "Thread hung waiting for solver failure to propagate";

      auto result = f.get();
      ASSERT_TRUE(result.is_negative())
        << "Solver exception should produce a negative result for all waiters";

      if (result.origin() == ResultOrigin::ComputedNegative)
        ++computed_negative;
      else if (result.origin() == ResultOrigin::HitNegative)
        ++hit_negative;
    }

  EXPECT_EQ(call_count.load(), 1);
  EXPECT_EQ(computed_negative, 1);
  EXPECT_EQ(hit_negative, T - 1);

  // The failed entry should remain cached and visible via non-refreshing APIs.
  EXPECT_TRUE(cache.has(1));
  auto cached = cache.peek(1);
  ASSERT_TRUE(cached.has_value());
  EXPECT_TRUE(cached->is_negative());
  EXPECT_TRUE(cached->is_hit());
  EXPECT_EQ(cached->origin(), ResultOrigin::HitNegative);

  // Still within the configured 10s negative TTL, so the cached failure
  // should remain valid and get_or_compute() must not reinvoke the solver.
  this_thread::sleep_for(1s);
  EXPECT_TRUE(cache.has(1));
  auto still_cached = cache.peek(1);
  ASSERT_TRUE(still_cached.has_value());
  EXPECT_TRUE(still_cached->is_negative());

  auto retry = cache.get_or_compute(1);
  EXPECT_TRUE(retry.is_negative());
  EXPECT_TRUE(retry.is_hit());
  EXPECT_EQ(retry.origin(), ResultOrigin::HitNegative);
  EXPECT_EQ(call_count.load(), 1);
}

// ================================================================
// 10. Negative caching — solver returns nullptr
//
// Exercises: null shared_ptr → Failed state, negative TTL path,
//            hit_negative for subsequent requests, refcount on
//            entries with no value.
// ================================================================

TEST(ValgrindConcurrency, negative_caching_concurrent)
{
  Cache<int, int> cache(8, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(10us);
      if (key % 2 == 0)
        return nullptr;  // negative result
      return make_shared<int>(key);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  for (int i = 0; i < T; ++i)
    threads.emplace_back([&, i]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          int key = (r + i) % 6 + 1;
          auto result = cache.get_or_compute(key);
          if (key % 2 == 0)
            EXPECT_TRUE(result.is_negative());
          else
            {
              EXPECT_TRUE(result.is_positive());
              EXPECT_EQ(*result.value(), key);
            }
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}

// ================================================================
// 11. Full API storm — all operations on overlapping keys
//
// Exercises: every concurrent path simultaneously.
// This is the most aggressive test for Helgrind/DRD.
// ================================================================

TEST(ValgrindConcurrency, full_api_storm)
{
  constexpr int CAPACITY = 6;
  constexpr int NUM_KEYS = 10;

  Cache<int, int> cache(CAPACITY, 5s, 2s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(15us);
      if (key == 7)
        return nullptr;  // negative for key 7
      return make_shared<int>(key * 100);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  // get_or_compute threads
  for (int i = 0; i < 3; ++i)
    threads.emplace_back([&, i]
    {
      mt19937 rng(i);
      uniform_int_distribution<int> dist(1, NUM_KEYS);
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.get_or_compute(dist(rng));
    });

  // find threads
  for (int i = 0; i < 2; ++i)
    threads.emplace_back([&, i]
    {
      mt19937 rng(100 + i);
      uniform_int_distribution<int> dist(1, NUM_KEYS);
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.find(dist(rng));
    });

  // peek threads
  for (int i = 0; i < 2; ++i)
    threads.emplace_back([&, i]
    {
      mt19937 rng(200 + i);
      uniform_int_distribution<int> dist(1, NUM_KEYS);
      spin_until(go);
      for (int r = 0; r < R; ++r)
        cache.peek(dist(rng));
    });

  // invalidate threads
  threads.emplace_back([&]
  {
    mt19937 rng(300);
    uniform_int_distribution<int> dist(1, NUM_KEYS);
    spin_until(go);
    for (int r = 0; r < R; ++r)
      cache.invalidate(dist(rng));
  });

  // touch threads
  threads.emplace_back([&]
  {
    mt19937 rng(400);
    uniform_int_distribution<int> dist(1, NUM_KEYS);
    spin_until(go);
    for (int r = 0; r < R; ++r)
      cache.touch(dist(rng));
  });

  // has threads
  threads.emplace_back([&]
  {
    mt19937 rng(500);
    uniform_int_distribution<int> dist(1, NUM_KEYS);
    spin_until(go);
    for (int r = 0; r < R; ++r)
      cache.has(dist(rng));
  });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();

  // Sanity: stats should be internally consistent. 
  // Since we performed many requests, at least one of these must be > 0.
  auto s = cache.stats();
  EXPECT_GT(s.hits + s.misses + s.negative_hits + s.saturations, 0u);
  EXPECT_LE(cache.size(), static_cast<size_t>(CAPACITY));
}

// ================================================================
// 12. Refcount stress — eviction while threads hold references
//
// Exercises: TOCTOU refcount safety, EntryGuard RAII, use-after-free
//            detection (primary Memcheck target).
// ================================================================

TEST(ValgrindConcurrency, refcount_eviction_stress)
{
  constexpr int CAPACITY = 3;

  Cache<int, int> cache(CAPACITY, 30s, 10s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(10us);
      return make_shared<int>(key);
    });

  atomic<bool> go{false};
  vector<thread> threads;

  // Threads that compute a key and hold the result briefly
  for (int i = 0; i < T; ++i)
    threads.emplace_back([&, i]
    {
      spin_until(go);
      for (int r = 0; r < R; ++r)
        {
          // Use many more keys than capacity to force eviction
          int key = i * R + r;
          auto result = cache.get_or_compute(key);
          // Hold the shared_ptr briefly while other threads evict
          if (result.is_positive())
            {
              this_thread::sleep_for(5us);
              EXPECT_EQ(*result.value(), key);
            }
        }
    });

  go.store(true, memory_order_release);
  for (auto &t : threads) t.join();
}
