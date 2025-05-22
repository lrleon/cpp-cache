//
// Created by lrleon on 11/4/24.
//

#ifndef CPP_CACHE_CACHE_H
#define CPP_CACHE_CACHE_H

# include <chrono>
# include <memory>
# include <mutex>
# include <condition_variable>
# include <aleph.H>
# include <tpl_dnode.H>

# include <tpl_olhash.H>

#include <utility>

# include <gtest/gtest.h>

using namespace std;
using namespace Aleph;
using namespace std::chrono;

/* This is an implementation of a table-based associative cache.

   The cache handles <Key, Data> pairs where Key is the key
   associative and Data is the data related to Key.
   Are not allowed
   duplicate <Key, Data> pairs, but it is possible to have key
   duplicates.

   The cache has a size defined by cache_size specified in the
   builder.
   When the number of pairs inserted into the cache
   reaches cache_size, then the cache is said to be full.
   If
   tries to insert a new pair into a full cache, then it must
   delete a pair.
   In this implementation, the minus torque is eliminated
   recently used (lru).

   The implementation is based on a hash table with resolution of
   collisions by separate chaining.
   Each bucket in the table
   stores the pair.
   Additionally, the bucket contains a link that
   acts as a connector to a doubly linked list that simulates the
   lru order.
   Additionally, the bucket contains a direct link to the cache
   used to update your statistics.

   Pairs can be "locked"; that is: when a pair is
   blocked, it cannot be removed from the cache until it is
   released.
   A locked bucket will never be selected for
   replacement by lru policy.
*/
template<class Key, class Data, class Cmp = std::equal_to<Key>>
class Cache
{
  FRIEND_TEST(SimpleFixture, basic);
  FRIEND_TEST(SimpleFixture, lru);
  FRIEND_TEST(SimpleFixture, touch);
  FRIEND_TEST(cache_entry, basic);
  FRIEND_TEST(cache_entry, key_copy_works);
  FRIEND_TEST(cache_entry, key_move_works);
  FRIEND_TEST(cache_entry, data_copy_works);
  FRIEND_TEST(cache_entry, data_move_works);
  FRIEND_TEST(SimpleFixture, get_cache_entry);
  FRIEND_TEST(TimeConsumingFixture, calculating_status_while_computing);
  FRIEND_TEST(TimeConsumingFixture, multithread_heavy_threads);

  class Entry
  {
    private:
      Key _key = Key();
      Data _data = Data();

    public:
      Entry(const Key &k)
        : _key(k)
      {
        // empty
      }

      Entry(Key &&k)
        : _key(std::move(k))
      {
        // empty
      }

      Entry(const Entry &other)
        : _key(other._key), _data(other._data)
      {
        // empty
      }

      Entry(Entry &&other) noexcept
        : _key(std::move(other._key)), _data(std::move(other._data))
      {
        // empty
      }

      Entry& operator=(const Entry &other)
      {
        if (this == &other)
          return *this;

        _key = other._key;
        _data = other._data;

        return *this;
      }

      Entry& operator=(Entry &&other) noexcept
      {
        if (this == &other)
          return *this;

        _key = std::move(other._key);
        _data = std::move(other._data);

        return *this;
      }

      Entry() = default;

      const Key& key() const { return _key; }

      const Data& get_data() const { return _data; }

      Data& data() { return _data; }

      Data *data_ptr() { return &_data; }

      void set_key(const Key &k) { _key = k; }

      void set_key(Key &&k) { _key = std::move(k); }

      void set_data(const Data &d)
      {
        _data = d;
      }

      void set_data(Data &&d)
      {
        _data = move(d);
      }

      void swap(Entry &other) noexcept
      {
        using std::swap;
        swap(this->_key, other._key);
        swap(this->_data, other._data);
      }

      friend void swap(Entry &lhs, Entry &rhs) noexcept
      {
        using std::swap;
        swap(lhs._key, rhs._key);
        swap(lhs._data, rhs._data);
      }

      // assuming data is a pointer to the field _data of Entry class,
      // it returns a pointer to Entry
      static Entry *to_Entry(Data &data)
      {
        return reinterpret_cast<Entry*>(reinterpret_cast<byte*>(&data) - offsetof(Entry, _data));
      }
  }; // end class Entry

  class CacheEntry : public Entry
  {
    FRIEND_TEST(cache_entry, basic);

    friend struct SimpleFixture;

    friend class Cache<Key, Data, Cmp>;

    Dlink _dlink_lru; // dlink to lru queue

    mutex _mtx; // protects the CacheEntry while the calculation of the data is being done
    condition_variable _waiting_cv; // used for wake-up invoker waiting for the data is ready

    uint8_t _status = static_cast<uint8_t>(Status::AVAILABLE);
    // status of the cache entry (AVAILABLE, CALCULATING, READY, FAILED)
    int8_t _ad_hoc_code = 0; // ad hoc code to be used by the user for indicating their own codes

    time_point<high_resolution_clock> _ttl_exp_time; // when ttl expires

    public:
      static CacheEntry *to_CacheEntry(Data &data)
      {
        return reinterpret_cast<CacheEntry*>(Entry::to_Entry(data));
      }

      enum class Status
      {
        AVAILABLE, CALCULATING, READY, FAILED
      };

      static string status_to_string(Status status)
      {
        switch (status)
          {
          case Status::AVAILABLE:
            return "AVAILABLE";
          case Status::CALCULATING:
            return "CALCULATING";
          case Status::READY:
            return "READY";
          case Status::FAILED:
            return "FAILED";
          default:
            return "UNKNOWN";
          }
      }

      CacheEntry()
      {
        assert(_dlink_lru.is_empty());
      }

      CacheEntry(const Key &k)
        : Entry(k)
      {
        // empty
      }

      CacheEntry(Key &&k)
        : Entry(std::move(k))
      {
        // empty
      }

      CacheEntry(Key &&k, Data &&d)
        : Entry(std::move(k))
      {
        this->set_data(std::move(d));
      }

      CacheEntry(const CacheEntry &other)
        : Entry(other), _dlink_lru(other._dlink_lru),
          _status(other._status), _ad_hoc_code(other._ad_hoc_code),
          _ttl_exp_time(other._ttl_exp_time)
      {
        // empty
      }

      CacheEntry(CacheEntry &&other) noexcept
        : Entry(std::move(other)), _dlink_lru(std::move(other._dlink_lru)),
          _status(other._status), _ad_hoc_code(other._ad_hoc_code),
          _ttl_exp_time(other._ttl_exp_time)
      {
        // empty
      }

      CacheEntry& operator=(const CacheEntry &other)
      {
        if (this == &other)
          return *this;

        Entry::operator=(other);
        _dlink_lru = other._dlink_lru;
        _status = other._status;
        _ad_hoc_code = other._ad_hoc_code;
        _ttl_exp_time = other._ttl_exp_time;

        return *this;
      }

      CacheEntry& operator=(CacheEntry &&other) noexcept
      {
        if (this == &other)
          return *this;

        Entry::operator=(std::move(other));
        _dlink_lru = std::move(other._dlink_lru);
        _status = other._status;
        _ad_hoc_code = other._ad_hoc_code;
        _ttl_exp_time = other._ttl_exp_time;

        return *this;
      }

      bool operator==(const CacheEntry &other) const
      {
        return Cmp()(this->key(), other.key());
      }

      friend void
      swap(typename Cache<Key, Data, Cmp>::CacheEntry &lhs,
           typename Cache<Key, Data, Cmp>::CacheEntry &rhs) noexcept
      {
        lhs.swap(rhs);
      }

      void swap(CacheEntry &other) noexcept
      {
        this->Entry::swap(other);
        std::swap(this->_dlink_lru, other._dlink_lru);
        std::swap(this->_status, other._status);
        std::swap(this->_ad_hoc_code, other._ad_hoc_code);
        std::swap(this->_ttl_exp_time, other._ttl_exp_time);
      }

      Dlink *link_lru() { return &_dlink_lru; }

      mutex& mtx() { return _mtx; }

      condition_variable& waiting_cv() { return _waiting_cv; }

      Status status() const { return static_cast<Status>(_status); }

      void set_status(const Status &status)
      {
        _status = static_cast<uint8_t>(status);
      }

      int8_t& ad_hoc_code() { return _ad_hoc_code; }

      time_point<high_resolution_clock> ttl_exp_time() const noexcept
      {
        return _ttl_exp_time;
      }

      bool has_ttl_expired(const high_resolution_clock::time_point &now) const
      {
        const auto ret = now > _ttl_exp_time;
        return ret;
      }

      void set_ttl_exp_time(const time_point<high_resolution_clock> &exp_time)
      {
        _ttl_exp_time = exp_time;
      }
  }; // end class CacheEntry

  struct CacheCmp
  {
    bool operator()(const CacheEntry &lhs, const CacheEntry &rhs) const
    {
      return Cmp()(lhs.key(), rhs.key());
    }
  };

  // ********** data members of Cache class

  Dlink lru_list; // lru list

  const size_t cache_size; // cache length; MUST be less than hash_table.capacity()

  OLhashTable<CacheEntry, Cache::CacheCmp> hash_table;

  seconds positive_ttl;
  seconds negative_ttl;

  mutex mtx; // protects the cache

  protected:
    LINKNAME_TO_TYPE(CacheEntry, _dlink_lru);

    void insert_entry_to_lru_list(CacheEntry *cache_entry)
    {
      lru_list.insert(cache_entry->link_lru());
    }

    void move_to_lru_front(CacheEntry *cache_entry)
    {
      cache_entry->link_lru()->del();
      lru_list.insert(cache_entry->link_lru());
    }

    void do_mru(CacheEntry *cache_entry)
    {
      move_to_lru_front(cache_entry);
    }

    // removes from the hash table and lru list
    void remove_entry_from_hash_table(unique_lock<mutex> &taken_lock,
                                      CacheEntry *cache_entry)
    {
      cache_entry->waiting_cv().wait(taken_lock, [cache_entry]
      {
        return cache_entry->status() != CacheEntry::Status::CALCULATING;
      });

      assert(cache_entry != nullptr);
      assert(cache_entry->status() == CacheEntry::Status::READY or
        cache_entry->status() == CacheEntry::Status::FAILED);
      assert(not cache_entry->link_lru()->is_empty());

      // we need to clean the entry before removing it so that a future
      // insertion of the same key gets a fresh entry
      cache_entry->set_status(CacheEntry::Status::AVAILABLE);
      cache_entry->ad_hoc_code() = 0;
      new(cache_entry->data_ptr()) Data(); // reset data

      cache_entry->link_lru()->del();

      hash_table.remove(*cache_entry);
    }

    // searches and returns the next entry according to lru priority that does not have
    // a lock, removes from the hash table and makes the entry the most
    // recently used.
    CacheEntry *get_lru_entry()
    {
      assert(not lru_list.is_empty());

      Dlink *lru_entry_link = lru_list.get_prev();
      CacheEntry *cache_entry = _dlink_lru_to_CacheEntry(lru_entry_link);

      return cache_entry;
    }

    CacheEntry *get_mru_entry()
    {
      assert(hash_table.size() <= cache_size);
      assert(not lru_list.is_empty());

      Dlink *mru_entry_link = lru_list.get_next();
      CacheEntry *cache_entry = _dlink_lru_to_CacheEntry(mru_entry_link);

      return cache_entry;
    }

    // returns true if the entry has expired
    static bool has_entry_ttl_expired(CacheEntry *cache_entry,
                                      const high_resolution_clock::time_point &time_now)
      noexcept
    {
      return cache_entry->has_ttl_expired(time_now);
    }

    template<typename Fct>
    static
    size_t entry_hash_fct(Fct fct, const CacheEntry &entry) noexcept
    {
      return fct(entry.key());
    }

  public:
    // MissHandlerType is a function that is called when a key is not found in
    // the cache. The function receives the key, a pointer to the data,
    // a reference to an ad hoc code, and a parameter pack of arguments.

    using MissHandlerType =
    std::function<bool(const Key &, Data *, int8_t &, void *)>;

    static constexpr float ratio = 1.3f;

    MissHandlerType miss_handler;

    using Hash_Fct = std::function<size_t(const Key &)>;
    using Hash_Fct_Ptr = size_t (*)(const Key &);

    using C = Cache;

    Cache(const size_t len,
          const seconds &positive_ttl,
          const seconds &negative_ttl,
          MissHandlerType miss_handler,
          Hash_Fct_Ptr hash_fct_ptr = dft_hash_fct<Key>)
      : cache_size(len),
        hash_table(ratio * len,
                   std::bind_front(C::template entry_hash_fct<Hash_Fct>,
                                   hash_fct_ptr),
                   CacheCmp(),
                   hash_default_lower_alpha,
                   hash_default_upper_alpha,
                   false),
        positive_ttl(positive_ttl), negative_ttl(negative_ttl),
        miss_handler(move(miss_handler))
    {
      assert(len > 1);
    }

  private:
    // Takes the global mutex
    // Search key in the hash table and if not found, insert it and second is false;
    // Otherwise, second is true (key is already in the hash table).
    pair<CacheEntry*, bool> thread_safe_contains_or_insert_in_hash_table(const Key &key)
    {
      scoped_lock lock(mtx);

      assert(hash_table.size() <= cache_size);

      CacheEntry *cache_entry = static_cast<CacheEntry*>(hash_table.search(key));
      if (const bool is_in_table = cache_entry != nullptr)
        {
          do_mru(cache_entry); // That would prevent another thread to evict by LRU
          return make_pair(cache_entry, true);
        }

      if (const bool is_cache_full = hash_table.size() == cache_size)
        {
          // we need to evict the LRU entry
          CacheEntry *lru_entry = get_lru_entry();
          unique_lock<mutex> entry_lock(lru_entry->mtx());

          ah_runtime_error_if(lru_entry->status() == CacheEntry::Status::CALCULATING)
            << "CacheEntry is being calculated, cannot evict it";

          assert(hash_table.size() == cache_size);
          remove_entry_from_hash_table(entry_lock, lru_entry);
          assert(hash_table.size() == cache_size - 1);
          cout << "**** Evicted entry " << (int) lru_entry->key() << endl;
        }

      // key is was not found in the cache (it was inserted by contains_or_insert())
      cache_entry = hash_table.insert(CacheEntry(key));
      insert_entry_to_lru_list(cache_entry); // <-- this does cache entry the MRU entry

      return make_pair(cache_entry, false);
    }

  public:
    // Insert a pair <key, data> into the cache. If successful, it returns a pointer
    // to the data in the cache. Otherwise, it returns nullptr.
    Data *insert(Key &&key, Data &&data)
    {
      using CacheEntry = typename Cache<Key, Data, Cmp>::CacheEntry;
      assert(hash_table.size() <= cache_size);

      pair<CacheEntry*, bool> p = thread_safe_contains_or_insert_in_hash_table(move(key));

      if (const bool is_in_table = p.second)
        return nullptr;

      CacheEntry *cache_entry = p.first;
      scoped_lock entry_lock(cache_entry->mtx());
      cache_entry->set_data(std::move(data));
      cache_entry->set_status(CacheEntry::Status::READY);
      cache_entry->set_ttl_exp_time(high_resolution_clock::now() + positive_ttl);

      return cache_entry->data_ptr();
    }

    bool has(const Key &key)
    {
      assert(hash_table.size() <= cache_size);

      const CacheEntry entry(key);

      scoped_lock lock(mtx);

      auto cache_entry = hash_table.search(entry);

      if (cache_entry == nullptr)
        return false;

      unique_lock<mutex> entry_lock(cache_entry->mtx());
      if (not has_entry_ttl_expired(cache_entry, high_resolution_clock::now()))
        return true;

      remove_entry_from_hash_table(entry_lock, cache_entry);
      return false;
    }

    bool touch(const Key &key)
    {
      scoped_lock lock(mtx);

      assert(hash_table.size() <= cache_size);

      auto *cache_entry = static_cast<CacheEntry*>(hash_table.search(key));

      if (cache_entry == nullptr)
        return false;

      scoped_lock entry_lock(cache_entry->mtx());
      if (not has_entry_ttl_expired(cache_entry, high_resolution_clock::now()))
        {
          do_mru(cache_entry);
          return true;
        }
      return false;
    };

  private:
    pair<Key, Data>
    get_extreme_from_lrl_list(CacheEntry *(Cache<Key, Data, Cmp>::*get_entry)())
    {
      assert(hash_table.size() <= cache_size);

      ah_domain_error_if(lru_list.is_empty())
        << "get_extreme_from_lrl_list() helper called on an empty lru list";

      scoped_lock lock(mtx);

      auto *cache_entry = (this->*get_entry)();

      return make_pair(cache_entry->key(), cache_entry->data());
    }

  public:
    pair<Key, Data> get_lru()
    {
      return get_extreme_from_lrl_list(&Cache<Key, Data, Cmp>::get_lru_entry);
    }

    pair<Key, Data> get_mru()
    {
      return get_extreme_from_lrl_list(&Cache<Key, Data, Cmp>::get_mru_entry);
    }

  private:
    // Handles the entry when it is not found on the cache.
    // Returns the calculated data.
    Data *resolve_cache_miss(unique_lock<mutex> &taken_entry_lock,
                             CacheEntry *cache_entry,
                             const high_resolution_clock::time_point &time_now,
                             void *cookie)
    {
      Data *result_ptr = nullptr;

      switch (cache_entry->status())
        {
        case CacheEntry::Status::AVAILABLE:
          cache_entry->set_status(CacheEntry::Status::CALCULATING);
          if (miss_handler(cache_entry->key(), cache_entry->data_ptr(),
                           cache_entry->ad_hoc_code(), cookie))
            {
              cache_entry->set_ttl_exp_time(time_now + positive_ttl);
              cache_entry->set_status(CacheEntry::Status::READY);
            }
          else
            {
              cache_entry->set_ttl_exp_time(time_now + negative_ttl);
              cache_entry->set_status(CacheEntry::Status::FAILED);
            }

          result_ptr = cache_entry->data_ptr();

          cache_entry->waiting_cv().notify_all(); // wake up waiting threads associated with the key

          break;

        case CacheEntry::Status::CALCULATING:
          {
            // Result is not ready yet, wait for its completion
            cache_entry->waiting_cv().
              wait(taken_entry_lock, [cache_entry]
              {
                return cache_entry->status() != CacheEntry::Status::CALCULATING;
              });
            result_ptr = cache_entry->data_ptr();
          }
          break;

        case CacheEntry::Status::READY:
          result_ptr = cache_entry->data_ptr();
          break;

        case CacheEntry::Status::FAILED:
          result_ptr = nullptr;
          break;

        default:
          ah_fatal_error()
            << "unknown status " << static_cast<int>(cache_entry->status());
          break;
        }

      return result_ptr;
    }

    bool evict_lru_entry()
    {
      auto *lru_entry = get_lru_entry();
      if (lru_entry == nullptr)
        return false;

      lru_entry->link_lru()->del();
      hash_table.remove(lru_entry->key()); // remove from hash table as well
      lru_entry->set_status(CacheEntry::Status::AVAILABLE);
      lru_entry->ad_hoc_code() = 0;
      new(lru_entry->data_ptr()) Data(); // reset data

      return true;
    }

    CacheEntry *allocate_entry(const Key &key)
    {
      if (hash_table.size() == cache_size)
        {
          auto *lru_entry = get_lru_entry();
          unique_lock<mutex> entry_lock(lru_entry->mtx());
          remove_entry_from_hash_table(entry_lock, lru_entry);
        }

      return hash_table.insert(CacheEntry(key));
    }

  public:
    // computed/retrieved data, ad hoc status set by the miss handler
    pair<Data*, int8_t>
    retrieve_from_cache_or_compute(const Key &key, void *cookie = nullptr)
    {
      CacheEntry *cache_entry = nullptr;
      const auto time_now = high_resolution_clock::now();

      mtx.lock();
      cache_entry = static_cast<CacheEntry*>(hash_table.search(key));
      if (cache_entry != nullptr) // possible cache hit
        {
          do_mru(cache_entry);

          mtx.unlock(); // unlock before the wait

          unique_lock entry_lock(cache_entry->mtx());
          cache_entry->waiting_cv().wait(entry_lock,
                                         [cache_entry]
                                         {
                                           return cache_entry->status() != CacheEntry::Status::CALCULATING;
                                         });

          cache_entry->set_ttl_exp_time(time_now + (cache_entry->status() == CacheEntry::Status::READY
                                                      ? positive_ttl
                                                      : negative_ttl));

          return make_pair(cache_entry->data_ptr(), cache_entry->ad_hoc_code());
        }

      cache_entry = allocate_entry(key);
      if (cache_entry == nullptr)
        {
          mtx.unlock();
          return make_pair(nullptr, 0);
        }

      unique_lock<mutex> entry_lock(cache_entry->mtx());
      cache_entry->set_status(CacheEntry::Status::CALCULATING);
      mtx.unlock();

      const bool miss_result =
        miss_handler(key, cache_entry->data_ptr(), cache_entry->ad_hoc_code(), cookie);
      if (not miss_result)
        {
          cache_entry->set_ttl_exp_time(time_now + negative_ttl);
          cache_entry->set_status(CacheEntry::Status::FAILED);
        }
      else
        {
          cache_entry->set_ttl_exp_time(time_now + positive_ttl);
          cache_entry->set_status(CacheEntry::Status::READY);
        }

      cache_entry->waiting_cv().notify_all(); // wake up waiting threads associated with the key

      return make_pair(cache_entry->data_ptr(), cache_entry->ad_hoc_code());
    }

    void remove(const Key &key)
    {
      scoped_lock lock(mtx);

      CacheEntry entry(key);

      auto *cache_entry = static_cast<CacheEntry*>(hash_table.search(entry));

      if (cache_entry == nullptr)
        return;

      unique_lock<mutex> entry_lock(cache_entry->mtx());

      if (cache_entry->status() == CacheEntry::Status::CALCULATING)
        {
          cache_entry->waiting_cv().
            wait_for(entry_lock,
                     std::chrono::seconds(5),
                     [cache_entry]
                     {
                       return cache_entry->status() != CacheEntry::Status::CALCULATING;
                     });

          ah_runtime_error_if(cache_entry->status() == CacheEntry::Status::CALCULATING)
            << "CacheEntry is still in CALCULATING status after 5 seconds";
        }

      remove_entry_from_hash_table(entry_lock, cache_entry);
    }

    const size_t& capacity() const { return cache_size; }

    size_t size() const { return hash_table.size(); }

    const size_t& get_num_busy_slots() const
    {
      return hash_table.get_num_busy_slots();
    }

    // Iterator to traverse the cache. It is not thread-safe.
    struct Iterator : public OLhashTable<CacheEntry, CacheCmp>::Iterator
    {
      Iterator(Cache &_cache)
        : OLhashTable<CacheEntry, CacheCmp>::Iterator(_cache.hash_table)
      {
        // empty
      }

      pair<const Key &, Data*> get_curr()
      {
        CacheEntry &cache_entry =
          OLhashTable<CacheEntry, CacheCmp>::Iterator::get_curr();

        return make_pair(cache_entry.key(), &cache_entry.data());
      }
    };

    Iterator get_it()
    {
      return Iterator(*this);
    }

    // Mutex to protect the cache. It could be necessary to protect the cache if
    // the user wants to use the iterator. Use it at your own risk.
    mutex& get_mtx() { return mtx; }
};

#endif // CPP_CACHE_CACHE_H
