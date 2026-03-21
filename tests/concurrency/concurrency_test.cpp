//
// concurrency_test.cpp -- Concurrency tests for cpp-cache v2
//

# include <iostream>
# include <thread>
# include <future>
# include <vector>
# include <chrono>
# include <atomic>
# include <mutex>
# include <random>
# include <gtest/gtest.h>

# include <cache/cache.H>

using namespace std;
using namespace std::chrono;
using namespace CppCache;

// ================================================================
// Single-flight: same key, multiple threads
// ================================================================

struct SingleFlightFixture : public testing::Test
{
  atomic<int> solver_call_count{0};

  Cache<int, int> cache;

  SingleFlightFixture()
    : cache(5, 20s, 1s,
            [this](const int & key) -> shared_ptr<int>
            {
              ++solver_call_count;
              this_thread::sleep_for(500ms); // Simulate slow computation
              return make_shared<int>(key * 10);
            })
  {
    // empty
  }
};

TEST_F(SingleFlightFixture, two_threads_same_key)
{
  auto f1 = async(launch::async, [this]()
  {
    return cache.get_or_compute(1);
  });

  auto f2 = async(launch::async, [this]()
  {
    return cache.get_or_compute(1);
  });

  auto r1 = f1.get();
  auto r2 = f2.get();

  ASSERT_TRUE(r1.is_positive());
  ASSERT_TRUE(r2.is_positive());
  ASSERT_EQ(*r1.value(), 10);
  ASSERT_EQ(*r2.value(), 10);

  // Both should point to the same object
  ASSERT_EQ(r1.value().get(), r2.value().get());

  // Solver must have been called exactly once
  ASSERT_EQ(solver_call_count.load(), 1);
}

TEST_F(SingleFlightFixture, many_threads_same_key)
{
  constexpr int N = 20;
  vector<future<CacheResult<int>>> futures;

  for (int i = 0; i < N; ++i)
    futures.push_back(async(launch::async, [this]()
    {
      return cache.get_or_compute(1);
    }));

  vector<CacheResult<int>> results;
  for (auto & f : futures)
    results.push_back(f.get());

  for (auto & r : results)
    {
      ASSERT_TRUE(r.is_positive());
      ASSERT_EQ(*r.value(), 10);
    }

  // All results point to the same object
  for (size_t i = 1; i < results.size(); ++i)
    ASSERT_EQ(results[0].value().get(), results[i].value().get());

  // Solver called exactly once
  ASSERT_EQ(solver_call_count.load(), 1);
}

// ================================================================
// Parallel progress: different keys should not block each other
// ================================================================

struct ParallelProgressFixture : public testing::Test
{
  atomic<int> concurrent_solvers{0};
  atomic<int> max_concurrent{0};

  Cache<int, int> cache;

  ParallelProgressFixture()
    : cache(100, 20s, 1s,
            [this](const int & key) -> shared_ptr<int>
            {
              int cur = ++concurrent_solvers;
              // Track max concurrency
              int prev_max = max_concurrent.load();
              while (cur > prev_max &&
                     !max_concurrent.compare_exchange_weak(prev_max, cur))
                ;
              this_thread::sleep_for(200ms);
              --concurrent_solvers;
              return make_shared<int>(key * 10);
            })
  {
    // empty
  }
};

TEST_F(ParallelProgressFixture, different_keys_progress_in_parallel)
{
  constexpr int NUM_KEYS = 5;
  vector<future<CacheResult<int>>> futures;

  for (int i = 1; i <= NUM_KEYS; ++i)
    futures.push_back(async(launch::async, [this, i]()
    {
      return cache.get_or_compute(i);
    }));

  for (auto & f : futures)
    {
      auto r = f.get();
      ASSERT_TRUE(r.is_positive());
    }

  // At least some solvers should have run concurrently
  // (not strictly guaranteed, but with 200ms sleep and 5 keys, very likely)
  ASSERT_GT(max_concurrent.load(), 1);
}

// ================================================================
// Heavy concurrency: many threads, many keys
// ================================================================

struct HeavyConcurrencyFixture : public testing::Test
{
  Cache<int, int> cache;

  HeavyConcurrencyFixture()
    : cache(5, 20s, 1s,
            [](const int & key) -> shared_ptr<int>
            {
              this_thread::sleep_for(100ms);
              return make_shared<int>(key * 10);
            })
  {
    // empty
  }
};

TEST_F(HeavyConcurrencyFixture, many_threads_many_keys)
{
  constexpr int NUM_KEYS = 5;
  constexpr int THREADS_PER_KEY = 20;

  vector<thread> threads;
  vector<shared_ptr<int>> results(NUM_KEYS * THREADS_PER_KEY);
  mutex results_mutex;

  for (int k = 0; k < NUM_KEYS; ++k)
    for (int t = 0; t < THREADS_PER_KEY; ++t)
      {
        threads.emplace_back(
          [this, k, t, &results, &results_mutex]()
          {
            auto r = cache.get_or_compute(k + 1);
            lock_guard<mutex> lock(results_mutex);
            results[k * THREADS_PER_KEY + t] = r.value();
          });
      }

  for (auto & th : threads)
    th.join();

  ASSERT_EQ(cache.size(), 5u);

  // All threads for the same key must get the same shared_ptr
  for (int k = 0; k < NUM_KEYS; ++k)
    {
      auto expected_val = (k + 1) * 10;
      auto first_ptr = results[k * THREADS_PER_KEY];
      ASSERT_NE(first_ptr, nullptr);
      ASSERT_EQ(*first_ptr, expected_val);

      for (int t = 1; t < THREADS_PER_KEY; ++t)
        {
          ASSERT_EQ(results[k * THREADS_PER_KEY + t].get(),
                    first_ptr.get())
            << "key=" << (k+1) << " thread=" << t;
        }
    }
}

TEST_F(HeavyConcurrencyFixture, repeated_heavy_rounds)
{
  for (int round = 0; round < 10; ++round)
    {
      constexpr int N = 20;
      vector<future<CacheResult<int>>> futures;

      for (int i = 1; i <= 5; ++i)
        for (int j = 0; j < N; ++j)
          futures.push_back(async(launch::async, [this, i]()
          {
            return cache.get_or_compute(i);
          }));

      for (auto & f : futures)
        {
          auto r = f.get();
          ASSERT_TRUE(r.is_positive());
        }

      ASSERT_EQ(cache.size(), 5u);

      for (int i = 1; i <= 5; ++i)
        ASSERT_TRUE(cache.has(i));
    }
}

// ================================================================
// Negative caching under concurrency
// ================================================================

struct ConcurrentNegativeFixture : public testing::Test
{
  atomic<int> solver_calls{0};

  Cache<int, int> cache;

  ConcurrentNegativeFixture()
    : cache(5, 20s, 1s,
            [this](const int &) -> shared_ptr<int>
            {
              ++solver_calls;
              this_thread::sleep_for(300ms);
              return nullptr; // Fail
            })
  {
    // empty
  }
};

TEST_F(ConcurrentNegativeFixture, negative_single_flight)
{
  constexpr int N = 10;
  vector<future<CacheResult<int>>> futures;

  for (int i = 0; i < N; ++i)
    futures.push_back(async(launch::async, [this]()
    {
      return cache.get_or_compute(1);
    }));

  for (auto & f : futures)
    {
      auto r = f.get();
      ASSERT_TRUE(r.is_negative());
    }

  // Solver called exactly once
  ASSERT_EQ(solver_calls.load(), 1);
}

// ================================================================
// Race between compute and invalidate
// ================================================================

TEST(RaceTest, invalidate_during_compute)
{
  atomic<bool> solver_started{false};
  atomic<bool> solver_may_finish{false};

  Cache<int, int> cache(5, 20s, 1s,
    [&](const int & key) -> shared_ptr<int>
    {
      solver_started = true;
      while (!solver_may_finish.load())
        this_thread::sleep_for(10ms);
      return make_shared<int>(key * 10);
    });

  // Start computation in background
  auto f = async(launch::async, [&]()
  {
    return cache.get_or_compute(1);
  });

  // Wait until solver starts
  while (!solver_started.load())
    this_thread::sleep_for(5ms);

  // Try to invalidate while computing -- should fail
  ASSERT_FALSE(cache.invalidate(1));

  // Let solver finish
  solver_may_finish = true;

  auto r = f.get();
  ASSERT_TRUE(r.is_positive());
  ASSERT_EQ(*r.value(), 10);

  // Now invalidation should work
  ASSERT_TRUE(cache.invalidate(1));
  ASSERT_FALSE(cache.has(1));
}

// ================================================================
// Computing entry not evicted
// ================================================================

TEST(EvictionSafetyTest, computing_entry_not_evicted)
{
  atomic<bool> solver_started{false};
  atomic<bool> solver_may_finish{false};

  Cache<int, int> cache(2, 20s, 1s,
    [&](const int & key) -> shared_ptr<int>
    {
      if (key == 1)
        {
          solver_started = true;
          while (!solver_may_finish.load())
            this_thread::sleep_for(10ms);
        }
      return make_shared<int>(key * 10);
    });

  // Start a slow computation for key=1
  auto f = async(launch::async, [&]()
  {
    return cache.get_or_compute(1);
  });

  while (!solver_started.load())
    this_thread::sleep_for(5ms);

  // Fill the cache: key=2 (capacity=2, key=1 is Computing)
  cache.get_or_compute(2);

  // key=3 should evict key=2 (not key=1 which is Computing)
  cache.get_or_compute(3);

  // Let key=1 finish
  solver_may_finish = true;
  auto r = f.get();

  ASSERT_TRUE(r.is_positive());
  ASSERT_EQ(*r.value(), 10);

  // key=1 should still be in cache
  ASSERT_TRUE(cache.has(1));
  // key=2 should have been evicted
  ASSERT_FALSE(cache.has(2));
  ASSERT_TRUE(cache.has(3));
}

// ================================================================
// Stress test: random keys with random delays
// ================================================================

TEST(StressTest, random_keys_random_delays)
{
  Cache<int, int> cache(100, 10s, 1s,
    [](const int & key) -> shared_ptr<int>
    {
      static thread_local std::mt19937 rng(std::random_device{}());
      std::uniform_int_distribution<int> sleep_dist(10, 59);
      this_thread::sleep_for(chrono::milliseconds(sleep_dist(rng)));
      return make_shared<int>(key * 10);
    });

  constexpr int NUM_THREADS = 50;
  constexpr int NUM_KEYS = 20;

  vector<future<CacheResult<int>>> futures;

  for (int i = 0; i < NUM_THREADS; ++i)
    {
      futures.push_back(async(launch::async, [&cache]()
      {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> key_dist(1, NUM_KEYS);
        int key = key_dist(rng);
        return cache.get_or_compute(key);
      }));
    }

  for (auto & f : futures)
    {
      auto r = f.get();
      ASSERT_TRUE(r.is_positive());
      ASSERT_NE(r.value(), nullptr);
    }
}
