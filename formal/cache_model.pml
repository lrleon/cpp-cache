/*
 * Promela model of cpp-cache with explicit refcount (v5).
 *
 * CHANGES FROM v4:
 *   - Phase 1 no longer transitions expired/Invalidated entries to Computing.
 *     Instead, it acquires refcount and defers state handling to Phase 2
 *     (resolve_hit under entry lock), faithfully modeling the C++ flow where
 *     contains_or_insert_locked() returns {entry, true} regardless of state.
 *   - Background/Invalidator/TouchRequester/PeekRequester no longer require
 *     slot_rc <= 1.  They only require !slot_locked (entry mutex free),
 *     correctly decoupling refcount from entry-lock availability as in C++.
 *   - Added HasRequester process modeling has() API.
 *   - Added liveness: no_orphaned_computing, saturation_transient.
 *   - Removed spurious move_to_mru from hit harvest — matches C++ where
 *     move_to_mru for get_or_compute hits is only in contains_or_insert_locked.
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
 *   - Eviction requires rc <= 1 (no holders: models refcount==1 check)
 *   - Entry lock (!slot_locked) is independent of refcount
 *   - Background/Invalidator/Touch/Peek acquire entry lock, not blocked by rc
 *
 * STRUCTURE:
 *   Phase 1 (atomic): search/insert under global _mtx, acquire refcount
 *   Phase 2 (atomic): resolve_hit under entry lock (existed entries only)
 *   Phase 3 (non-atomic + atomic): solver execution + state update (owner)
 *   Phase 4 (atomic): release refcount (EntryGuard destructor)
 *
 * MAPPING TO C++:
 * --------------------------------------------------------------------------
 * Promela Construct          | C++ Entity
 * --------------------------------------------------------------------------
 * slot_rc[CAPACITY]          | CacheEntry::_refcount (atomic<int>)
 * slot_rc++ in Phase 1       | entry->acquire() in contains_or_insert_locked
 * slot_rc-- in Phase 4       | ~EntryGuard() → entry->release()
 * slot_rc <= 1 in eviction   | find_evictable_lru() skipping held entries
 * !slot_locked in Bg/Inv/... | entry->mtx() availability (independent of rc)
 * Phase 2 validity check     | resolve_hit() under entry lock
 * Phase 2 expired→Computing  | resolve_hit nullopt → resolve_miss set Computing
 * Phase 2 Computing wait     | entry->cv().wait(lock, []{state!=Computing})
 * Phase 3 solver + update    | solver() then lock(entry->mtx()) + set state
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
bool  slot_locked[CAPACITY];  /* per-entry mutex state */
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
  /* Safety: verify no external holder before overwriting */
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
 * get_or_compute_api: models a single call to get_or_compute().
 *
 * Phase 1 (atomic, global _mtx): Search or insert.
 *   Found: acquire refcount, move to MRU, existed=true.  NO state change.
 *   Not found: find slot (empty/evict), install Computing, is_owner=true.
 *
 * Phase 2 (atomic, entry lock): Resolve hit — existed entries only.
 *   Computing → CV wait, harvest.
 *   Ready/Failed valid → hit.
 *   Expired/Invalidated → transition to Computing, become owner.
 *   Models resolve_hit() + recomputation arm of resolve_miss().
 *
 * Phase 3 (non-atomic + atomic): Solver + state update (owner only).
 *   3a: entry lock — set state, decrement computing_count, set result.
 *   3b: global lock — move_to_mru (guard against concurrent invalidation).
 *
 * Phase 4 (atomic): Release refcount (EntryGuard destructor).
 */
inline get_or_compute_api(myid, mykey, sid, is_owner, existed)
{
  sid = NONE;
  is_owner = false;
  existed = false;

  /*
   * PHASE 1: SEARCH / INSERT (under global _mtx)
   * Models contains_or_insert_locked().
   * For found entries: acquires refcount regardless of state.
   */
  atomic {
    if
    :: slot_key[0] == mykey && OCCUPIED(0) -> sid = 0
    :: slot_key[1] == mykey && OCCUPIED(1) -> sid = 1
    :: else -> skip
    fi;

    if
    :: sid != NONE ->
       /* Key found: acquire ref, move to MRU.  No state change. */
       slot_rc[sid]++;
       move_to_mru(sid);
       existed = true

    :: else ->
       /* Key not found.  Find empty or evictable slot. */
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
  } /* end Phase 1 */

  if
  :: request_done[myid] -> skip   /* Saturated */
  :: else ->

     /*
      * PHASE 2: RESOLVE HIT (under entry lock, existed entries only)
      *
      * Between Phase 1 (global lock released) and Phase 2 (entry lock
      * acquired), Background can expire and Invalidator can invalidate.
      * This is the key improvement over v4 which blocked them via rc<=1.
      */
     if
     :: existed && !is_owner ->
        atomic {
          !slot_locked[sid] ->
          slot_locked[sid] = true;

          if
          /*
           * Case 1: Computing — another thread is solving.
           * CV wait: release lock, block until state != Computing.
           */
          :: slot_key[sid] == mykey && slot_state[sid] == Computing ->
             slot_locked[sid] = false;
             slot_state[sid] != Computing && !slot_locked[sid] ->
             slot_locked[sid] = true;
             if
             :: slot_key[sid] == mykey && slot_state[sid] == Ready ->
                request_result[myid] = HitPositive;
                request_done[myid] = true;
                ever_stored[mykey] = true;
                slot_age[sid] = 0
             :: slot_key[sid] == mykey && slot_state[sid] == Failed ->
                request_result[myid] = HitNegative;
                request_done[myid] = true;
                ever_stored[mykey] = true;
                slot_age[sid] = 0
             :: else -> skip  /* Entry modified; retry via outer loop */
             fi

          /*
           * Case 2: Ready, not expired — positive hit.
           */
          :: slot_key[sid] == mykey &&
             slot_state[sid] == Ready && !slot_expired[sid] ->
             request_result[myid] = HitPositive;
             request_done[myid] = true;
             ever_stored[mykey] = true;
             slot_age[sid] = 0

          /*
           * Case 3: Failed, not expired — negative hit.
           */
          :: slot_key[sid] == mykey &&
             slot_state[sid] == Failed && !slot_expired[sid] ->
             request_result[myid] = HitNegative;
             request_done[myid] = true;
             ever_stored[mykey] = true;
             slot_age[sid] = 0

          /*
           * Case 4: Expired or Invalidated — recompute.
           * Models resolve_hit() nullopt → resolve_miss() set Computing.
           * Skips the intermediate Empty state (entry-lock serialized).
           */
          :: slot_key[sid] == mykey &&
             (slot_state[sid] == Invalidated ||
              (MATERIALIZED(sid) && slot_expired[sid])) ->
             slot_state[sid] = Computing;
             slot_expired[sid] = false;
             slot_age[sid] = 0;
             computing_count++;
             is_owner = true

          /* Case 5: defensive fallthrough */
          :: else -> skip
          fi;

          slot_locked[sid] = false;
          refresh_and_check()
        } /* end Phase 2 */
     :: else -> skip
     fi;

     /*
      * PHASE 3: SOLVER EXECUTION (owner only)
      *
      * The skip is the solver interleaving point — no locks held.
      * 3a: re-acquire entry lock, set terminal state, notify waiters.
      * 3b: acquire global lock, move_to_mru, set result + done.
      */
     if
     :: is_owner && !request_done[myid] ->
        skip;   /* solver — non-atomic interleaving point */

        /* 3a: state update under entry lock */
        atomic {
          !slot_locked[sid] ->
          slot_locked[sid] = true;
          if :: slot_state[sid] = Ready :: slot_state[sid] = Failed fi;
          slot_expired[sid] = false;
          computing_count--;
          ever_stored[mykey] = true;
          /* Set result while holding entry lock (before concurrent invalidation) */
          if
          :: slot_state[sid] == Ready  -> request_result[myid] = ComputedPositive
          :: slot_state[sid] == Failed -> request_result[myid] = ComputedNegative
          fi;
          refresh_and_check();
          slot_locked[sid] = false
        };

        /* 3b: move_to_mru under global lock */
        atomic {
          if
          :: slot_state[sid] == Ready || slot_state[sid] == Failed ->
             move_to_mru(sid)
          :: else -> skip  /* Guard against concurrent invalidation */
          fi;
          request_done[myid] = true;
          refresh_and_check()
        }
     :: else -> skip
     fi;

     /*
      * PHASE 4: RELEASE REFCOUNT (EntryGuard destructor)
      */
     atomic {
       assert(slot_rc[sid] > 0);
       slot_rc[sid]--;
       is_owner = false;
       refresh_and_check()
     }
  fi
}

/*
 * Requester: models a thread calling get_or_compute().
 */
proctype Requester(byte myid)
{
  byte mykey = target_key[myid];
  byte sid;
  bool is_owner;
  bool existed;

  do
  :: !request_done[myid] ->
     get_or_compute_api(myid, mykey, sid, is_owner, existed);
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
 *
 * Unlike get_or_compute:
 *   - Never installs a Computing entry (no solver)
 *   - Returns nullopt for missing, Invalidated, expired
 *   - Blocks on Computing (CV wait), then harvests
 *   - Does NOT move to MRU in Phase 1 (only after valid harvest)
 *   - Acquires refcount for ANY found entry (matches C++)
 */
proctype FindRequester(byte myid)
{
  byte mykey = target_key[myid];
  byte sid = NONE;

  /*
   * PHASE 1: SEARCH (under global _mtx)
   * No move_to_mru here — find() confirms validity first.
   */
  atomic {
    if
    :: slot_key[0] == mykey && OCCUPIED(0) -> sid = 0; slot_rc[0]++
    :: slot_key[1] == mykey && OCCUPIED(1) -> sid = 1; slot_rc[1]++
    :: else -> skip
    fi;
    refresh_and_check()
  }

  if
  :: sid == NONE ->
     request_done[myid] = true

  :: else ->
     /*
      * PHASE 2: VALIDITY CHECK + HARVEST (under entry lock)
      */
     atomic {
       !slot_locked[sid] ->
       slot_locked[sid] = true;

       if
       /* Invalid: Invalidated, Empty, or expired → nullopt */
       :: slot_key[sid] == mykey &&
          (slot_state[sid] == Invalidated ||
           slot_state[sid] == Empty ||
           (MATERIALIZED(sid) && slot_expired[sid])) ->
          skip  /* nullopt */

       /* Computing: CV wait */
       :: slot_key[sid] == mykey && slot_state[sid] == Computing ->
          slot_locked[sid] = false;
          slot_state[sid] != Computing && !slot_locked[sid] ->
          slot_locked[sid] = true;
          if
          :: slot_key[sid] == mykey && slot_state[sid] == Ready ->
             request_result[myid] = HitPositive;
             slot_age[sid] = 0
          :: slot_key[sid] == mykey && slot_state[sid] == Failed ->
             request_result[myid] = HitNegative;
             slot_age[sid] = 0
          :: else -> skip  /* Modified during wait: nullopt */
          fi

       /* Ready, not expired */
       :: slot_key[sid] == mykey &&
          slot_state[sid] == Ready && !slot_expired[sid] ->
          request_result[myid] = HitPositive;
          slot_age[sid] = 0

       /* Failed, not expired */
       :: slot_key[sid] == mykey &&
          slot_state[sid] == Failed && !slot_expired[sid] ->
          request_result[myid] = HitNegative;
          slot_age[sid] = 0

       :: else -> skip  /* Defensive: nullopt */
       fi;

       slot_locked[sid] = false;
       refresh_and_check()
     }

     /*
      * PHASE 2b: move_to_mru under global lock (hits only).
      * Models: scoped_lock glock(_mtx); lru_move_to_mru(entry)
      */
     atomic {
       if
       :: request_result[myid] == HitPositive ||
          request_result[myid] == HitNegative ->
          if
          :: slot_state[sid] == Ready || slot_state[sid] == Failed ->
             move_to_mru(sid)
          :: else -> skip  /* Guard against concurrent invalidation */
          fi
       :: else -> skip
       fi;
       refresh_and_check()
     }

     /* PHASE 3: RELEASE REFCOUNT */
     atomic {
       assert(slot_rc[sid] > 0);
       slot_rc[sid]--;
       refresh_and_check()
     };

     request_done[myid] = true
  fi
}

/*
 * Background: simulates TTL aging and expiry.
 *
 * v5 change: guard no longer requires rc <= 1.  Only !slot_locked is
 * needed — this models entry->mtx() availability, independent of the
 * refcount.  Allows expiry while threads hold refs (as in real C++
 * where time passes independently of refcount).
 */
proctype Background()
{
  end: do
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
 *
 * v5 change: guard no longer requires rc <= 1.  In C++, invalidate()
 * acquires entry->mtx() which is independent of refcount.  A thread
 * can hold a ref (rc > 1) without holding the entry lock, so the
 * Invalidator can proceed.  The thread will detect Invalidated state
 * when it later acquires the entry lock in Phase 2.
 *
 * MAPPING TO C++:
 *   Guard (!slot_locked)       → std::scoped_lock entry_lock(entry->mtx())
 *   assert(!Computing)         → if (state == Computing) return false
 *   state = Invalidated        → entry->set_state(Invalidated)
 *   move_to_lru                → _lru_list.append(link)
 */
proctype Invalidator()
{
  end: do
  :: atomic {
       slot_state[0] != Empty && slot_state[0] != Computing &&
       slot_state[0] != Invalidated && !slot_locked[0] ->
       slot_rc[0]++;
       slot_locked[0] = true;
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
 *
 * v5 change: guard no longer requires rc <= 1.
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
 * PeekRequester: models peek(key) — truly non-blocking.
 * Uses try_to_lock: !slot_locked models std::try_to_lock success.
 *
 * v5 change: guard no longer requires rc <= 1.
 */
proctype PeekRequester()
{
  end: do
  :: atomic {
       MATERIALIZED(0) && !slot_expired[0] && !slot_locked[0] ->
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

/*
 * HasRequester: models has(key) — fast validity check.
 *
 * MAPPING TO C++:
 *   Global lock: search_entry + acquire()
 *   Entry lock:  check state == Ready/Failed && !expired
 *   Release ref: ~EntryGuard()
 *
 * Read-only: never modifies entry state or LRU position.
 */
proctype HasRequester()
{
  end: do
  :: atomic {
       OCCUPIED(0) && !slot_locked[0] ->
       slot_rc[0]++;
       slot_locked[0] = true;
       /* has() returns true only for Ready/Failed + not expired */
       if
       :: MATERIALIZED(0) && !slot_expired[0] -> skip  /* true */
       :: else -> skip  /* false */
       fi;
       slot_locked[0] = false;
       slot_rc[0]--;
       refresh_and_check()
     }
  :: atomic {
       OCCUPIED(1) && !slot_locked[1] ->
       slot_rc[1]++;
       slot_locked[1] = true;
       if
       :: MATERIALIZED(1) && !slot_expired[1] -> skip
       :: else -> skip
       fi;
       slot_locked[1] = false;
       slot_rc[1]--;
       refresh_and_check()
     }
  od
}

init
{
  atomic {
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

    /* FindRequester threads: find() on keys 0 and 1. */
    target_key[4] = 0; request_done[4] = false; request_result[4] = Saturated;
    target_key[5] = 1; request_done[5] = false; request_result[5] = Saturated;

    refresh_and_check();

    run Background();
    run Invalidator();
    run TouchRequester();
    run PeekRequester();
    run HasRequester();
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

/* NEW: Whenever a computation starts, all computations eventually complete. */
ltl no_orphaned_computing {
  [] (computing_count > 0 -> <> (computing_count == 0))
}

/* NEW: Saturation is transient — if the cache is fully blocked,
 * it eventually becomes unblocked (solvers finish, refs are released). */
ltl saturation_transient {
  [] (ALL_BLOCKED() -> <> !ALL_BLOCKED())
}