//
// Created by lrleon on 11/8/24.
//

# include <iostream>
# include <utility>
# include <gtest/gtest.h>
# include <future>
# include <thread>
# include "cpp-cache.H"

# include <tpl_dynMapTree.H>

using namespace std;
using namespace testing;

TEST(cache_entry, basic)
{
  using Cache = Cache<int, int>;
  Cache::CacheEntry cache_entry;

  ASSERT_EQ(cache_entry.status(),
            Cache::CacheEntry::Status::AVAILABLE);
  ASSERT_TRUE(cache_entry.link_lru()->is_empty());
  ASSERT_EQ(cache_entry.ad_hoc_code(), 0);

  cache_entry.set_data(10);

  ASSERT_EQ(cache_entry.get_data(), 10);

  cache_entry.set_status(Cache::CacheEntry::Status::READY);

  ASSERT_EQ(cache_entry.status(),
            Cache::CacheEntry::Status::READY);

  cache_entry.ad_hoc_code() = 1;

  ASSERT_EQ(cache_entry.ad_hoc_code(), 1);

  cache_entry.set_ttl_exp_time(high_resolution_clock::now() + 1s);

  ASSERT_FALSE(cache_entry.has_ttl_expired(high_resolution_clock::now()));

  // sleep for 1 second
  std::this_thread::sleep_for(std::chrono::seconds(1));

  ASSERT_TRUE(cache_entry.has_ttl_expired(high_resolution_clock::now()));

  cout << "CacheEntry: " << cache_entry.get_data() << endl;
}

TEST(cache_entry, key_copy_works)
{
  Cache<vector<int>, int>::CacheEntry cache_entry;

  vector<int> key = {1, 2, 3};
  cache_entry.set_key(key);

  ASSERT_EQ(cache_entry.key(), key);
}

TEST(cache_entry, key_move_works)
{
  Cache<vector<int>, int>::CacheEntry cache_entry;

  vector<int> key = {1, 2, 3};
  cache_entry.set_key(std::move(key));

  ASSERT_EQ(cache_entry.key().size(), 3);
  ASSERT_EQ(cache_entry.key(), vector<int>({1, 2, 3}));
  ASSERT_EQ(key.size(), 0);
  ASSERT_TRUE(key.empty());
}
TEST(cache_entry, data_copy_works)
{
  Cache<int, vector<int>>::CacheEntry cache_entry;

  vector<int> data = {1, 2, 3};
  cache_entry.set_data(data);

  ASSERT_EQ(cache_entry.get_data(), data);
}

TEST(cache_entry, data_move_works)
{
  Cache<int, vector<int>>::CacheEntry cache_entry;

  vector<int> data = {1, 2, 3};
  cache_entry.set_data(std::move(data));

  ASSERT_EQ(cache_entry.get_data().size(), 3);
  ASSERT_EQ(cache_entry.get_data(), vector<int>({1, 2, 3}));
  ASSERT_EQ(data.size(), 0);
  ASSERT_TRUE(data.empty());
}

///template <typename ... Args>
struct SimpleFixture : public Test
{
  //template <typename ... Args>
  static bool miss_handler(const int &key, int *data,
                           int8_t &ad_hoc_code, void *)
  {
    *data = key * 10;
    ++ad_hoc_code; // never must be greater than 1
    return true;
  }

  Cache<int, int> cache;

  SimpleFixture()
    : cache(5, 1s, 1s, miss_handler)
  {
    // empty
  }
};
TEST_F(SimpleFixture, basic)
{
  ASSERT_EQ(cache.capacity(), 5);
  ASSERT_EQ(cache.size(), 0);

  //cache is empty
  ASSERT_FALSE(cache.has(1));

  //insert a key-value pair
  cache.insert(1, 10);

  //cache is not empty
  ASSERT_EQ(cache.size(), 1);

  // check lru entry
  ASSERT_EQ(cache.get_lru_entry()->key(), 1);

  //key is in cache
  ASSERT_TRUE(cache.has(1));

  // wait ttl to expire
  sleep(1);

  // key is not in cache
  ASSERT_FALSE(cache.has(1));

  // check expired key was removed from the cache
  ASSERT_EQ(cache.size(), 0);
}
TEST_F(SimpleFixture, lru)
{
  cache.insert(1, 10);
  cache.insert(2, 20);

  ASSERT_EQ(cache.size(), 2);

  // check lru entry
  ASSERT_EQ(cache.get_lru_entry()->key(), 1);
  ASSERT_NE(cache.get_lru_entry()->key(), 90);

  // remove lru entry
  cache.remove_entry_from_hash_table(cache.get_lru_entry());

  ASSERT_EQ(cache.size(), 1);

  // check lru entry
  ASSERT_EQ(cache.get_lru_entry()->key(), 2);
}
TEST_F(SimpleFixture, insert_with_has_and_touch)
{
  ASSERT_TRUE(cache.insert(1, 10))
            << "It should be able to insert a key-value pair";

  ASSERT_TRUE(cache.has(1))
            << "It should be able to find the key";

  {
    auto p_lru = cache.get_lru();
    ASSERT_EQ(p_lru.first, 1);
    ASSERT_EQ(p_lru.second, 10);

    auto p_mru = cache.get_mru();
    ASSERT_EQ(p_mru.first, 1);
    ASSERT_EQ(p_mru.second, 10);

    // test positive key expiration
    sleep(1);
    ASSERT_FALSE(cache.has(1))
              << "It should not be able to find the key, since it has expired";
  }

  ASSERT_TRUE(cache.insert(1, 10))
            << "It should be able to insert a key-value pair";
  ASSERT_TRUE(cache.insert(2, 20))
            << "It should be able to insert a key-value pair";
  ASSERT_TRUE(cache.insert(3, 30))
            << "It should be able to insert a key-value pair";

  {
    auto p_lru = cache.get_lru();
    ASSERT_EQ(p_lru.first, 1);
    ASSERT_EQ(p_lru.second, 10);

    auto p_mru = cache.get_mru();
    ASSERT_EQ(p_mru.first, 3);
    ASSERT_EQ(p_mru.second, 30);
  }

  ASSERT_TRUE(cache.touch(2))
            << "It should be able to touch the key";

  auto p_mru = cache.get_mru();
  ASSERT_EQ(p_mru.first, 2);
  ASSERT_EQ(p_mru.second, 20);

  auto p_lru = cache.get_lru();
  ASSERT_EQ(p_lru.first, 1);
  ASSERT_EQ(p_lru.second, 10);
}
TEST_F(SimpleFixture, touch)
{
  cache.insert(1, 10);
  cache.insert(2, 20);

  ASSERT_EQ(cache.size(), 2);

  // touch key 1
  ASSERT_TRUE(cache.touch(1));

  // check lru entry
  ASSERT_EQ(cache.get_lru_entry()->key(), 2);
  ASSERT_NE(cache.get_lru_entry()->key(), 1);

  // touch unknown key
  ASSERT_FALSE(cache.touch(90));
}
TEST_F(SimpleFixture, cache_is_full)
{
  cache.insert(1, 10);
  cache.insert(2, 20);
  cache.insert(3, 30);
  cache.insert(4, 40);
  cache.insert(5, 50);

  ASSERT_EQ(cache.size(), 5);

  {
    auto p_lru = cache.get_lru();
    ASSERT_EQ(p_lru.first, 1);
    ASSERT_EQ(p_lru.second, 10);

    auto p_mru = cache.get_mru();
    ASSERT_EQ(p_mru.first, 5);
    ASSERT_EQ(p_mru.second, 50);
  }

  // insert a new key
  cache.insert(6, 60);

  ASSERT_EQ(cache.size(), 5);

  {
    auto p_lru = cache.get_lru();
    ASSERT_EQ(p_lru.first, 2);
    ASSERT_EQ(p_lru.second, 20);

    auto p_mru = cache.get_mru();
    ASSERT_EQ(p_mru.first, 6);
    ASSERT_EQ(p_mru.second, 60);
  }

  ASSERT_FALSE(cache.has(1));
}
TEST_F(SimpleFixture, remove)
{
  cache.insert(1, 10);
  cache.insert(2, 20);

  ASSERT_EQ(cache.size(), 2);

  ASSERT_TRUE(cache.has(1));

  cache.remove(1);

  ASSERT_EQ(cache.size(), 1);
  ASSERT_FALSE(cache.has(1));
}
TEST_F(SimpleFixture, retrieve_or_compute_basic)
{
  auto res = cache.retrieve_from_cache_or_compute(1);

  ASSERT_EQ(cache.size(), 1);
  ASSERT_TRUE(cache.has(1));
  ASSERT_EQ(*res.first, 10);
  ASSERT_EQ(res.second, 1);

  for (int i = 1; i < 10; ++i)
    {
      res = cache.retrieve_from_cache_or_compute(1);

      ASSERT_EQ(cache.size(), 1);
      ASSERT_TRUE(cache.has(1));
      (*res.first)++;
      ASSERT_EQ(*res.first, 10 + i);
      ASSERT_EQ(res.second, 1);
    }
}

TEST_F(SimpleFixture, retrieve_or_compute_expired)
{
  auto res = cache.retrieve_from_cache_or_compute(1);

  ASSERT_EQ(cache.size(), 1);
  ASSERT_TRUE(cache.has(1));
  ASSERT_EQ(*res.first, 10);
  ASSERT_EQ(res.second, 1);

  // wait ttl to expire
  sleep(1);

  res = cache.retrieve_from_cache_or_compute(1);

  ASSERT_EQ(cache.size(), 1);
  ASSERT_TRUE(cache.has(1));
  ASSERT_EQ(*res.first, 10);
  ASSERT_EQ(res.second, 1);
}
TEST_F(SimpleFixture, iterator)
{
  DynMapTree<int, int> key_value_map = {{1, 10},
                                        {2, 20},
                                        {3, 30},
                                        {4, 40},
                                        {5, 50}};
  cache.insert(1, 10);
  cache.insert(2, 20);
  cache.insert(3, 30);
  cache.insert(4, 40);
  cache.insert(5, 50);

  auto it = cache.get_it();
  ASSERT_TRUE(it.has_curr());
  auto kv = key_value_map.search(it.get_curr().first);
  ASSERT_NE(kv, nullptr);
  ASSERT_EQ(*it.get_curr().second, kv->second);

  it.next();
  ASSERT_TRUE(it.has_curr());
  kv = key_value_map.search(it.get_curr().first);
  ASSERT_NE(kv, nullptr);
  ASSERT_EQ(*it.get_curr().second, kv->second);

  it.next();
  ASSERT_TRUE(it.has_curr());
  kv = key_value_map.search(it.get_curr().first);
  ASSERT_NE(kv, nullptr);
  ASSERT_EQ(*it.get_curr().second, kv->second);

  it.next();
  ASSERT_TRUE(it.has_curr());
  kv = key_value_map.search(it.get_curr().first);
  ASSERT_NE(kv, nullptr);
  ASSERT_EQ(*it.get_curr().second, kv->second);
}

struct TimeConsumingFixture : public Test
{
  static bool miss_handler(const int &key, int *data,
                           int8_t &ad_hoc_code, void *)
  {
    *data = key * 10;
    ++ad_hoc_code; // never must be greater than 1
    sleep(2);
    return true;
  }

  Cache<int, int> cache;

  TimeConsumingFixture()
    : cache(5, 20s, 1s, miss_handler)
  {
    // empty
  }
};

TEST_F(TimeConsumingFixture, calculating_status_while_computing)
{
  using CacheEntry = Cache<int, int>::CacheEntry;
  CacheEntry entry(1);
  CacheEntry *cache_entry = &entry;
  auto future =
    std::async(std::launch::async, [this, &cache_entry]()
    {
      return cache.resolve_cache_miss(cache_entry,
                                      high_resolution_clock::now(), nullptr);
    });

  // wait 1 s
  sleep(1);
  cout << CacheEntry::status_to_string(cache_entry->status()) << endl;
  ASSERT_EQ(cache_entry->status(), CacheEntry::Status::CALCULATING);

  // wait miss handler to finish
  int *res = future.get();

  cout << CacheEntry::status_to_string(cache_entry->status()) << endl;
  ASSERT_EQ(cache_entry->status(), CacheEntry::Status::READY);
  ASSERT_FALSE(cache_entry->has_ttl_expired(high_resolution_clock::now()));

  ASSERT_EQ(*res, 10);
  ASSERT_EQ(cache_entry->ad_hoc_code(), 1);
  ASSERT_EQ(cache_entry->get_data(), 10);
}

TEST_F(TimeConsumingFixture, two_threads)
{
  auto future1 = std::async(std::launch::async, [this]()
  {
    return cache.retrieve_from_cache_or_compute(1);
  });

  auto future2 = std::async(std::launch::async, [this]()
  {
    return cache.retrieve_from_cache_or_compute(1);
  });

  auto res1 = future1.get();
  auto res2 = future2.get();

  ASSERT_EQ(cache.size(), 1);
  ASSERT_TRUE(cache.has(1));

  ASSERT_EQ(*res1.first, 10);
  ASSERT_EQ(res1.second, 1);

  ASSERT_EQ(res1.first, res2.first); // same address
  ASSERT_EQ(res1.second, res2.second);

}

TEST_F(TimeConsumingFixture, multithread_cache_full)
{
  constexpr int N = 3;
  vector<future<pair<int *, int8_t>>> futures;

  for (int i = 1; i <= 5; ++i)
    {
      for (int j = 0; j < N; ++j)
        {
          futures.push_back(std::async(std::launch::async, [this, i]()
          {
            return cache.retrieve_from_cache_or_compute(i);
          }));
        }
    }

  vector<pair<int *, int8_t>> results;
  for (int i = 0; i < N * 5; ++i)
    results.push_back(futures[i].get());

  ASSERT_EQ(cache.size(), 5);

  for (int i = 1; i <= 5; ++i)
    ASSERT_TRUE(cache.has(i));

  for (int i = 0; i < N * 5; i += N)
    {
      auto res_i = results[i];
      for (int j = 1; j < N; ++j)
        {
          auto res_j = results[i + j];
          ASSERT_EQ(res_i.first, res_j.first); // same address
          ASSERT_EQ(res_i.second, res_j.second);
        }
    }

}

TEST_F(TimeConsumingFixture, multithread_heavy_futures)
{
  for (int k = 0; k < 20; k++)
    {
      constexpr int N = 20;
      vector<future<pair<int *, int8_t>>> futures;

      for (int i = 1; i <= 5; ++i)
        {
          for (int j = 0; j < N; ++j)
            {
              futures.push_back(std::async(std::launch::async, [this, i]()
              {
                return cache.retrieve_from_cache_or_compute(i);
              }));
            }
        }

      vector<pair<int *, int8_t>> results;
      for (int i = 0; i < N * 5; ++i)
        results.push_back(futures[i].get());

      ASSERT_EQ(cache.size(), 5);

      for (int i = 1; i <= 5; ++i)
        ASSERT_TRUE(cache.has(i));

      for (int i = 0; i < N * 5; i += N)
        {
          auto res_i = results[i];
          for (int j = 1; j < N; ++j)
            {
              auto res_j = results[i + j];
              ASSERT_EQ(res_i.first, res_j.first); // same address
              ASSERT_EQ(res_i.second, res_j.second);
            }
        }
    }
}

TEST_F(TimeConsumingFixture, multithread_heavy_threads)
{
  for (int k = 0; k < 50; k++)
    {
      constexpr int N = 20;
      vector<thread> threads;
      vector<pair<int *, int8_t>> results(N * 5);
      mutex results_mutex;
      int result_index = 0;

      for (int i = 0; i < 5; ++i)
        for (int j = 0; j < N; ++j)
          {
            threads.emplace_back([this, i, j, &results, &results_mutex, &result_index]()
                                 {
                                   pair<int *, int8_t> result =
                                     cache.retrieve_from_cache_or_compute(i + 1);
                                   {
                                     lock_guard<mutex> lock(results_mutex);
                                     results[i * N + j] = result;
                                   }
                                 });
          }

      for (auto &t: threads)
        t.join();

      for (auto const &res: results)
        ASSERT_EQ(res.second, 1);
      // continue;
      ASSERT_EQ(cache.size(), 5);

      for (int i = 1; i <= 5; ++i)
        ASSERT_TRUE(cache.has(i));

      for (int i = 0; i < N * 5; i += N)
        {
          auto res_i = results[i];
          for (int j = 1; j < N; ++j)
            {
              auto res_j = results[i + j];
              ASSERT_EQ(*res_i.first, *res_j.first);
              ASSERT_EQ(res_i.first, res_j.first); // same address
              ASSERT_EQ(res_i.second, res_j.second);
            }
        }
    }
}

struct RandomTimeFixture : public Test
{
  static bool miss_handler(const int &key, int *data,
                           int8_t &ad_hoc_code, void *)
  {
    *data = key * 10;
    ++ad_hoc_code; // never must be greater than 1

    // sleep for a random time between 0.5 s and 9.5 s
    this_thread::sleep_for(chrono::milliseconds(500) +
                           chrono::milliseconds(rand() % 9000));

    return true;
  }

  Cache<int, int> cache;

  RandomTimeFixture()
    : cache(1019, 10s, 1s, miss_handler)
  {
    // empty
  }
};

TEST_F(RandomTimeFixture, random_miss_handler)
{
  constexpr int N = 30;
  vector<future<pair<int *, int8_t>>> futures;

  for (int i = 1; i <= 5; ++i)
    {
      for (int j = 0; j < N; ++j)
        {
          futures.push_back(std::async(std::launch::async, [this, i]()
          {
            return cache.retrieve_from_cache_or_compute(i);
          }));
        }
    }

  vector<pair<int *, int8_t>> results;
  for (int i = 0; i < N * 5; ++i)
    results.push_back(futures[i].get());

  ASSERT_EQ(cache.size(), 5);

  for (int i = 1; i <= 5; ++i)
    ASSERT_TRUE(cache.has(i));

  for (int i = 0; i < N * 5; i += N)
    {
      auto res_i = results[i];
      for (int j = 1; j < N; ++j)
        {
          auto res_j = results[i + j];
          ASSERT_EQ(res_i.first, res_j.first); // same address
          ASSERT_EQ(res_i.second, res_j.second);
        }
    }
}

struct ComplexKey : public Test
{
  using Tree = DynSetTree<int>;

  template <typename ... Args>
  Tree create_key(Args ... args)
  {
    Tree key;
    (key.insert(args), ...);
    return key;
  }

  static size_t hash_fct(const Tree &tree)
  {
    return tree.foldl<size_t>(0, [](size_t acc, int i)
    {
      return acc ^ SuperFastHash(i);
    });
  }

  static bool miss_handler(const Tree &tree, int *data,
                           int8_t &ad_hoc_code, void *)
  {
    *data = tree.foldl<int>(0, [](int acc, int i)
    {
      return acc + i;
    });
    ++ad_hoc_code; // never must be greater than 1

    return true;
  }

  Cache<Tree, int> cache = Cache<Tree, int>(3, 2s, 1s,
                                            miss_handler,
                                            hash_fct);

};

TEST_F(ComplexKey, key_creation)
{
  // a key with just a node
  auto tree_1 = create_key(1);
  ASSERT_EQ(tree_1.size(), 1);
  ASSERT_TRUE(tree_1.contains(1));

  // a key with 3 nodes
  auto tree_2 = create_key(1, 2, 3);
  ASSERT_EQ(tree_2.size(), 3);
  ASSERT_TRUE(tree_2.contains(1));
  ASSERT_TRUE(tree_2.contains(2));
  ASSERT_TRUE(tree_2.contains(3));
}

TEST_F(ComplexKey, hash_fct)
{
  auto tree_1 = create_key(1);
  auto tree_2 = create_key(11, 20, 30);

  cout << "Hash tree_1: " << hash_fct(tree_1) << endl
       << "Hash tree_2: " << hash_fct(tree_2) << endl;

  ASSERT_EQ(hash_fct(tree_1), hash_fct(tree_1));
  ASSERT_EQ(hash_fct(tree_2), hash_fct(tree_2));
  ASSERT_NE(hash_fct(tree_1), hash_fct(tree_2));
}

TEST_F(ComplexKey, cache)
{
  auto tree_1 = create_key(1);
  auto tree_2 = create_key(11, 20, 30);

  auto [data_1, ad_hoc_code_1] = cache.retrieve_from_cache_or_compute(tree_1);
  auto [data_2, ad_hoc_code_2] = cache.retrieve_from_cache_or_compute(tree_2);

  ASSERT_EQ(cache.size(), 2);
  ASSERT_TRUE(cache.has(tree_1));
  ASSERT_TRUE(cache.has(tree_2));

  ASSERT_EQ(*data_1, 1);
  ASSERT_EQ(*data_2, 61);

  ASSERT_EQ(ad_hoc_code_1, 1);
  ASSERT_EQ(ad_hoc_code_2, 1);
}

struct CookieHandler : public Test
{
  struct Parameters
  {
    int a;
    int b;
  };

  static bool miss_handler(const int &key, int *data,
                           int8_t &ad_hoc_code,
                           void *cookie)
  {
    Parameters *params = static_cast<Parameters *>(cookie);
    cout << "Params: " << params->a << " " << params->b << endl;

    *data = params->a + params->b;
    ++ad_hoc_code; // never must be greater than 1

    return true;
  }

  Cache<int, int> cache;

  CookieHandler()
    : cache(5, 1s, 1s, miss_handler)
  {
    // empty
  }
};
TEST_F(CookieHandler, cookie)
{
  Parameters params = {10, 20};
  auto cookie = static_cast<void *>(&params);

  auto [data, ad_hoc_code] =
    cache.retrieve_from_cache_or_compute(1, cookie);

  ASSERT_EQ(cache.size(), 1);
  ASSERT_TRUE(cache.has(1));

  ASSERT_EQ(*data, 30);
  ASSERT_EQ(ad_hoc_code, 1);
}
