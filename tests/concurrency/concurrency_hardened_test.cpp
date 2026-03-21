//
// concurrency_hardened_test.cpp -- Hardened concurrency tests for cpp-cache v2
//
// These tests target concurrency bugs, race conditions, deadlocks,
// and design deficiencies identified in the audit.
//
// Tests marked [BUG x.y] reference audit sections.
// Tests marked [DEADLOCK] may hang if the bug triggers.
// Run with -DSANITIZE=thread for systematic race/lock-order detection.
//

# include <atomic>
# include <chrono>
# include <future>
# include <map>
# include <mutex>
# include <random>
# include <thread>
# include <vector>
# include <gtest/gtest.h>

# include <cache/cache.H>

using namespace std;
using namespace std::chrono;
using namespace CppCache;

// ================================================================
// Section 1: Saturation (all entries Computing)
// ================================================================

TEST(SaturationTest, all_computing_returns_valid_result)
{
  // When all entries are Computing, get_or_compute should still
  // return a valid result by computing directly (bypass mode).

  atomic<bool> may_finish{false};
  atomic<int> solver_calls{0};

  Cache<int, int> cache(2, 20s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_calls.fetch_add(1);
      if (key <= 2)
        {
          while (!may_finish.load(memory_order_acquire))
            this_thread::sleep_for(100us);
        }
      return make_shared<int>(key * 10);
    });

  // Fill both slots with slow Computing entries
  auto f1 = async(launch::async, [&]() { return cache.get_or_compute(1); });
  auto f2 = async(launch::async, [&]() { return cache.get_or_compute(2); });

  // Wait for both solvers to start
  while (solver_calls.load() < 2)
    this_thread::sleep_for(100us);

  // All slots are Computing. key=3 should return Saturated.
  auto r3 = cache.get_or_compute(3);

  EXPECT_TRUE(r3.is_saturated())
    << "When all entries are Computing, get_or_compute should return Saturated";
  EXPECT_EQ(r3.value(), nullptr);

  may_finish.store(true, memory_order_release);
  auto r1 = f1.get();
  auto r2 = f2.get();

  EXPECT_TRUE(r1.is_positive());
  EXPECT_TRUE(r2.is_positive());
}

// Saturation returns explicit Saturated result (no silent fallback)
TEST(SaturationTest, all_threads_get_saturated_result)
{
  atomic<bool> may_finish{false};
  atomic<int> total_solver_calls{0};

  Cache<int, int> cache(2, 20s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      total_solver_calls.fetch_add(1);
      while (!may_finish.load(memory_order_acquire))
        this_thread::sleep_for(100us);
      return make_shared<int>(key * 10);
    });

  // Fill cache with slow Computing entries
  auto f1 = async(launch::async, [&]() { return cache.get_or_compute(1); });
  auto f2 = async(launch::async, [&]() { return cache.get_or_compute(2); });

  while (total_solver_calls.load() < 2)
    this_thread::sleep_for(100us);

  // Launch 10 threads all requesting key=3 under saturation
  constexpr int N = 10;
  vector<future<CacheResult<int>>> futures;
  for (int i = 0; i < N; ++i)
    futures.push_back(async(launch::async, [&]()
    {
      return cache.get_or_compute(3);
    }));

  for (auto &f : futures)
    {
      auto r = f.get();
      EXPECT_TRUE(r.is_saturated())
        << "All threads should get Saturated result under full saturation";
    }

  // Solver should NOT have been called for key=3 (no silent fallback)
  EXPECT_EQ(total_solver_calls.load(), 2)
    << "Solver should only be called for keys 1 and 2, not for saturated key=3";

  may_finish.store(true, memory_order_release);
  f1.get();
  f2.get();

  // Verify saturations counter
  EXPECT_GE(cache.stats().saturations, static_cast<size_t>(N));
}

// ================================================================
// Section 2: TOCTOU - Empty entry eviction [BUG 1.1]
// ================================================================

// [BUG 1.1] New entry inserted with Empty state can be evicted
// before resolve_miss marks it Computing.
TEST(TOCTOUBug, empty_entry_evictable_stress)
{
  // Stress test: capacity=2, one Computing entry (non-evictable),
  // race multiple threads inserting new keys simultaneously.
  // The second thread may evict the first thread's Empty entry.
  //
  // The symptom: solver called with wrong key, or wrong value returned.
  //
  // Under ThreadSanitizer, the data race would be detected even if
  // the symptom doesn't manifest.

  constexpr int ITERATIONS = 200;

  for (int iter = 0; iter < ITERATIONS; ++iter)
    {
      atomic<bool> key1_started{false};
      atomic<bool> key1_may_finish{false};

      Cache<int, int> cache(2, 20s, 1s,
        [&](const int &key) -> shared_ptr<int>
        {
          if (key == 1)
            {
              key1_started.store(true, memory_order_release);
              while (!key1_may_finish.load(memory_order_acquire))
                this_thread::sleep_for(50us);
            }
          return make_shared<int>(key * 10);
        });

      // Start slow computation for key=1 (Computing, not evictable)
      auto f1 = async(launch::async, [&]()
      {
        return cache.get_or_compute(1);
      });

      while (!key1_started.load(memory_order_acquire))
        this_thread::sleep_for(50us);

      // Race: two threads insert key=2 and key=3 simultaneously.
      // Cache has capacity=2: key=1 (Computing), one empty slot.
      // First thread gets the empty slot (Empty state).
      // Second thread needs to evict: key=1 is Computing, the first
      // thread's entry is Empty → evictable! Bug 1.1.
      auto f2 = async(launch::async, [&]()
      {
        return cache.get_or_compute(2);
      });
      auto f3 = async(launch::async, [&]()
      {
        return cache.get_or_compute(3);
      });

      key1_may_finish.store(true, memory_order_release);

      auto r1 = f1.get();
      auto r2 = f2.get();
      auto r3 = f3.get();

      ASSERT_TRUE(r1.is_positive());
      EXPECT_EQ(*r1.value(), 10) << "Iteration " << iter;

      // Under saturation one of r2/r3 may get Saturated (no slot available)
      if (r2.is_positive())
        EXPECT_EQ(*r2.value(), 20) << "Iteration " << iter
          << ": Bug 1.1 may have caused wrong key→value mapping";
      else
        EXPECT_TRUE(r2.is_saturated()) << "Iteration " << iter;

      if (r3.is_positive())
        EXPECT_EQ(*r3.value(), 30) << "Iteration " << iter
          << ": Bug 1.1 may have caused wrong key→value mapping";
      else
        EXPECT_TRUE(r3.is_saturated()) << "Iteration " << iter;
    }
}

// ================================================================
// Section 3: TTL Race Conditions (was Section 4)
// ================================================================

TEST(TTLRaceTest, concurrent_ttl_expiry_recompute)
{
  // Many threads access the same key right as its TTL expires.
  // All should get valid results. At most one should recompute.

  atomic<int> solver_calls{0};

  Cache<int, int> cache(5, 1s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_calls.fetch_add(1);
      this_thread::sleep_for(50ms);
      return make_shared<int>(key * 10);
    });

  // Initial computation
  cache.get_or_compute(1);
  ASSERT_EQ(solver_calls.load(), 1);

  // Wait until TTL is about to expire
  this_thread::sleep_for(950ms);

  // Launch many threads right around TTL expiry
  constexpr int N = 20;
  vector<future<CacheResult<int>>> futures;

  // Stagger launches across the expiry boundary
  for (int i = 0; i < N; ++i)
    {
      futures.push_back(async(launch::async, [&]()
      {
        this_thread::sleep_for(chrono::milliseconds(i * 10));
        return cache.get_or_compute(1);
      }));
    }

  for (auto &f : futures)
    {
      auto r = f.get();
      ASSERT_TRUE(r.is_positive());
      ASSERT_EQ(*r.value(), 10);
    }

  // After TTL expiry, exactly one recomputation should happen
  // (single-flight on the recompute). Some threads may hit the
  // old cached value before expiry.
  EXPECT_LE(solver_calls.load(), 3)
    << "Excessive solver calls during TTL expiry race. "
       "Expected at most a few recomputations.";
}

// ================================================================
// Section 5: Exception Safety Under Concurrency
// ================================================================

TEST(ConcurrentExceptionTest, solver_exception_waiters_get_negative)
{
  // When the solver throws while other threads wait on the CV,
  // all waiters should get a negative result (not hang forever).

  atomic<bool> solver_started{false};

  Cache<int, int> cache(5, 20s, 5s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_started.store(true, memory_order_release);
      this_thread::sleep_for(200ms);
      throw runtime_error("solver crashed");
    });

  constexpr int N = 5;
  vector<future<CacheResult<int>>> futures;

  for (int i = 0; i < N; ++i)
    futures.push_back(async(launch::async, [&]()
    {
      return cache.get_or_compute(1);
    }));

  // Wait for at least one solver to start
  while (!solver_started.load(memory_order_acquire))
    this_thread::sleep_for(100us);

  // All threads should eventually complete (not hang)
  for (auto &f : futures)
    {
      auto status = f.wait_for(10s);
      ASSERT_EQ(status, future_status::ready)
        << "Thread hung waiting for solver exception to propagate";

      auto r = f.get();
      EXPECT_TRUE(r.is_negative())
        << "Solver exception should produce negative result for all waiters";
    }
}

TEST(ConcurrentExceptionTest, entry_usable_after_exception)
{
  // After a solver exception, the entry should be in Failed state
  // and usable (not stuck in Computing forever).

  atomic<int> call_count{0};
  bool should_throw = true;

  Cache<int, int> cache(5, 20s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      call_count.fetch_add(1);
      if (should_throw)
        {
          this_thread::sleep_for(100ms);
          throw runtime_error("fail");
        }
      return make_shared<int>(key * 10);
    });

  // First call: exception → Failed
  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(r1.is_negative());

  // Cached negative hit (within negative TTL)
  auto r2 = cache.get_or_compute(1);
  EXPECT_TRUE(r2.is_negative());
  EXPECT_TRUE(r2.is_hit());

  // Wait for negative TTL to expire
  this_thread::sleep_for(1100ms);

  // Fix solver
  should_throw = false;

  // Should recompute successfully
  auto r3 = cache.get_or_compute(1);
  EXPECT_TRUE(r3.is_positive());
  EXPECT_EQ(*r3.value(), 10);
}

// ================================================================
// Section 6: find() Blocking Behavior
// ================================================================

TEST(FindBlockingTest, find_blocks_on_computing_entry)
{
  // find() is documented as "Non-computing lookup" but actually
  // blocks on Computing entries (waits on CV). [Bug 2.3 / design issue]
  //
  // This test verifies the blocking behavior.

  atomic<bool> solver_started{false};
  atomic<bool> solver_may_finish{false};

  Cache<int, int> cache(5, 20s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_started.store(true, memory_order_release);
      while (!solver_may_finish.load(memory_order_acquire))
        this_thread::sleep_for(1ms);
      return make_shared<int>(key * 10);
    });

  // Start computation in background
  auto compute_future = async(launch::async, [&]()
  {
    return cache.get_or_compute(1);
  });

  while (!solver_started.load(memory_order_acquire))
    this_thread::sleep_for(100us);

  // find() should block until computation finishes
  auto find_future = async(launch::async, [&]()
  {
    return cache.find(1);
  });

  // find() should NOT have returned yet (entry is Computing)
  auto status = find_future.wait_for(200ms);
  EXPECT_EQ(status, future_status::timeout)
    << "find() should block on Computing entries (not return immediately)";

  // Let solver finish
  solver_may_finish.store(true, memory_order_release);

  auto compute_result = compute_future.get();
  ASSERT_TRUE(compute_result.is_positive());

  auto find_result = find_future.get();
  ASSERT_TRUE(find_result.has_value());
  EXPECT_TRUE(find_result->is_positive());
  EXPECT_EQ(*find_result->value(), 10);
}

// ================================================================
// Section 7: Mixed Concurrent Operations
// ================================================================

TEST(MixedConcurrentTest, invalidate_then_recompute)
{
  // Thread A invalidates, thread B recomputes the same key.

  atomic<int> solver_calls{0};

  Cache<int, int> cache(5, 20s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_calls.fetch_add(1);
      return make_shared<int>(key * 10);
    });

  // Populate
  cache.get_or_compute(1);
  ASSERT_EQ(solver_calls.load(), 1);

  for (int round = 0; round < 100; ++round)
    {
      // Invalidate and recompute concurrently
      thread t1([&]() { cache.invalidate(1); });
      thread t2([&]() { cache.get_or_compute(1); });

      t1.join();
      t2.join();

      // After both threads finish, the entry should be valid
      auto r = cache.get_or_compute(1);
      ASSERT_TRUE(r.is_positive());
      ASSERT_EQ(*r.value(), 10);
    }
}

TEST(MixedConcurrentTest, concurrent_find_and_invalidate)
{
  Cache<int, int> cache(5, 20s, 1s,
    [](const int &key) -> shared_ptr<int>
    {
      return make_shared<int>(key * 10);
    });

  for (int round = 0; round < 200; ++round)
    {
      cache.get_or_compute(1);

      thread t1([&]()
      {
        auto r = cache.find(1);
        // May or may not find it (depends on invalidation timing)
        if (r.has_value())
          EXPECT_TRUE(r->is_positive());
      });

      thread t2([&]() { cache.invalidate(1); });

      t1.join();
      t2.join();

      // Restore for next round
      cache.get_or_compute(1);
    }
}

TEST(MixedConcurrentTest, concurrent_touch_and_get)
{
  Cache<int, int> cache(5, 20s, 1s,
    [](const int &key) -> shared_ptr<int>
    {
      return make_shared<int>(key * 10);
    });

  cache.get_or_compute(1);

  for (int round = 0; round < 200; ++round)
    {
      thread t1([&]() { cache.touch(1); });
      thread t2([&]()
      {
        auto r = cache.get_or_compute(1);
        ASSERT_TRUE(r.is_positive());
        ASSERT_EQ(*r.value(), 10);
      });

      t1.join();
      t2.join();
    }
}

TEST(MixedConcurrentTest, all_operations_mixed)
{
  // Hammer the cache with all operations concurrently.
  // Capacity is set above the key range to avoid frequent evictions,
  // which can trigger the known TOCTOU bug (audit 1.2) with inline
  // entries. A dedicated TOCTOU stress test covers that scenario.

  Cache<int, int> cache(30, 5s, 2s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(1ms);
      if (key % 7 == 0) return nullptr; // Some negatives
      return make_shared<int>(key * 10);
    });

  constexpr int NUM_THREADS = 30;
  constexpr int OPS_PER_THREAD = 50;

  vector<thread> threads;

  for (int t = 0; t < NUM_THREADS; ++t)
    {
      threads.emplace_back([&, t]()
      {
        mt19937 rng(t * 42 + 7);
        uniform_int_distribution<int> key_dist(1, 15);
        uniform_int_distribution<int> op_dist(0, 4);

        for (int op = 0; op < OPS_PER_THREAD; ++op)
          {
            int key = key_dist(rng);
            switch (op_dist(rng))
              {
                case 0: // get_or_compute
                  {
                    auto r = cache.get_or_compute(key);
                    ASSERT_TRUE(r.is_positive() || r.is_negative() || r.is_saturated());
                    if (r.is_positive())
                      ASSERT_EQ(*r.value(), key * 10);
                    break;
                  }
                case 1: // find
                  cache.find(key);
                  break;
                case 2: // has
                  cache.has(key);
                  break;
                case 3: // touch
                  cache.touch(key);
                  break;
                case 4: // invalidate
                  cache.invalidate(key);
                  break;
                // Note: remove() was eliminated (lazy-only invalidation)
              }
          }
      });
    }

  for (auto &th : threads)
    th.join();

  // Basic sanity: cache size should not exceed capacity
  EXPECT_LE(cache.size(), cache.capacity());
}

// ================================================================
// Section 8: Stale TTL [BUG 2.1]
// ================================================================

// [BUG 2.1] TTL calculated from lookup time, not solver completion time
TEST(StaleTTLBug, ttl_effectively_shorter_for_slow_solver)
{
  // `now` is captured BEFORE the solver runs. With a slow solver,
  // the TTL starts ticking before the value is available, making
  // the effective TTL shorter than configured.
  //
  // Solver takes ~2s, TTL is 3s.
  // Correct: value valid for 3s after completion (total ~5s from start)
  // Buggy:   value valid for 3s from start = 1s after completion

  Cache<int, int> cache(5, 3s, 1s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(2s);
      return make_shared<int>(key * 10);
    });

  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(r1.is_positive());
  ASSERT_EQ(*r1.value(), 10);

  // Solver just finished (~2s elapsed).
  // Wait 1.5s (well within a 3s TTL from completion, but
  // 3.5s from lookup start, which exceeds the buggy 3s TTL)
  this_thread::sleep_for(1500ms);

  auto r2 = cache.get_or_compute(1);
  EXPECT_TRUE(r2.is_hit())
    << "BUG 2.1: TTL starts from lookup time (before solver). "
       "A 2s solver + 3s TTL gives only ~1s of effective validity "
       "after completion. Value expired prematurely.";
}

// ================================================================
// Section 9: Stats Consistency Under Concurrency
// ================================================================

TEST(ConcurrentStatsTest, stats_consistent)
{
  atomic<int> solver_calls{0};

  Cache<int, int> cache(10, 20s, 10s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_calls.fetch_add(1);
      return make_shared<int>(key * 10);
    });

  constexpr int NUM_THREADS = 20;
  constexpr int KEYS = 5;

  vector<thread> threads;

  for (int t = 0; t < NUM_THREADS; ++t)
    threads.emplace_back([&, t]()
    {
      for (int k = 1; k <= KEYS; ++k)
        cache.get_or_compute(k);
    });

  for (auto &th : threads)
    th.join();

  auto s = cache.stats();

  // Total operations = NUM_THREADS * KEYS = 100
  // Exactly KEYS misses (one per unique key)
  // Remaining are hits or negative_hits
  EXPECT_EQ(s.misses, static_cast<size_t>(KEYS))
    << "Each key should be computed exactly once";

  size_t total = s.hits + s.misses + s.negative_hits;
  EXPECT_EQ(total, static_cast<size_t>(NUM_THREADS * KEYS))
    << "Total stats should equal total operations. "
       "hits=" << s.hits << " misses=" << s.misses
    << " negative_hits=" << s.negative_hits;
}

// ================================================================
// Section 10: Improved Stress Test (fixes rand() thread-safety)
// ================================================================

TEST(StressTest, random_keys_random_delays_threadsafe)
{
  // Replaces the original stress test that used rand() (not thread-safe).
  // Uses std::mt19937 with per-thread seeds.

  Cache<int, int> cache(100, 10s, 1s,
    [](const int &key) -> shared_ptr<int>
    {
      // Thread-safe: no shared state
      mt19937 rng(key * 31 + 17);
      uniform_int_distribution<int> delay(10, 50);
      this_thread::sleep_for(chrono::milliseconds(delay(rng)));
      return make_shared<int>(key * 10);
    });

  constexpr int NUM_THREADS = 50;
  constexpr int NUM_KEYS = 20;

  vector<future<CacheResult<int>>> futures;

  for (int i = 0; i < NUM_THREADS; ++i)
    {
      mt19937 rng(i * 42 + 1);
      uniform_int_distribution<int> key_dist(1, NUM_KEYS);
      int key = key_dist(rng);

      futures.push_back(async(launch::async, [&cache, key]()
      {
        return cache.get_or_compute(key);
      }));
    }

  for (auto &f : futures)
    {
      auto r = f.get();
      ASSERT_TRUE(r.is_positive());
      ASSERT_NE(r.value(), nullptr);
      // Verify value matches key
      int val = *r.value();
      EXPECT_EQ(val % 10, 0) << "Value should be key*10";
    }
}

// ================================================================
// Section 11: Computing Entry Protection
// ================================================================

TEST(ComputingProtectionTest, computing_entry_not_evicted_heavy)
{
  // Stress version: many threads trying to cause evictions while
  // one entry is Computing. The Computing entry must never be evicted.

  for (int round = 0; round < 50; ++round)
    {
      atomic<bool> solver_started{false};
      atomic<bool> solver_may_finish{false};

      Cache<int, int> cache(3, 20s, 1s,
        [&](const int &key) -> shared_ptr<int>
        {
          if (key == 1)
            {
              solver_started.store(true, memory_order_release);
              while (!solver_may_finish.load(memory_order_acquire))
                this_thread::sleep_for(100us);
            }
          return make_shared<int>(key * 10);
        });

      // Start slow computation for key=1
      auto f1 = async(launch::async, [&]()
      {
        return cache.get_or_compute(1);
      });

      while (!solver_started.load(memory_order_acquire))
        this_thread::sleep_for(100us);

      // Rapidly insert many keys to force evictions
      for (int k = 2; k <= 20; ++k)
        cache.get_or_compute(k);

      // Let key=1 finish
      solver_may_finish.store(true, memory_order_release);
      auto r1 = f1.get();

      ASSERT_TRUE(r1.is_positive()) << "Round " << round;
      ASSERT_EQ(*r1.value(), 10) << "Round " << round;

      // key=1 must still be in cache
      EXPECT_TRUE(cache.has(1))
        << "Round " << round
        << ": Computing entry was evicted! This violates a core invariant.";
    }
}

// ================================================================
// Section 12: Concurrent Negative Caching
// ================================================================

TEST(ConcurrentNegativeTest, negative_with_expiry_and_retry)
{
  // Multiple threads hit a failing key, negative result cached.
  // After negative TTL, retry should call solver again.

  atomic<int> solver_calls{0};
  atomic<bool> should_fail{true};

  Cache<int, int> cache(5, 20s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_calls.fetch_add(1);
      this_thread::sleep_for(50ms);
      if (should_fail.load(memory_order_acquire))
        return nullptr;
      return make_shared<int>(key * 10);
    });

  // Phase 1: Multiple threads get negative result
  {
    constexpr int N = 10;
    vector<future<CacheResult<int>>> futures;
    for (int i = 0; i < N; ++i)
      futures.push_back(async(launch::async, [&]()
      {
        return cache.get_or_compute(1);
      }));

    for (auto &f : futures)
      {
        auto r = f.get();
        ASSERT_TRUE(r.is_negative());
      }

    // Single-flight: solver called exactly once
    EXPECT_EQ(solver_calls.load(), 1);
  }

  // Phase 2: Wait for negative TTL to expire
  this_thread::sleep_for(1100ms);

  // Phase 3: Fix solver, retry
  should_fail.store(false, memory_order_release);

  {
    constexpr int N = 10;
    vector<future<CacheResult<int>>> futures;
    for (int i = 0; i < N; ++i)
      futures.push_back(async(launch::async, [&]()
      {
        return cache.get_or_compute(1);
      }));

    for (auto &f : futures)
      {
        auto r = f.get();
        ASSERT_TRUE(r.is_positive())
          << "After negative TTL expiry and solver fix, should get positive result";
        ASSERT_EQ(*r.value(), 10);
      }

    // Should have called solver exactly once more
    EXPECT_EQ(solver_calls.load(), 2);
  }
}

// ================================================================
// Section 13: RAII lock safety [BUG 2.4]
// ================================================================

// [BUG 2.4] resolve_hit uses manual _mtx.lock()/unlock() instead of
// scoped_lock. If an exception occurred between lock and unlock,
// the mutex would be permanently locked.
//
// This is a code quality issue. We can verify the symptom:
// after a get_or_compute call, the global mutex must be unlocked
// (i.e., other operations must succeed).

TEST(RAIISafetyTest, mutex_not_stuck_after_operations)
{
  Cache<int, int> cache(5, 10s, 5s,
    [](const int &key) -> shared_ptr<int>
    {
      return make_shared<int>(key * 10);
    });

  // Populate and hit (exercises resolve_hit with its manual lock/unlock)
  cache.get_or_compute(1);
  cache.get_or_compute(1); // hit path → resolve_hit

  // If the mutex got stuck, these would deadlock
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_TRUE(cache.has(1));
  EXPECT_EQ(cache.stats().hits, 1u);

  auto r = cache.find(1);
  EXPECT_TRUE(r.has_value());

  cache.invalidate(1);
  cache.get_or_compute(1);

  EXPECT_EQ(cache.size(), 1u);
}

// ================================================================
// Section 14: Concurrent touch() around TTL expiry
// ================================================================

TEST(ConcurrentTouchExpiryTest, touch_during_ttl_expiry)
{
  // Multiple threads call touch() and has() right around TTL expiry.
  // No crash or undefined behavior should occur.

  Cache<int, int> cache(5, 1s, 1s,
    [](const int &key) -> shared_ptr<int>
    {
      return make_shared<int>(key * 10);
    });

  for (int round = 0; round < 20; ++round)
    {
      cache.get_or_compute(1);
      ASSERT_TRUE(cache.has(1));

      // Wait until close to expiry
      this_thread::sleep_for(900ms);

      // Launch threads that race around the expiry boundary
      constexpr int N = 10;
      vector<thread> threads;

      for (int i = 0; i < N; ++i)
        threads.emplace_back([&, i]()
        {
          this_thread::sleep_for(chrono::milliseconds(i * 20));
          cache.touch(1);  // may succeed or fail depending on timing
          cache.has(1);    // same
        });

      for (auto &t : threads)
        t.join();

      // After all threads, ensure cache is still usable
      auto r = cache.get_or_compute(1);
      ASSERT_TRUE(r.is_positive());
      ASSERT_EQ(*r.value(), 10);
    }
}

// ================================================================
// Section 15: invalidate() during TTL expiry
// ================================================================

TEST(ConcurrentInvalidateExpiryTest, invalidate_races_with_expiry_and_recompute)
{
  atomic<int> solver_calls{0};

  Cache<int, int> cache(5, 1s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_calls.fetch_add(1);
      return make_shared<int>(key * 10);
    });

  for (int round = 0; round < 30; ++round)
    {
      cache.get_or_compute(1);

      // Wait until close to expiry
      this_thread::sleep_for(900ms);

      // Race: one thread invalidates, another tries get_or_compute
      thread t1([&]()
      {
        this_thread::sleep_for(100ms); // right around expiry
        cache.invalidate(1);
      });

      thread t2([&]()
      {
        this_thread::sleep_for(100ms);
        auto r = cache.get_or_compute(1);
        ASSERT_TRUE(r.is_positive());
        ASSERT_EQ(*r.value(), 10);
      });

      t1.join();
      t2.join();

      // After the race, ensure entry is consistent
      auto r = cache.get_or_compute(1);
      ASSERT_TRUE(r.is_positive());
      ASSERT_EQ(*r.value(), 10);
    }
}