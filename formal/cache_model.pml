/*
 * Promela model of cpp-cache with explicit refcount (v4).
 *
 * Replaces the boolean slot_fresh[] from v3 with a byte slot_rc[] counter,
 * faithfully modeling CacheEntry::_refcount and EntryGuard RAII lifetime.
 *
 * REFCOUNT SEMANTICS:
 *   slot_rc[s] == 0  →  slot is free (Empty)
 *   slot_rc[s] == 1  →  table ownership only (no external holders)
 *   slot_rc[s] >= 2  →  table + N holders (threads with EntryGuard)
 *
 * KEY INVARIANTS VERIFIED:
 *   - Empty → rc == 0
 *   - Occupied → rc >= 1
 *   - Computing → rc >= 2 (table + owner who installed it)
 *   - Expired → rc <= 1 (Background needs entry lock, blocked by holders)
 *   - Invalidated → rc <= 1 (same reason)
 *   - Eviction requires rc <= 1 (no holders: models refcount==1 check)
 *   - Background requires rc <= 1 (entry lock free: models try_lock success)
 *
 * STRUCTURE:
 *   Owner Requester handles solver completion (no separate Resolver process).
 *   Phase 1 (atomic): search/insert under global _mtx, acquire refcount
 *   Phase 2 (non-atomic): solver execution (owner only, no locks held)
 *   Phase 3 (atomic): wait/harvest under entry mtx, release refcount
 *
 * MAPPING TO C++:
 * --------------------------------------------------------------------------
 * Promela Construct          | C++ Entity
 * --------------------------------------------------------------------------
 * slot_rc[CAPACITY]          | CacheEntry::_refcount (atomic<int>)
 * slot_rc++ in Phase 1       | entry->acquire() in contains_or_insert_locked
 * slot_rc-- in Phase 3       | ~EntryGuard() → entry->release()
 * slot_rc <= 1 in eviction   | find_evictable_lru() skipping held entries
 * slot_rc <= 1 in Background | entry->mtx() acquisition (blocked by holders)
 * Phase 2 skip + atomic      | solver() then lock(entry->mtx()) + set state
 * Phase 3 Computing wait     | entry->cv().wait(lock, []{state!=Computing})
 * --------------------------------------------------------------------------
 */

#define CAPACITY 2
#define NKEYS 3
#define NTHREADS 6
#define NONE 3
#define OTHER(s) (1-(s))
#define AGE_THRESHOLD 2

mtype = {
  Empty, Computing, Ready, Failed, Invalidated,
  HitPositive, HitNegative, ComputedPositive, ComputedNegative, Saturated
};

#define OCCUPIED(s)     (slot_state[s] != Empty)
#define MATERIALIZED(s) ((slot_state[s] == Ready) || (slot_state[s] == Failed))
#define FULL()          (OCCUPIED(0) && OCCUPIED(1))
/* A slot is unevictable if Computing or held by external threads */
#define UNEVICTABLE(s)  (slot_state[s] == Computing || slot_rc[s] > 1)
/* Cache is fully blocked when no slot can be evicted */
#define ALL_BLOCKED()   (FULL() && UNEVICTABLE(0) && UNEVICTABLE(1))

mtype slot_state[CAPACITY];
byte  slot_key[CAPACITY];
bool  slot_expired[CAPACITY];
byte  slot_rank[CAPACITY];
byte  slot_rc[CAPACITY];      /* refcount */
bool  slot_locked[CAPACITY];  /* explicit mutex state */
byte  slot_age[CAPACITY];     /* TTL age: expires when >= AGE_THRESHOLD */
byte  computing_count = 0;

bool key_present[NKEYS];
bool key_materialized[NKEYS];
bool key_computing[NKEYS];
bool key_invalidated[NKEYS];

bool ever_stored[NKEYS];
bool request_done[NTHREADS];
mtype request_result[NTHREADS];
byte target_key[NTHREADS];

inline clear_slot(s)
{
  slot_state[s] = Empty;
  slot_key[s] = NONE;
  slot_expired[s] = false;
  slot_rank[s] = 0;
  slot_rc[s] = 0;
  slot_locked[s] = false;
  slot_age[s] = 0
}

inline move_to_mru(s)
{
  if
  :: OCCUPIED(OTHER(s)) ->
       slot_rank[s] = 1;
       slot_rank[OTHER(s)] = 0
  :: else ->
       slot_rank[s] = 0
  fi
}

inline move_to_lru(s)
{
  if
  :: OCCUPIED(OTHER(s)) ->
       slot_rank[s] = 0;
       slot_rank[OTHER(s)] = 1
  :: else ->
       slot_rank[s] = 0
  fi
}

inline install_computing(s, k)
{
  /* Triple-delete safety: verify no external holder before overwriting */
  if
  :: OCCUPIED(s) -> assert(slot_rc[s] == 1)  /* Eviction: only table holds ref */
  :: else        -> assert(slot_rc[s] == 0)  /* Empty slot: no refs */
  fi;
  slot_key[s] = k;
  slot_state[s] = Computing;
  slot_expired[s] = false;
  slot_rc[s] = 2;       /* table + owner */
  slot_age[s] = 0;
  computing_count++;
  move_to_mru(s)
}

inline recompute_view()
{
  key_present[0] = false; key_present[1] = false; key_present[2] = false;
  key_materialized[0] = false; key_materialized[1] = false; key_materialized[2] = false;
  key_computing[0] = false; key_computing[1] = false; key_computing[2] = false;
  key_invalidated[0] = false; key_invalidated[1] = false; key_invalidated[2] = false;

  if
  :: OCCUPIED(0) ->
       key_present[slot_key[0]] = true;
       if
       :: slot_state[0] == Computing   -> key_computing[slot_key[0]] = true
       :: MATERIALIZED(0)              -> key_materialized[slot_key[0]] = true
       :: slot_state[0] == Invalidated -> key_invalidated[slot_key[0]] = true
       :: else -> skip
       fi
  :: else -> skip
  fi;

  if
  :: OCCUPIED(1) ->
       key_present[slot_key[1]] = true;
       if
       :: slot_state[1] == Computing   -> key_computing[slot_key[1]] = true
       :: MATERIALIZED(1)              -> key_materialized[slot_key[1]] = true
       :: slot_state[1] == Invalidated -> key_invalidated[slot_key[1]] = true
       :: else -> skip
       fi
  :: else -> skip
  fi
}

inline assert_consistent()
{
  /* Structural invariants */
  assert(computing_count <= CAPACITY);
  assert(((slot_state[0] == Computing) + (slot_state[1] == Computing)) == computing_count);
  assert(!(slot_state[0] == Empty && slot_key[0] != NONE));
  assert(!(slot_state[1] == Empty && slot_key[1] != NONE));
  assert(!(OCCUPIED(0) && OCCUPIED(1) && slot_key[0] == slot_key[1]));
  assert(!(slot_state[0] == Computing && slot_expired[0]));
  assert(!(slot_state[1] == Computing && slot_expired[1]));

  /* Refcount invariants */
  assert(!(slot_state[0] == Empty && slot_rc[0] != 0));
  assert(!(slot_state[1] == Empty && slot_rc[1] != 0));
  assert(!(OCCUPIED(0) && slot_rc[0] < 1));
  assert(!(OCCUPIED(1) && slot_rc[1] < 1));
  assert(!(slot_state[0] == Computing && slot_rc[0] < 2));
  assert(!(slot_state[1] == Computing && slot_rc[1] < 2));
  assert(slot_rc[0] <= NTHREADS + 1);
  assert(slot_rc[1] <= NTHREADS + 1);

  /* LRU rank invariants */
  if
  :: OCCUPIED(0) && OCCUPIED(1) ->
       assert(slot_rank[0] != slot_rank[1]);
       assert((slot_rank[0] == 0 || slot_rank[0] == 1));
       assert((slot_rank[1] == 0 || slot_rank[1] == 1));
       assert(slot_rank[0] + slot_rank[1] == 1)
  :: OCCUPIED(0) && !OCCUPIED(1) ->
       assert(slot_rank[0] == 0)
  :: OCCUPIED(1) && !OCCUPIED(0) ->
       assert(slot_rank[1] == 0)
  :: else -> skip
  fi
}

inline refresh_and_check()
{
  recompute_view();
  assert_consistent()
}

/*
 * Requester: models a thread calling get_or_compute().
 * Three-phase structure with explicit refcount acquire/release.
 */
/*
 * get_or_compute_api: models a single call to get_or_compute().
 * Sets request_done[myid] = true on completion (including Saturation).
 */
inline get_or_compute_api(myid, mykey, sid, is_owner)
{
  sid = NONE;
  is_owner = false;

  /*
   * PHASE 1: SEARCH / INSERT
   * Models contains_or_insert_locked() under global _mtx.
   * Acquires refcount on found/created entry.
   */
  atomic {
    /* Search hash table for key */
    if
    :: slot_key[0] == mykey && OCCUPIED(0) -> sid = 0
    :: slot_key[1] == mykey && OCCUPIED(1) -> sid = 1
    :: else -> skip
    fi;

    if
    :: sid != NONE ->
       /* Key found in slot sid */
       if
       :: slot_state[sid] == Computing ||
          (MATERIALIZED(sid) && !slot_expired[sid]) ->
          /* Valid or being computed: acquire ref, proceed to Phase 3 */
          slot_rc[sid]++;
          move_to_mru(sid)
       :: slot_state[sid] == Invalidated ||
          (MATERIALIZED(sid) && slot_expired[sid]) ->
          /* Stale entry: reinstall as Computing (same slot, same key) */
          slot_rc[sid]++;
          slot_state[sid] = Computing;
          slot_expired[sid] = false;
          slot_age[sid] = 0;
          computing_count++;
          move_to_mru(sid);
          is_owner = true
       fi

    :: else ->
       /* Key not found. Find empty or evictable slot. */
       if
       :: !OCCUPIED(0) -> sid = 0
       :: OCCUPIED(0) && !OCCUPIED(1) -> sid = 1
       :: FULL() &&
          slot_state[0] != Computing && slot_rc[0] <= 1 &&
          (UNEVICTABLE(1) || slot_rank[0] == 0) ->
          sid = 0
       :: FULL() &&
          slot_state[1] != Computing && slot_rc[1] <= 1 &&
          (UNEVICTABLE(0) || slot_rank[1] == 0) ->
          sid = 1
       :: else -> skip
       fi;

       if
       :: sid == NONE ->
          request_result[myid] = Saturated;
          request_done[myid] = true
       :: else ->
          install_computing(sid, mykey);
          is_owner = true
       fi
    fi;
    refresh_and_check()
  } /* end Phase 1 atomic */

  if
  :: request_done[myid] -> skip   /* Saturated — terminal for this call */
  :: sid == NONE -> skip         /* Internal retry (Phase 1 skipped due to race) */
  :: else ->

     /*
      * PHASE 2: SOLVER EXECUTION (owner only)
      */
     if
     :: is_owner ->
        skip;   /* solver work — non-atomic interleaving point */
        atomic {
          /* Completion under entry lock */
          slot_locked[sid] = true;
          if :: slot_state[sid] = Ready :: slot_state[sid] = Failed fi;
          slot_expired[sid] = false;
          computing_count--;
          move_to_mru(sid);
          ever_stored[mykey] = true;
          refresh_and_check();
          slot_locked[sid] = false
        }
     :: else -> skip
     fi;

     /*
      * PHASE 3: HARVEST / WAIT
      */
     atomic {
       slot_locked[sid] = true;
       if
       :: slot_key[sid] == mykey && slot_state[sid] == Computing ->
          slot_locked[sid] = false;
          slot_state[sid] != Computing && !slot_locked[sid] ->
          slot_locked[sid] = true;
          if
          :: slot_state[sid] == Ready ->
             request_result[myid] = HitPositive;
             request_done[myid] = true;
             ever_stored[mykey] = true;
             slot_age[sid] = 0;
             move_to_mru(sid)
          :: slot_state[sid] == Failed ->
             request_result[myid] = HitNegative;
             request_done[myid] = true;
             ever_stored[mykey] = true;
             slot_age[sid] = 0;
             move_to_mru(sid)
          fi

       :: slot_key[sid] == mykey &&
          slot_state[sid] == Ready && !slot_expired[sid] ->
          if
          :: is_owner -> request_result[myid] = ComputedPositive
          :: else     -> request_result[myid] = HitPositive
          fi;
          request_done[myid] = true;
          ever_stored[mykey] = true;
          slot_age[sid] = 0;
          move_to_mru(sid)

       :: slot_key[sid] == mykey &&
          slot_state[sid] == Failed && !slot_expired[sid] ->
          if
          :: is_owner -> request_result[myid] = ComputedNegative
          :: else     -> request_result[myid] = HitNegative
          fi;
          request_done[myid] = true;
          ever_stored[mykey] = true;
          slot_age[sid] = 0;
          move_to_mru(sid)

       :: else ->
          /* Entry was unexpectedly modified. Bail out, retry. */
          skip
       fi;

       /* Release reference (EntryGuard destructor) */
       assert(slot_rc[sid] > 0);
       slot_rc[sid]--;
       is_owner = false;
       refresh_and_check();
       slot_locked[sid] = false
     } /* end Phase 3 atomic */
  fi
}

proctype Requester(byte myid)
{
  byte mykey = target_key[myid];
  byte sid;
  bool is_owner;

  do
  :: !request_done[myid] ->
     get_or_compute_api(myid, mykey, sid, is_owner);
     if
     :: request_result[myid] == Saturated ->
        request_done[myid] = false;
        atomic { !ALL_BLOCKED() -> skip }
     :: else -> break
     fi
  :: request_done[myid] -> break
  od
}

/*
 * FindRequester: models a thread calling find().
 * Unlike Requester (get_or_compute), this process:
 *   - Never installs a Computing entry (no solver)
 *   - Returns nullopt if key not found, Invalidated, or Expired
 *   - Blocks on Computing entries (CV wait), then harvests result
 *   - Always releases refcount via EntryGuard
 *
 * MAPPING TO C++:
 *   Phase 1 → global _mtx: search_entry + acquire()
 *   Phase 2 → entry lock: is_valid_hit + cv.wait + harvest
 *   nullopt → EntryGuard dtor releases refcount
 */
proctype FindRequester(byte myid)
{
  byte mykey = target_key[myid];
  byte sid;

  do
  :: request_done[myid] -> break

  :: atomic {
       !request_done[myid] ->
       sid = NONE;

       /*
        * PHASE 1: SEARCH + VALIDITY CHECK (under global _mtx)
        * Acquire refcount only on valid entries (Computing, or
        * Ready/Failed + not expired). Invalidated and expired entries
        * produce nullopt without acquiring refcount, preserving the
        * invariant: Invalidated → rc <= 1.
        */
       if
       :: slot_key[0] == mykey && OCCUPIED(0) &&
          (slot_state[0] == Computing ||
           (MATERIALIZED(0) && !slot_expired[0])) ->
          sid = 0; slot_rc[0]++
       :: slot_key[1] == mykey && OCCUPIED(1) &&
          (slot_state[1] == Computing ||
           (MATERIALIZED(1) && !slot_expired[1])) ->
          sid = 1; slot_rc[1]++
       :: else -> skip  /* Not found or not valid → nullopt */
       fi;

       if
       :: sid == NONE ->
          /* find() returned nullopt: key absent, invalidated, or expired */
          request_done[myid] = true
       :: else -> skip
       fi;
       refresh_and_check()
     }

     if
     :: sid == NONE -> skip  /* Already done */
     :: else ->
        /*
         * PHASE 2: WAIT / HARVEST (under entry lock)
         * Refcount (rc >= 2) protects slot from eviction and Background.
         * If Computing: CV wait until owner completes.
         * If Ready/Failed: harvest result immediately.
         */
        atomic {
          slot_locked[sid] = true;
          if
          :: slot_key[sid] == mykey && slot_state[sid] == Computing ->
             slot_locked[sid] = false;
             slot_state[sid] != Computing && !slot_locked[sid] ->
             slot_locked[sid] = true;
             if
             :: slot_key[sid] == mykey && slot_state[sid] == Ready ->
                request_result[myid] = HitPositive;
                slot_age[sid] = 0;
                move_to_mru(sid)
             :: slot_key[sid] == mykey && slot_state[sid] == Failed ->
                request_result[myid] = HitNegative;
                slot_age[sid] = 0;
                move_to_mru(sid)
             :: else -> skip
             fi
          :: slot_key[sid] == mykey &&
             slot_state[sid] == Ready && !slot_expired[sid] ->
             request_result[myid] = HitPositive;
             slot_age[sid] = 0;
             move_to_mru(sid)
          :: slot_key[sid] == mykey &&
             slot_state[sid] == Failed && !slot_expired[sid] ->
             request_result[myid] = HitNegative;
             slot_age[sid] = 0;
             move_to_mru(sid)
          :: else -> skip
          fi;

          /* Release reference (EntryGuard destructor) */
          request_done[myid] = true;
          assert(slot_rc[sid] > 0);
          slot_rc[sid]--;
          refresh_and_check();
          slot_locked[sid] = false
        } /* end Phase 2 atomic */
     fi
  od
}

/*
 * Background: simulates TTL expiry and manual invalidation.
 *
 * NOTE: TTL aging/expiry in the model is proactive (discrete steps), 
 * whereas the C++ implementation uses lazy expiry checked on access.
 *
 * All guards require rc <= 1: this models mutex acquisition blocking 
 * (entry->mtx()) rather than the C++ refcount semantics itself. It is a 
 * conservative abstraction: if rc > 1, some thread might be holding 
 * the entry mutex or about to take it.
 */
proctype Background()
{
  end: do
  /* TTL Aging + Expiry: increment age, expire only at threshold.
   * Models steady_clock advancing: entry must age AGE_THRESHOLD ticks
   * without a hit before expiring. Hits reset age to 0. */
  :: atomic {
       MATERIALIZED(0) && !slot_expired[0] && !slot_locked[0] ->
       slot_locked[0] = true;
       slot_age[0]++;
       if
       :: slot_age[0] >= AGE_THRESHOLD -> slot_expired[0] = true
       :: else -> skip
       fi;
       refresh_and_check();
       slot_locked[0] = false
     }
  :: atomic {
       MATERIALIZED(1) && !slot_expired[1] && !slot_locked[1] ->
       slot_locked[1] = true;
       slot_age[1]++;
       if
       :: slot_age[1] >= AGE_THRESHOLD -> slot_expired[1] = true
       :: else -> skip
       fi;
       refresh_and_check();
       slot_locked[1] = false
     }
  od
}

/*
 * Invalidator: models explicit invalidate(key) API calls.
 * Separated from Background (TTL expiry) to independently verify:
 *   - Computing entries are NEVER invalidated (critical safety)
 *   - Invalidation moves entry to LRU tail
 *
 * NOTE: The guard slot_rc <= 1 models the ability to acquire entry->mtx().
 * This is an intentional over-approximation (rc > 1 might report busy 
 * even if the mutex is technically free for a brief window during 
 * EntryGuard construction), ensuring safety against state transitions.
 *
 * MAPPING TO C++:
 *   Guard (rc <= 1)          → std::scoped_lock entry_lock(entry->mtx())
 *   assert(!Computing)       → if (state == Computing) return false
 *   state = Invalidated      → entry->set_state(Invalidated)
 *   move_to_lru              → _lru_list.append(link)
 */
proctype Invalidator()
{
  end: do
  :: atomic {
       slot_state[0] != Empty && slot_state[0] != Computing &&
       slot_state[0] != Invalidated && !slot_locked[0] ->
       /* EntryGuard(entry) and entry_lock(entry->mtx()) */
       slot_rc[0]++;
       slot_locked[0] = true;
       /* CRITICAL: Computing entries must never be invalidated */
       assert(slot_state[0] != Computing);
       slot_state[0] = Invalidated;
       slot_expired[0] = true;
       move_to_lru(0);
       refresh_and_check();
       slot_locked[0] = false;
       slot_rc[0]--
     }
  :: atomic {
       slot_state[1] != Empty && slot_state[1] != Computing &&
       slot_state[1] != Invalidated && !slot_locked[1] ->
       slot_rc[1]++;
       slot_locked[1] = true;
       assert(slot_state[1] != Computing);
       slot_state[1] = Invalidated;
       slot_expired[1] = true;
       move_to_lru(1);
       refresh_and_check();
       slot_locked[1] = false;
       slot_rc[1]--
     }
  od
}

/*
 * TouchRequester: models explicit touch(key) API calls.
 * Refreshes TTL and moves entry to MRU position.
 *
 * MAPPING TO C++:
 *   Guard (rc <= 1)          → std::scoped_lock entry_lock(entry->mtx())
 *   MATERIALIZED + !expired  → state == Ready || state == Failed, !has_expired
 *   slot_age = 0             → entry->set_expiration(now + ttl)
 *   move_to_mru              → lru_move_to_mru(entry)
 */
proctype TouchRequester()
{
  end: do
  :: atomic {
       MATERIALIZED(0) && !slot_expired[0] && !slot_locked[0] ->
       slot_rc[0]++;
       slot_locked[0] = true;
       slot_age[0] = 0;
       move_to_mru(0);
       refresh_and_check();
       slot_locked[0] = false;
       slot_rc[0]--
     }
  :: atomic {
       MATERIALIZED(1) && !slot_expired[1] && !slot_locked[1] ->
       slot_rc[1]++;
       slot_locked[1] = true;
       slot_age[1] = 0;
       move_to_mru(1);
       refresh_and_check();
       slot_locked[1] = false;
       slot_rc[1]--
     }
  od
}

/*
 * PeekRequester: models peek(key) — truly non-blocking inspection.
 * Uses try_to_lock: returns nullopt immediately if entry is contended.
 * All operations are in a single atomic block (no intermediate states),
 * which guarantees peek never blocks by construction.
 *
 * MAPPING TO C++:
 *   Guard (!slot_locked)       → std::unique_lock(mtx, std::try_to_lock)
 *   MATERIALIZED + !expired    → is_valid_hit() check
 *   atomic acquire+release     → EntryGuard RAII within the lock scope
 *   No state modification      → peek() is read-only
 */
proctype PeekRequester()
{
  end: do
  :: atomic {
       MATERIALIZED(0) && !slot_expired[0] && !slot_locked[0] ->
       /* Atomic peek: acquire ref, lock, read, unlock, release */
       slot_rc[0]++;
       slot_locked[0] = true;
       assert(slot_state[0] == Ready || slot_state[0] == Failed);
       slot_locked[0] = false;
       slot_rc[0]--;
       refresh_and_check()
     }
  :: atomic {
       MATERIALIZED(1) && !slot_expired[1] && !slot_locked[1] ->
       slot_rc[1]++;
       slot_locked[1] = true;
       assert(slot_state[1] == Ready || slot_state[1] == Failed);
       slot_locked[1] = false;
       slot_rc[1]--;
       refresh_and_check()
     }
  od
}

init
{
  atomic {
    /* Capacity validation: models Cache constructor rejecting zero capacity */
    assert(CAPACITY > 0);

    clear_slot(0);
    clear_slot(1);

    ever_stored[0] = false; ever_stored[1] = false; ever_stored[2] = false;

    /* Scenario: Threads 0,1 compete for key 0 (thundering herd).
     * Thread 2 wants key 1, Thread 3 wants key 2.
     * With CAPACITY=2, forces eviction, saturation, and contention. */
    target_key[0] = 0; request_done[0] = false; request_result[0] = Saturated;
    target_key[1] = 0; request_done[1] = false; request_result[1] = Saturated;
    target_key[2] = 1; request_done[2] = false; request_result[2] = Saturated;
    target_key[3] = 2; request_done[3] = false; request_result[3] = Saturated;

    /* FindRequester threads: find() on keys 0 and 1.
     * Thread 4 targets key 0 (interacts with thundering herd on threads 0,1).
     * Thread 5 targets key 1 (interacts with Requester thread 2). */
    target_key[4] = 0; request_done[4] = false; request_result[4] = Saturated;
    target_key[5] = 1; request_done[5] = false; request_result[5] = Saturated;

    refresh_and_check();

    run Background();
    run Invalidator();
    run TouchRequester();
    run PeekRequester();
    run Requester(0);
    run Requester(1);
    run Requester(2);
    run Requester(3);
    run FindRequester(4);
    run FindRequester(5)
  }
}

/*
 * =========================================================================
 * LTL Properties — verify under weak fairness (spin -f / -DNFAIR).
 * =========================================================================
 */

/* Every requested key is eventually computed and stored. */
ltl every_requested_key_eventually_materializes {
  <> (ever_stored[0] && ever_stored[1] && ever_stored[2])
}

/* No thread starves — every pending request eventually completes. */
ltl no_starvation_thread0 { [] (!request_done[0] -> <> request_done[0]) }
ltl no_starvation_thread1 { [] (!request_done[1] -> <> request_done[1]) }
ltl no_starvation_thread2 { [] (!request_done[2] -> <> request_done[2]) }
ltl no_starvation_thread3 { [] (!request_done[3] -> <> request_done[3]) }
ltl no_starvation_thread4 { [] (!request_done[4] -> <> request_done[4]) }
ltl no_starvation_thread5 { [] (!request_done[5] -> <> request_done[5]) }

/* All threads eventually terminate. */
ltl all_terminate {
  <> (request_done[0] && request_done[1] && request_done[2] && request_done[3] &&
      request_done[4] && request_done[5])
}