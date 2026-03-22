//
// cache_hardened_test.cpp -- Hardened unit tests for cpp-cache v2
//
// These tests systematically probe edge cases, state transitions,
// and known bugs identified in the audit. Tests targeting known bugs
// are marked with [BUG x.y] referencing the audit section.
//

# include <atomic>
# include <chrono>
# include <functional>
# include <future>
# include <memory>
# include <string>
# include <thread>
# include <vector>
# include <gtest/gtest.h>

# include <cache/cache.H>

using namespace std;
using namespace std::chrono;
using namespace CppCache;

// ================================================================
// Helpers
// ================================================================

static shared_ptr<int> int_solver(const int &key)
{
  return make_shared<int>(key * 10);
}

// ================================================================
// Section 1: Edge Cases
// ================================================================

TEST(EdgeCase, capacity_one_basic)
{
  Cache<int, int> cache(1, 10s, 5s, int_solver);

  ASSERT_EQ(cache.capacity(), 1u);
  ASSERT_EQ(cache.size(), 0u);

  auto r = cache.get_or_compute(1);
  ASSERT_TRUE(r.is_positive());
  ASSERT_EQ(*r.value(), 10);
  ASSERT_EQ(cache.size(), 1u);

  // Hit
  auto r2 = cache.get_or_compute(1);
  ASSERT_TRUE(r2.is_hit());
  ASSERT_EQ(*r2.value(), 10);
}

TEST(EdgeCase, capacity_one_eviction)
{
  Cache<int, int> cache(1, 10s, 5s, int_solver);

  cache.get_or_compute(1);
  ASSERT_TRUE(cache.has(1));

  // Second key evicts the first
  cache.get_or_compute(2);
  ASSERT_FALSE(cache.has(1));
  ASSERT_TRUE(cache.has(2));
  ASSERT_EQ(cache.size(), 1u);
}

TEST(EdgeCase, zero_positive_ttl_expires_immediately)
{
  // TTL = 0s means entries should expire on next access
  Cache<int, int> cache(5, 0s, 0s, int_solver);

  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(r1.is_positive());
  ASSERT_EQ(*r1.value(), 10);

  // Tiny sleep to ensure steady_clock advances
  this_thread::sleep_for(1ms);

  // With TTL=0, the entry should be expired
  EXPECT_FALSE(cache.has(1))
    << "Entry with TTL=0 should expire immediately";

  // Recompute
  auto r2 = cache.get_or_compute(1);
  EXPECT_TRUE(r2.was_computed())
    << "Expired entry should be recomputed";
}

// ================================================================
// Section 2: Solver Behavior
// ================================================================

TEST(SolverTest, exception_produces_negative_result)
{
  int call_count = 0;

  Cache<int, int> cache(5, 10s, 5s,
    [&](const int &) -> shared_ptr<int>
    {
      ++call_count;
      throw runtime_error("solver failed");
    });

  auto r = cache.get_or_compute(1);

  EXPECT_TRUE(r.is_negative())
    << "Solver exception should produce negative result";
  EXPECT_TRUE(r.was_computed());
  EXPECT_EQ(r.origin(), ResultOrigin::ComputedNegative);
  EXPECT_EQ(call_count, 1);
}

TEST(SolverTest, exception_entry_reusable_after_negative_ttl)
{
  int call_count = 0;
  bool should_throw = true;

  Cache<int, int> cache(5, 10s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      ++call_count;
      if (should_throw)
        throw runtime_error("fail");
      return make_shared<int>(key * 10);
    });

  // First call: exception -> negative
  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(r1.is_negative());

  // Second call within negative TTL: cached negative hit
  auto r2 = cache.get_or_compute(1);
  EXPECT_TRUE(r2.is_negative());
  EXPECT_TRUE(r2.is_hit());
  EXPECT_EQ(call_count, 1);

  // Wait for negative TTL to expire
  this_thread::sleep_for(1100ms);

  // Now fix the solver
  should_throw = false;

  // Should recompute successfully
  auto r3 = cache.get_or_compute(1);
  EXPECT_TRUE(r3.is_positive());
  EXPECT_TRUE(r3.was_computed());
  EXPECT_EQ(*r3.value(), 10);
  EXPECT_EQ(call_count, 2);
}

TEST(SolverTest, mixed_positive_negative_results)
{
  // Even keys succeed, odd keys fail
  Cache<int, int> cache(10, 10s, 5s,
    [](const int &key) -> shared_ptr<int>
    {
      if (key % 2 == 0)
        return make_shared<int>(key * 10);
      return nullptr;
    });

  for (int i = 1; i <= 6; ++i)
    {
      auto r = cache.get_or_compute(i);
      if (i % 2 == 0)
        {
          EXPECT_TRUE(r.is_positive()) << "key=" << i;
          EXPECT_EQ(*r.value(), i * 10) << "key=" << i;
        }
      else
        {
          EXPECT_TRUE(r.is_negative()) << "key=" << i;
          EXPECT_EQ(r.value(), nullptr) << "key=" << i;
        }
    }

  auto s = cache.stats();
  EXPECT_EQ(s.misses, 6u);
}

// ================================================================
// Section 3: Find Operations
// ================================================================

TEST(FindTest, find_on_expired_returns_nullopt)
{
  Cache<int, int> cache(5, 1s, 1s, int_solver);

  cache.get_or_compute(1);
  ASSERT_TRUE(cache.find(1).has_value());

  this_thread::sleep_for(1100ms);

  auto r = cache.find(1);
  EXPECT_FALSE(r.has_value())
    << "find() on expired entry should return nullopt";
}

TEST(FindTest, find_on_invalidated_returns_nullopt)
{
  Cache<int, int> cache(5, 10s, 5s, int_solver);

  cache.get_or_compute(1);
  cache.invalidate(1);

  auto r = cache.find(1);
  EXPECT_FALSE(r.has_value())
    << "find() on invalidated entry should return nullopt";
}

TEST(FindTest, find_on_failed_returns_negative_hit)
{
  Cache<int, int> cache(5, 10s, 5s,
    [](const int &) -> shared_ptr<int> { return nullptr; });

  cache.get_or_compute(1); // Caches negative result

  auto r = cache.find(1);
  ASSERT_TRUE(r.has_value())
    << "find() on Failed entry within TTL should return a result";
  EXPECT_TRUE(r->is_negative());
  EXPECT_TRUE(r->is_hit());
  EXPECT_EQ(r->origin(), ResultOrigin::HitNegative);
}

TEST(FindTest, find_never_invokes_solver)
{
  int solver_calls = 0;

  Cache<int, int> cache(5, 10s, 5s,
    [&](const int &key) -> shared_ptr<int>
    {
      ++solver_calls;
      return make_shared<int>(key * 10);
    });

  // find() on non-existent key should NOT invoke solver
  auto r = cache.find(1);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(solver_calls, 0)
    << "find() must never invoke the solver";

  // Populate, then find() on existing key
  cache.get_or_compute(1);
  ASSERT_EQ(solver_calls, 1);

  auto r2 = cache.find(1);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(solver_calls, 1)
    << "find() must never invoke the solver, even on existing entries";
}

// [BUG] find() on invalidated entry undoes LRU priority positioning
TEST(FindBug, find_on_invalidated_undoes_lru_eviction_priority)
{
  // After invalidate(), the entry moves to LRU tail (priority eviction).
  // find() returns nullopt (correct) but moves the entry to MRU (wrong).
  // This defeats the purpose of invalidation's LRU positioning.

  Cache<int, int> cache(3, 20s, 10s, int_solver);

  cache.get_or_compute(1); // LRU order: 1
  cache.get_or_compute(2); // LRU order: 1, 2
  cache.get_or_compute(3); // LRU order: 1, 2, 3

  // Invalidate key=1 → moves to LRU tail (priority eviction)
  ASSERT_TRUE(cache.invalidate(1));

  // find(1) returns nullopt (correct), but moves key=1 to MRU (bug)
  auto r = cache.find(1);
  ASSERT_FALSE(r.has_value());

  // Insert key=4: should evict key=1 (invalidated, should be at LRU)
  cache.get_or_compute(4);

  // Correct behavior: key=1 evicted (invalidated), key=2 alive
  // Bug behavior: key=2 evicted (it's LRU after find moved key=1 to MRU)
  EXPECT_FALSE(cache.has(1))
    << "BUG: find() moved invalidated entry to MRU, preventing "
       "eviction priority. Invalidated entry should be evicted first.";
  EXPECT_TRUE(cache.has(2))
    << "BUG: valid entry evicted instead of invalidated entry";
  EXPECT_TRUE(cache.has(4));
}

// ================================================================
// Section 4: Invalidation
// ================================================================

TEST(InvalidationTest, double_invalidation_returns_false)
{
  Cache<int, int> cache(5, 10s, 5s, int_solver);

  cache.get_or_compute(1);
  ASSERT_TRUE(cache.invalidate(1));

  // Second invalidation should return false (already invalidated)
  EXPECT_FALSE(cache.invalidate(1))
    << "Double invalidation should return false";
}

TEST(InvalidationTest, invalidate_nonexistent_returns_false)
{
  Cache<int, int> cache(5, 10s, 5s, int_solver);
  EXPECT_FALSE(cache.invalidate(999));
}

TEST(InvalidationTest, invalidated_entry_evicted_before_ready)
{
  // Invalidated entries should be at LRU tail and evicted first
  Cache<int, int> cache(3, 20s, 10s, int_solver);

  cache.get_or_compute(1);
  cache.get_or_compute(2);
  cache.get_or_compute(3);
  // LRU order: 1 (LRU), 2, 3 (MRU)

  // Invalidate key=3 (currently MRU). It should move to LRU tail.
  ASSERT_TRUE(cache.invalidate(3));

  // Insert key=4: key=3 (invalidated, at LRU tail) should be evicted
  cache.get_or_compute(4);

  EXPECT_FALSE(cache.has(3))
    << "Invalidated entry should be evicted before valid entries";
  EXPECT_TRUE(cache.has(1));
  EXPECT_TRUE(cache.has(2));
  EXPECT_TRUE(cache.has(4));
}

TEST(InvalidationTest, invalidate_preserves_size)
{
  // Invalidation is lazy: entry stays in table until eviction
  Cache<int, int> cache(5, 10s, 5s, int_solver);

  cache.get_or_compute(1);
  cache.get_or_compute(2);
  ASSERT_EQ(cache.size(), 2u);

  cache.invalidate(1);

  // Size should NOT change (lazy removal)
  EXPECT_EQ(cache.size(), 2u)
    << "Invalidation is lazy: size should not change";
}

TEST(InvalidationTest, invalidate_then_get_or_compute_recomputes)
{
  int call_count = 0;

  Cache<int, int> cache(5, 10s, 5s,
    [&](const int &key) -> shared_ptr<int>
    {
      ++call_count;
      return make_shared<int>(key * 10 + call_count);
    });

  auto r1 = cache.get_or_compute(1);
  ASSERT_EQ(*r1.value(), 11); // key*10 + call_count(1)

  cache.invalidate(1);

  auto r2 = cache.get_or_compute(1);
  EXPECT_TRUE(r2.was_computed())
    << "After invalidation, get_or_compute should recompute";
  EXPECT_EQ(*r2.value(), 12); // key*10 + call_count(2)
  EXPECT_EQ(call_count, 2);
}

// ================================================================
// Section 5: Touch
// ================================================================

TEST(TouchTest, touch_nonexistent_returns_false)
{
  Cache<int, int> cache(5, 10s, 5s, int_solver);
  EXPECT_FALSE(cache.touch(999));
}

TEST(TouchTest, touch_expired_returns_false)
{
  Cache<int, int> cache(5, 1s, 1s, int_solver);

  cache.get_or_compute(1);
  this_thread::sleep_for(1100ms);

  EXPECT_FALSE(cache.touch(1))
    << "touch() on expired entry should return false";
}

TEST(TouchTest, touch_refreshes_ttl)
{
  Cache<int, int> cache(5, 2s, 1s, int_solver);

  cache.get_or_compute(1);

  // Wait 1.5s (within 2s TTL)
  this_thread::sleep_for(1500ms);

  // Touch should succeed and refresh TTL
  ASSERT_TRUE(cache.touch(1));

  // Wait another 1.5s (3s total from insert, but only 1.5s from touch)
  this_thread::sleep_for(1500ms);

  // Should still be valid (TTL refreshed by touch)
  EXPECT_TRUE(cache.has(1))
    << "touch() should refresh TTL, extending the entry's lifetime";
}

TEST(TouchTest, touch_on_failed_refreshes_ttl)
{
  Cache<int, int> cache(5, 10s, 1s,
    [](const int &) -> shared_ptr<int> { return nullptr; });

  cache.get_or_compute(1); // Failed entry (1s TTL)

  this_thread::sleep_for(600ms);

  // touch() should now work on Failed entries to extend negative TTL
  EXPECT_TRUE(cache.touch(1));

  this_thread::sleep_for(600ms);

  // Total time elapsed: 1.2s. Without touch, it would have expired.
  // With touch, it remains a hit.
  auto r = cache.get_or_compute(1);
  EXPECT_TRUE(r.is_negative());
  EXPECT_TRUE(r.is_hit());
}

TEST(TouchTest, touch_on_invalidated_returns_false)
{
  Cache<int, int> cache(5, 10s, 5s, int_solver);

  cache.get_or_compute(1);
  cache.invalidate(1);

  EXPECT_FALSE(cache.touch(1))
    << "touch() on invalidated entry should return false";
}

// ================================================================
// Section 6: Statistics (was Section 7)
// ================================================================

TEST(StatsTest, eviction_count_accurate)
{
  Cache<int, int> cache(3, 10s, 5s, int_solver);

  // Fill cache
  for (int i = 1; i <= 3; ++i)
    cache.get_or_compute(i);

  ASSERT_EQ(cache.stats().evictions, 0u);

  // Each new key causes one eviction
  cache.get_or_compute(4); // evicts key=1
  EXPECT_EQ(cache.stats().evictions, 1u);

  cache.get_or_compute(5); // evicts key=2
  EXPECT_EQ(cache.stats().evictions, 2u);

  cache.get_or_compute(6); // evicts key=3
  EXPECT_EQ(cache.stats().evictions, 3u);
}

TEST(StatsTest, stats_after_ttl_recompute)
{
  Cache<int, int> cache(5, 1s, 1s, int_solver);

  cache.get_or_compute(1); // miss
  cache.get_or_compute(1); // hit

  auto s1 = cache.stats();
  ASSERT_EQ(s1.misses, 1u);
  ASSERT_EQ(s1.hits, 1u);

  this_thread::sleep_for(1100ms);

  cache.get_or_compute(1); // expired → recompute (miss)

  auto s2 = cache.stats();
  EXPECT_EQ(s2.misses, 2u);
  EXPECT_EQ(s2.hits, 1u);
}

TEST(StatsTest, negative_hits_counted_correctly)
{
  Cache<int, int> cache(5, 10s, 5s,
    [](const int &) -> shared_ptr<int> { return nullptr; });

  cache.get_or_compute(1); // miss (negative)
  cache.get_or_compute(1); // negative hit
  cache.get_or_compute(1); // negative hit

  auto s = cache.stats();
  EXPECT_EQ(s.misses, 1u);
  EXPECT_EQ(s.negative_hits, 2u);
  EXPECT_EQ(s.hits, 0u);
}

// ================================================================
// Section 8: LRU Ordering
// ================================================================

TEST(LRUTest, hit_moves_to_mru_changes_eviction_order)
{
  Cache<int, int> cache(3, 20s, 10s, int_solver);

  cache.get_or_compute(1); // LRU: 1
  cache.get_or_compute(2); // LRU: 1, 2
  cache.get_or_compute(3); // LRU: 1, 2, 3

  // Hit key=1 moves it to MRU → LRU: 2, 3, 1
  cache.get_or_compute(1);

  // Insert key=4: evicts key=2 (now LRU)
  cache.get_or_compute(4);

  EXPECT_TRUE(cache.has(1)) << "Key=1 was hit, should be MRU";
  EXPECT_FALSE(cache.has(2)) << "Key=2 should be evicted (was LRU)";
  EXPECT_TRUE(cache.has(3));
  EXPECT_TRUE(cache.has(4));
}

TEST(LRUTest, find_moves_to_mru_for_valid_entries)
{
  Cache<int, int> cache(3, 20s, 10s, int_solver);

  cache.get_or_compute(1);
  cache.get_or_compute(2);
  cache.get_or_compute(3);

  // find(1) should move key=1 to MRU → LRU: 2, 3, 1
  auto r = cache.find(1);
  ASSERT_TRUE(r.has_value());

  // Insert key=4: evicts key=2 (now LRU)
  cache.get_or_compute(4);

  EXPECT_TRUE(cache.has(1)) << "Key=1 was found, should be MRU";
  EXPECT_FALSE(cache.has(2)) << "Key=2 should be evicted (LRU)";
}

TEST(LRUTest, multiple_sequential_evictions)
{
  Cache<int, int> cache(3, 20s, 10s, int_solver);

  // Insert 10 keys into a size-3 cache
  for (int i = 1; i <= 10; ++i)
    {
      auto r = cache.get_or_compute(i);
      ASSERT_TRUE(r.is_positive()) << "key=" << i;
      ASSERT_EQ(*r.value(), i * 10) << "key=" << i;
    }

  EXPECT_EQ(cache.size(), 3u);

  // Only the last 3 keys should remain
  for (int i = 1; i <= 7; ++i)
    EXPECT_FALSE(cache.has(i)) << "key=" << i << " should be evicted";

  for (int i = 8; i <= 10; ++i)
    EXPECT_TRUE(cache.has(i)) << "key=" << i << " should be present";

  EXPECT_EQ(cache.stats().evictions, 7u);
}

TEST(LRUTest, eviction_order_with_interleaved_access)
{
  // Verify that access patterns correctly influence eviction order
  Cache<int, int> cache(4, 20s, 10s, int_solver);

  cache.get_or_compute(1); // LRU: 1
  cache.get_or_compute(2); // LRU: 1, 2
  cache.get_or_compute(3); // LRU: 1, 2, 3
  cache.get_or_compute(4); // LRU: 1, 2, 3, 4

  // Access in pattern: 2, 1 → LRU: 3, 4, 2, 1
  cache.get_or_compute(2);
  cache.get_or_compute(1);

  // Insert key=5: evicts key=3 (LRU)
  cache.get_or_compute(5);
  EXPECT_FALSE(cache.has(3));
  EXPECT_TRUE(cache.has(4));

  // Insert key=6: evicts key=4 (now LRU)
  cache.get_or_compute(6);
  EXPECT_FALSE(cache.has(4));
  EXPECT_TRUE(cache.has(2));
  EXPECT_TRUE(cache.has(1));
}

// ================================================================
// Section 9: Recomputation
// ================================================================

TEST(RecomputeTest, after_ttl_solver_returns_different_value)
{
  atomic<int> call_count{0};

  Cache<int, int> cache(5, 1s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      int n = ++call_count;
      return make_shared<int>(key * 10 + n);
    });

  auto r1 = cache.get_or_compute(1);
  ASSERT_EQ(*r1.value(), 11);

  this_thread::sleep_for(1100ms);

  auto r2 = cache.get_or_compute(1);
  EXPECT_TRUE(r2.was_computed());
  EXPECT_EQ(*r2.value(), 12)
    << "After TTL expiry, solver should be called again with fresh result";

  // Old shared_ptr should still hold old value
  EXPECT_EQ(*r1.value(), 11);
}

// [BUG 2.1] TTL starts from lookup time, not solver completion time
TEST(RecomputeBug, stale_ttl_for_slow_solver)
{
  // The TTL is calculated as now + positive_ttl where `now` is captured
  // BEFORE the solver runs. For a solver that takes 2 seconds with a 3s
  // TTL, the entry effectively has only ~1s of validity after completion.

  Cache<int, int> cache(5, 3s, 1s,
    [](const int &key) -> shared_ptr<int>
    {
      this_thread::sleep_for(2s); // Slow solver
      return make_shared<int>(key * 10);
    });

  auto r1 = cache.get_or_compute(1);
  ASSERT_TRUE(r1.is_positive());

  // Solver just finished. With correct TTL (starting from now),
  // the entry should be valid for 3 more seconds.
  // With buggy TTL (starting from before solver), it's valid for ~1s.

  this_thread::sleep_for(1500ms);

  auto r2 = cache.get_or_compute(1);
  EXPECT_TRUE(r2.is_hit())
    << "BUG 2.1: TTL starts from lookup time (before solver runs), not "
       "from solver completion. A 2s solver + 3s TTL gives only ~1s of "
       "effective validity. The entry expired prematurely.";
}

// ================================================================
// Section 10: Size Tracking
// ================================================================

TEST(SizeTest, size_through_lifecycle)
{
  Cache<int, int> cache(5, 10s, 5s, int_solver);

  ASSERT_EQ(cache.size(), 0u);

  // Insert
  cache.get_or_compute(1);
  EXPECT_EQ(cache.size(), 1u);

  cache.get_or_compute(2);
  cache.get_or_compute(3);
  EXPECT_EQ(cache.size(), 3u);

  // Hit doesn't change size
  cache.get_or_compute(1);
  EXPECT_EQ(cache.size(), 3u);

  // Invalidation does NOT change size (lazy)
  cache.invalidate(1);
  EXPECT_EQ(cache.size(), 3u);

  // Find doesn't change size
  cache.find(3);
  EXPECT_EQ(cache.size(), 3u);
}

TEST(SizeTest, size_at_capacity_with_evictions)
{
  Cache<int, int> cache(3, 10s, 5s, int_solver);

  for (int i = 1; i <= 3; ++i)
    cache.get_or_compute(i);

  EXPECT_EQ(cache.size(), 3u);

  // Eviction should maintain size at capacity
  for (int i = 4; i <= 10; ++i)
    {
      cache.get_or_compute(i);
      EXPECT_EQ(cache.size(), 3u) << "key=" << i;
    }
}

// ================================================================
// Section 11: CacheResult completeness
// ================================================================

TEST(CacheResultTest, all_factory_methods)
{
  auto hp = CacheResult<int>::hit_positive(make_shared<int>(42));
  EXPECT_TRUE(hp.is_positive());
  EXPECT_TRUE(hp.is_hit());
  EXPECT_FALSE(hp.was_computed());
  EXPECT_FALSE(hp.is_negative());
  EXPECT_TRUE(static_cast<bool>(hp));
  EXPECT_EQ(*hp.value(), 42);

  auto hn = CacheResult<int>::hit_negative();
  EXPECT_FALSE(hn.is_positive());
  EXPECT_TRUE(hn.is_hit());
  EXPECT_FALSE(hn.was_computed());
  EXPECT_TRUE(hn.is_negative());
  EXPECT_FALSE(static_cast<bool>(hn));
  EXPECT_EQ(hn.value(), nullptr);

  auto cp = CacheResult<int>::computed_positive(make_shared<int>(99));
  EXPECT_TRUE(cp.is_positive());
  EXPECT_FALSE(cp.is_hit());
  EXPECT_TRUE(cp.was_computed());
  EXPECT_FALSE(cp.is_negative());
  EXPECT_TRUE(static_cast<bool>(cp));
  EXPECT_EQ(*cp.value(), 99);

  auto cn = CacheResult<int>::computed_negative();
  EXPECT_FALSE(cn.is_positive());
  EXPECT_FALSE(cn.is_hit());
  EXPECT_TRUE(cn.was_computed());
  EXPECT_TRUE(cn.is_negative());
  EXPECT_FALSE(static_cast<bool>(cn));
  EXPECT_EQ(cn.value(), nullptr);
}

// ================================================================
// Section 12: has() edge cases
// ================================================================

TEST(HasTest, has_on_various_states)
{
  Cache<int, int> cache(5, 10s, 5s, int_solver);

  // Non-existent
  EXPECT_FALSE(cache.has(1));

  // Ready (positive)
  cache.get_or_compute(1);
  EXPECT_TRUE(cache.has(1));

  // Invalidated
  cache.invalidate(1);
  EXPECT_FALSE(cache.has(1));
}

TEST(HasTest, has_on_expired)
{
  Cache<int, int> cache(5, 1s, 1s, int_solver);

  cache.get_or_compute(1);
  ASSERT_TRUE(cache.has(1));

  this_thread::sleep_for(1100ms);

  EXPECT_FALSE(cache.has(1))
    << "has() on expired entry should return false";
}

TEST(HasTest, has_on_negative_cached)
{
  Cache<int, int> cache(5, 10s, 5s,
    [](const int &) -> shared_ptr<int> { return nullptr; });

  cache.get_or_compute(1);

  // Failed entries within TTL are "valid" in the sense they're cached
  // is_valid_hit returns true for non-expired Failed entries
  // The question is: does has() consider a Failed entry as "present"?
  // The implementation returns true (Failed + valid TTL → is_valid_hit=true)
  bool has_result = cache.has(1);

  // This tests the actual behavior: has() returns true for cached negatives
  EXPECT_TRUE(has_result)
    << "has() returns true for non-expired Failed entries (cached negatives)";
}

// ================================================================
// Section 13: Complex key types
// ================================================================

TEST(ComplexKeyTest, pair_keys)
{
  using Key = pair<int, int>;

  auto solver = [](const Key &k) -> shared_ptr<int>
  {
    return make_shared<int>(k.first * 100 + k.second);
  };

  auto hash = [](const Key &k) -> size_t
  {
    return std::hash<int>()(k.first) ^ (std::hash<int>()(k.second) << 16);
  };

  Cache<Key, int> cache(5, 10s, 5s, solver, hash);

  auto r1 = cache.get_or_compute({1, 2});
  ASSERT_TRUE(r1.is_positive());
  EXPECT_EQ(*r1.value(), 102);

  auto r2 = cache.get_or_compute({3, 4});
  ASSERT_TRUE(r2.is_positive());
  EXPECT_EQ(*r2.value(), 304);

  auto r3 = cache.get_or_compute({1, 2});
  EXPECT_TRUE(r3.is_hit());
}

// ================================================================
// Section 14: find() TTL refresh for negative entries
// ================================================================

TEST(FindNegativeTTLTest, find_on_failed_entry_refreshes_ttl)
{
  Cache<int, int> cache(5, 10s, 2s,
    [](const int &) -> shared_ptr<int> { return nullptr; });

  // Cache a negative result (negative TTL = 2s)
  cache.get_or_compute(1);

  // Wait for more than half the negative TTL
  this_thread::sleep_for(1200ms);

  // find() should return the negative hit and refresh the TTL
  auto r = cache.find(1);
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r->is_negative());
  ASSERT_TRUE(r->is_hit());

  // Wait another 1.2s. Total since insert: 2.4s (> 2s original TTL).
  // But find() refreshed it, so the new TTL expires 2s from find() call.
  this_thread::sleep_for(1200ms);

  // Should still be valid because find() refreshed the 2s TTL
  auto r2 = cache.find(1);
  EXPECT_TRUE(r2.has_value())
    << "find() on Failed entry should refresh negative TTL";
  EXPECT_TRUE(r2->is_negative());

  // Wait for the refreshed TTL to finally expire
  this_thread::sleep_for(1000ms);

  // Now it should be gone (second refresh would need another find() call)
  this_thread::sleep_for(1100ms);

  EXPECT_FALSE(cache.find(1).has_value())
    << "Negative entry should eventually expire after last access";
}

// ================================================================
// Section 15: find() blocks during Computing (model extension)
// ================================================================

TEST(FindModelTest, find_blocks_during_compute)
{
  atomic<bool> solver_started{false};
  atomic<bool> solver_can_finish{false};

  Cache<int, int> cache(5, 20s, 5s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_started.store(true, memory_order_release);
      while (not solver_can_finish.load(memory_order_acquire))
        this_thread::sleep_for(10ms);
      return make_shared<int>(key * 10);
    });

  // Start computing key 1 in background
  auto compute_future = async(launch::async, [&]()
  {
    return cache.get_or_compute(1);
  });

  // Wait for solver to start
  while (not solver_started.load(memory_order_acquire))
    this_thread::sleep_for(5ms);

  // find(1) should block until computation finishes
  auto find_future = async(launch::async, [&]()
  {
    return cache.find(1);
  });

  // Give find() time to enter the wait
  this_thread::sleep_for(50ms);

  // Release solver
  solver_can_finish.store(true, memory_order_release);

  auto find_result = find_future.get();
  auto compute_result = compute_future.get();

  ASSERT_TRUE(find_result.has_value())
    << "find() must return a value after waiting for Computing entry";
  EXPECT_TRUE(find_result->is_positive());
  EXPECT_EQ(*find_result->value(), 10);
  EXPECT_TRUE(compute_result.is_positive());
}

// ================================================================
// Section 16: find() returns nullopt after expiry (no recompute)
// ================================================================

TEST(FindModelTest, find_returns_nullopt_after_expiry)
{
  atomic<int> solver_calls{0};

  Cache<int, int> cache(5, 1s, 1s,
    [&](const int &key) -> shared_ptr<int>
    {
      solver_calls.fetch_add(1);
      return make_shared<int>(key * 10);
    });

  cache.get_or_compute(1);
  ASSERT_EQ(solver_calls.load(), 1);

  // Wait for TTL to expire
  this_thread::sleep_for(1100ms);

  // find() must return nullopt — it never triggers recomputation
  auto r = cache.find(1);
  EXPECT_FALSE(r.has_value())
    << "find() must not recompute expired entries";

  // Solver must NOT have been called again
  EXPECT_EQ(solver_calls.load(), 1)
    << "find() must never invoke the miss solver";
}