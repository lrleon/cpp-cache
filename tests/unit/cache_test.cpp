//
// cache_test.cpp -- Unit tests for cpp-cache v2
//

# include <iostream>
# include <thread>
# include <future>
# include <vector>
# include <chrono>
# include <gtest/gtest.h>

# include <cache/cache.H>

using namespace std;
using namespace std::chrono;
using namespace CppCache;

// ================================================================
// Basic tests
// ================================================================

struct BasicFixture : public testing::Test
{
  static shared_ptr<int> solver(const int & key)
  {
    return make_shared<int>(key * 10);
  }

  Cache<int, int> cache;

  BasicFixture()
    : cache(5, 2s, 1s, solver)
  {
    // empty
  }
};

TEST_F(BasicFixture, empty_cache)
{
  ASSERT_EQ(cache.capacity(), 5u);
  ASSERT_EQ(cache.size(), 0u);
  ASSERT_FALSE(cache.has(1));
}

TEST_F(BasicFixture, get_or_compute_inserts)
{
  auto result = cache.get_or_compute(1);

  ASSERT_TRUE(result.is_positive());
  ASSERT_TRUE(result.was_computed());
  ASSERT_EQ(result.origin(), ResultOrigin::ComputedPositive);
  ASSERT_NE(result.value(), nullptr);
  ASSERT_EQ(*result.value(), 10);
  ASSERT_EQ(cache.size(), 1u);
  ASSERT_TRUE(cache.has(1));
}

TEST_F(BasicFixture, get_or_compute_hit)
{
  auto r1 = cache.get_or_compute(1);
  auto r2 = cache.get_or_compute(1);

  ASSERT_TRUE(r1.was_computed());
  ASSERT_TRUE(r2.is_hit());
  ASSERT_EQ(r2.origin(), ResultOrigin::HitPositive);
  ASSERT_EQ(*r2.value(), 10);

  // Both share the same underlying object
  ASSERT_EQ(r1.value().get(), r2.value().get());
}

TEST_F(BasicFixture, simplified_get_api)
{
  // Test positive get
  auto val1 = cache.get(1);
  ASSERT_NE(val1, nullptr);
  ASSERT_EQ(*val1, 10);

  // Test that subsequent get hits the cache successfully
  auto val2 = cache.get(1);
  ASSERT_NE(val2, nullptr);
  ASSERT_EQ(val1, val2); // should be the exact same shared_ptr
}

TEST_F(BasicFixture, multiple_keys)
{
  for (int i = 1; i <= 5; ++i)
    {
      auto r = cache.get_or_compute(i);
      ASSERT_TRUE(r.is_positive());
      ASSERT_EQ(*r.value(), i * 10);
    }

  ASSERT_EQ(cache.size(), 5u);

  for (int i = 1; i <= 5; ++i)
    ASSERT_TRUE(cache.has(i));
}

TEST_F(BasicFixture, ttl_expiration)
{
  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(cache.has(1));

  // Wait for positive TTL to expire (2s)
  this_thread::sleep_for(2100ms);

  ASSERT_FALSE(cache.has(1));

  // Re-compute after expiration
  auto r2 = cache.get_or_compute(1);
  ASSERT_TRUE(r2.was_computed());
  ASSERT_EQ(*r2.value(), 10);
}

TEST_F(BasicFixture, hit_refreshes_ttl)
{
  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(r1.is_positive());

  // Wait 1.1s (more than half TTL)
  this_thread::sleep_for(1100ms);

  // Hit the cache -> should refresh the TTL to full 2s
  auto r2 = cache.get_or_compute(1);
  ASSERT_TRUE(r2.is_hit());

  // Wait another 1.1s. Total time since first insertion is 2.2s. 
  // If TTL was not refreshed, it would be dead.
  this_thread::sleep_for(1100ms);

  // Should still be valid because the hit refreshed the 2s TTL
  ASSERT_TRUE(cache.has(1));
  
  // Wait enough to finally expire
  this_thread::sleep_for(1000ms);
  ASSERT_FALSE(cache.has(1));
}

TEST_F(BasicFixture, lru_eviction)
{
  for (int i = 1; i <= 5; ++i)
    cache.get_or_compute(i);

  ASSERT_EQ(cache.size(), 5u);

  // Insert one more: key=1 (LRU) should be evicted
  auto r = cache.get_or_compute(6);
  ASSERT_TRUE(r.is_positive());
  ASSERT_EQ(*r.value(), 60);
  ASSERT_EQ(cache.size(), 5u);

  ASSERT_FALSE(cache.has(1)); // evicted
  ASSERT_TRUE(cache.has(6));
}

TEST_F(BasicFixture, lru_touch_prevents_eviction)
{
  for (int i = 1; i <= 5; ++i)
    cache.get_or_compute(i);

  // Touch key=1 so it becomes MRU
  ASSERT_TRUE(cache.touch(1));

  // Insert key=6: key=2 (now LRU) should be evicted, not key=1
  cache.get_or_compute(6);

  ASSERT_TRUE(cache.has(1));   // was touched, not evicted
  ASSERT_FALSE(cache.has(2));  // was LRU, evicted
  ASSERT_TRUE(cache.has(6));
}

TEST_F(BasicFixture, find_existing)
{
  cache.get_or_compute(1);

  auto r = cache.find(1);
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r->is_positive());
  ASSERT_EQ(*r->value(), 10);
}

TEST_F(BasicFixture, find_nonexistent)
{
  auto r = cache.find(999);
  ASSERT_FALSE(r.has_value());
}

TEST_F(BasicFixture, invalidate_entry)
{
  cache.get_or_compute(1);
  ASSERT_TRUE(cache.has(1));

  ASSERT_TRUE(cache.invalidate(1));
  ASSERT_FALSE(cache.has(1));

  // Re-compute should work
  auto r = cache.get_or_compute(1);
  ASSERT_TRUE(r.was_computed());
  ASSERT_EQ(*r.value(), 10);
}

TEST_F(BasicFixture, value_mutation_shared)
{
  auto r1 = cache.get_or_compute(1);
  ASSERT_EQ(*r1.value(), 10);

  // Mutate the value through shared_ptr
  *r1.value() = 42;

  // Another lookup should see the mutation
  auto r2 = cache.get_or_compute(1);
  ASSERT_EQ(*r2.value(), 42);
  ASSERT_EQ(r1.value().get(), r2.value().get());
}

TEST_F(BasicFixture, stats_basic)
{
  cache.get_or_compute(1); // miss
  cache.get_or_compute(1); // hit
  cache.get_or_compute(2); // miss

  auto s = cache.stats();
  ASSERT_EQ(s.misses, 2u);
  ASSERT_EQ(s.hits, 1u);
}

// ================================================================
// Negative caching tests
// ================================================================

struct NegativeCacheFixture : public testing::Test
{
  int call_count = 0;

  Cache<int, int> cache;

  NegativeCacheFixture()
    : cache(5, 2s, 1s,
            [this](const int &) -> shared_ptr<int>
            {
              ++call_count;
              return nullptr; // Always fail
            })
  {
    // empty
  }
};

TEST_F(NegativeCacheFixture, negative_result_cached)
{
  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(r1.is_negative());
  ASSERT_TRUE(r1.was_computed());
  ASSERT_EQ(r1.origin(), ResultOrigin::ComputedNegative);
  ASSERT_EQ(r1.value(), nullptr);
  ASSERT_EQ(call_count, 1);

  // Second call should be a cached negative hit
  auto r2 = cache.get_or_compute(1);
  ASSERT_TRUE(r2.is_negative());
  ASSERT_TRUE(r2.is_hit());
  ASSERT_EQ(r2.origin(), ResultOrigin::HitNegative);
  ASSERT_EQ(call_count, 1); // Solver NOT called again
}

TEST_F(NegativeCacheFixture, simplified_get_negative_api)
{
  auto val1 = cache.get(1);
  ASSERT_EQ(val1, nullptr); // First miss, returns nullptr
  ASSERT_EQ(call_count, 1);

  auto val2 = cache.get(1);
  ASSERT_EQ(val2, nullptr); // Second time, hits the negative cache
  ASSERT_EQ(call_count, 1); // Solver not called again
}

TEST_F(NegativeCacheFixture, negative_ttl_expiration)
{
  cache.get_or_compute(1);
  ASSERT_EQ(call_count, 1);

  // Wait for negative TTL to expire (1s)
  this_thread::sleep_for(1100ms);

  cache.get_or_compute(1);
  ASSERT_EQ(call_count, 2); // Solver called again after negative TTL expires
}

TEST_F(NegativeCacheFixture, stats_negative_hits)
{
  cache.get_or_compute(1); // miss
  cache.get_or_compute(1); // negative hit

  auto s = cache.stats();
  ASSERT_EQ(s.misses, 1u);
  ASSERT_EQ(s.negative_hits, 1u);
}

TEST_F(NegativeCacheFixture, negative_hit_refreshes_ttl)
{
  cache.get_or_compute(1);
  ASSERT_EQ(call_count, 1);

  // Wait for 600ms (more than half of 1s negative TTL)
  this_thread::sleep_for(600ms);

  // Negative hit -> should refresh the TTL back to 1s
  auto r2 = cache.get_or_compute(1);
  ASSERT_TRUE(r2.is_negative());
  ASSERT_TRUE(r2.is_hit());

  // Wait another 600ms. Total time is 1.2s. Without refresh, it would expire.
  this_thread::sleep_for(600ms);

  // Should still be a cached negative because of the refresh
  auto r3 = cache.get_or_compute(1);
  ASSERT_TRUE(r3.is_negative());
  ASSERT_TRUE(r3.is_hit());
  ASSERT_EQ(call_count, 1); // Solver not called again
}

// ================================================================
// CacheResult type tests
// ================================================================

TEST(CacheResultTest, positive_result)
{
  auto r = CacheResult<int>::computed_positive(make_shared<int>(42));
  ASSERT_TRUE(r.is_positive());
  ASSERT_FALSE(r.is_negative());
  ASSERT_TRUE(r.was_computed());
  ASSERT_FALSE(r.is_hit());
  ASSERT_TRUE(static_cast<bool>(r));
  ASSERT_EQ(*r.value(), 42);
}

TEST(CacheResultTest, negative_result)
{
  auto r = CacheResult<int>::hit_negative();
  ASSERT_FALSE(r.is_positive());
  ASSERT_TRUE(r.is_negative());
  ASSERT_TRUE(r.is_hit());
  ASSERT_FALSE(static_cast<bool>(r));
  ASSERT_EQ(r.value(), nullptr);
}

// ================================================================
// Complex key tests
// ================================================================

TEST(ComplexKeyTest, string_keys)
{
  Cache<string, string> cache(
    3, 2s, 1s,
    [](const string & key) -> shared_ptr<string>
    {
      return make_shared<string>("value_for_" + key);
    });

  auto r = cache.get_or_compute("hello");
  ASSERT_TRUE(r.is_positive());
  ASSERT_EQ(*r.value(), "value_for_hello");
}

// ================================================================
// Variadic Args tests
// ================================================================

struct MockDbConnection {
  int query_count = 0;
  std::string last_tx;

  std::shared_ptr<int> fetch(int id, const std::string& tx) {
    query_count++;
    last_tx = tx;
    return std::make_shared<int>(id * 100);
  }
};

TEST(VariadicArgsTest, single_extra_arg)
{
  Cache<int, std::string, std::equal_to<int>, std::string> cache(
    3, 10s, 1s,
    [](const int &key, std::string suffix) -> std::shared_ptr<std::string>
    {
      return std::make_shared<std::string>(std::to_string(key) + "_" + suffix);
    });

  auto r = cache.get_or_compute(5, "extra");
  ASSERT_TRUE(r.is_positive());
  ASSERT_EQ(*r.value(), "5_extra");
}

TEST(VariadicArgsTest, multiple_args_with_references)
{
  Cache<int, int, std::equal_to<int>, MockDbConnection&, const std::string&> cache(
    10, 10s, 1s,
    [](const int &key, MockDbConnection& db, const std::string& tx_id) -> std::shared_ptr<int>
    {
      return db.fetch(key, tx_id);
    });

  MockDbConnection db;

  auto r1 = cache.get_or_compute(1, db, "tx_1");
  ASSERT_TRUE(r1.is_positive());
  ASSERT_EQ(*r1.value(), 100);
  ASSERT_EQ(db.query_count, 1);
  ASSERT_EQ(db.last_tx, "tx_1");

  // A cache hit should NOT execute the solver again, thus query_count remains 1
  auto r2 = cache.get_or_compute(1, db, "tx_2");
  ASSERT_TRUE(r2.is_hit());
  ASSERT_EQ(*r2.value(), 100);
  ASSERT_EQ(db.query_count, 1); // Solver not called
  ASSERT_EQ(db.last_tx, "tx_1"); // Last TX shouldn't have changed
}

TEST(VariadicArgsTest, solver_exception_with_extra_args)
{
  int call_count = 0;

  Cache<int, std::string, std::equal_to<int>, std::string> cache(
    5, 10s, 5s,
    [&](const int &, std::string) -> std::shared_ptr<std::string>
    {
      ++call_count;
      throw std::runtime_error("solver failed with args");
    });

  auto r = cache.get_or_compute(1, "extra_arg");
  ASSERT_TRUE(r.is_negative());
  ASSERT_TRUE(r.was_computed());
  ASSERT_EQ(r.value(), nullptr);
  ASSERT_EQ(call_count, 1);
}

TEST(VariadicArgsTest, solver_returns_nullptr_with_extra_args)
{
  int call_count = 0;

  Cache<int, std::string, std::equal_to<int>, std::string> cache(
    5, 10s, 5s,
    [&](const int &, std::string) -> std::shared_ptr<std::string>
    {
      ++call_count;
      return nullptr;
    });

  auto r1 = cache.get_or_compute(1, "arg1");
  ASSERT_TRUE(r1.is_negative());
  ASSERT_EQ(call_count, 1);

  // Cached negative hit
  auto r2 = cache.get_or_compute(1, "arg2");
  ASSERT_TRUE(r2.is_negative());
  ASSERT_TRUE(r2.is_hit());
  ASSERT_EQ(call_count, 1);
}

// ================================================================
// get() simplified API — additional coverage
// ================================================================

TEST(GetApiTest, get_returns_nullptr_on_saturated)
{
  std::atomic<bool> may_finish{false};
  std::atomic<int> solver_started{0};

  Cache<int, int> cache(1, 20s, 1s,
    [&](const int &key) -> std::shared_ptr<int>
    {
      solver_started.fetch_add(1);
      while (!may_finish.load(std::memory_order_acquire))
        std::this_thread::sleep_for(100us);
      return std::make_shared<int>(key * 10);
    });

  // Start a slow solver for key=1 (occupies the single slot)
  auto f = std::async(std::launch::async, [&]()
  {
    return cache.get_or_compute(1);
  });

  while (solver_started.load() < 1)
    std::this_thread::sleep_for(100us);

  // Cache is saturated: get() should return nullptr
  auto val = cache.get(2);
  ASSERT_EQ(val, nullptr);

  may_finish.store(true, std::memory_order_release);
  auto r = f.get();
  ASSERT_TRUE(r.is_positive());
}

// ================================================================
// CacheResult — saturated factory coverage
// ================================================================

TEST(CacheResultTest, saturated_result)
{
  auto s = CacheResult<int>::saturated();
  EXPECT_TRUE(s.is_saturated());
  EXPECT_FALSE(s.is_positive());
  EXPECT_FALSE(s.is_negative());
  EXPECT_FALSE(s.is_hit());
  EXPECT_FALSE(s.was_computed());
  EXPECT_EQ(s.origin(), ResultOrigin::Saturated);
  EXPECT_EQ(s.value(), nullptr);
  EXPECT_FALSE(static_cast<bool>(s));
}

// ================================================================
// Eviction cycle: evict then re-insert same key
// ================================================================

TEST(EvictionCycleTest, reinsert_after_eviction)
{
  int call_count = 0;

  Cache<int, int> cache(2, 20s, 10s,
    [&](const int &key) -> std::shared_ptr<int>
    {
      ++call_count;
      return std::make_shared<int>(key * 10 + call_count);
    });

  // Fill cache
  auto r1 = cache.get_or_compute(1); // call_count=1, value=11
  cache.get_or_compute(2);           // call_count=2, value=22

  ASSERT_EQ(cache.size(), 2u);
  ASSERT_EQ(*r1.value(), 11);

  // Evict key=1 by inserting key=3
  cache.get_or_compute(3);           // call_count=3, value=33
  ASSERT_FALSE(cache.has(1));

  // Re-insert key=1: should recompute with fresh value
  auto r4 = cache.get_or_compute(1); // call_count=4, value=14
  ASSERT_TRUE(r4.was_computed());
  ASSERT_EQ(*r4.value(), 14);

  // Old shared_ptr from first computation should still hold old value
  ASSERT_EQ(*r1.value(), 11);

  // New and old pointers should be different objects
  ASSERT_NE(r1.value().get(), r4.value().get());
}
