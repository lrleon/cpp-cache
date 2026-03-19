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

TEST_F(BasicFixture, remove_entry)
{
  cache.get_or_compute(1);
  ASSERT_TRUE(cache.has(1));

  ASSERT_TRUE(cache.remove(1));
  ASSERT_FALSE(cache.has(1));
  ASSERT_EQ(cache.size(), 0u);
}

TEST_F(BasicFixture, remove_nonexistent)
{
  ASSERT_FALSE(cache.remove(999));
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
