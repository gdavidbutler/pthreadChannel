/*
 * Promela model of pthreadChannel chanAll() - Atomic Multi-Channel Operations
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 * Generated with Claude Code (https://claude.ai/code)
 *
 * This model focuses specifically on verifying chanAll semantics:
 *   - All-or-nothing atomicity
 *   - Lock ladder correctness (acquire ascending, release descending)
 *   - No partial completion, at every nsTimeout including < 0
 *   - Proper interaction with concurrent operations
 *
 * Three scenarios, selected at spin time:
 *
 *   (default)             single pass. Two chanAll calls contend over the
 *                         same two channels. This is the nsTimeout < 0
 *                         shape: nothing waits.
 *   -DTEST_BLOCKING       nsTimeout == 0. One chanAll(GET,GET) over two
 *                         initially empty channels, so it MUST wait, plus
 *                         two putters that fill one channel each.
 *   -DTEST_BLOCKING_TIMED nsTimeout > 0. As above, but the wait may expire.
 *
 * What the blocking scenarios are for: registration-before-unlock (a
 * wakeup cannot be missed) and re-scan-before-commit (a wake alone never
 * commits anything). Both are checked by mutation -- moving registration
 * after the unlock breaks blocked_completes, and committing on the wake
 * without re-scanning breaks no_commit_on_partial_wake.
 *
 *   -DTEST_FAIRNESS       arrival order and its escape, with real FIFO
 *                         waiter queues and wake-one (not broadcast)
 *
 * The fairness scenario is where the two arrival rules are separated:
 *   arriving  (chan.c:492)  CanGet && (chanGe || !chanPe)
 *   post-wake (chan.c:827)  CanGet
 * The clause is deliberately ABSENT after a wake. Re-applying it there
 * deadlocks the Channel -- the woken thread goes behind waiters that were
 * not woken, and the item it was handed is never taken. That is checked by
 * mutation and is the sharpest thing this model has to say.
 *
 * Not modelled: a caller error (no entry with a live operation).
 *
 * Build and run:
 *   spin [-DTEST_BLOCKING|-DTEST_BLOCKING_TIMED] -a chanAll.pml
 *   cc -DSAFETY -O2 -o pan pan.c
 *   ./pan -N atomicity
 *
 * For liveness (needs fairness, and a verifier built without -DSAFETY):
 *   cc -O2 -o pan pan.c
 *   ./pan -a -f -N blocked_completes
 */

#define NCHANS 3

/* Channel states */
#define ST_EMPTY    0
#define ST_HAS_ITEM 1
#define ST_SHUT     2

/* Operations */
#define OP_NOP 0
#define OP_SHT 1
#define OP_GET 2
#define OP_PUT 3

/* Operation status */
#define OS_NOP 0
#define OS_SHT 1
#define OS_GET 2
#define OS_PUT 3

/* chanAll return */
#define AL_ERR 0
#define AL_EVT 1
#define AL_OP  2
#define AL_TMO 3

/*
 * Channel structure
 */
typedef Channel {
  byte state;
  byte lock;         /* 0 = unlocked, N = holder pid+1 */
  byte get_waiters;
  byte put_waiters;
};

Channel channels[NCHANS];

/* Global tracking for atomicity verification */
byte chanall_in_progress = 0;  /* count of threads in chanAll */
byte chanall_completed = 0;    /* successful chanAll completions */
byte chanall_events = 0;       /* chanAll returns with events */

/* Set by chanAll_2/chanAll_3 when a result and its statuses disagree.
 * This is what the atomicity claim observes; see those inlines. */
bool atomicity_violated = false;

/* TEST_FAIRNESS exercises the arrival-order rule and its documented escape */
#ifdef TEST_FAIRNESS
#ifndef TEST_BLOCKING
#define TEST_BLOCKING
#endif
#endif

/* TEST_BLOCKING_TIMED is TEST_BLOCKING with an expiring wait (nsTimeout > 0) */
#ifdef TEST_BLOCKING_TIMED
#ifndef TEST_BLOCKING
#define TEST_BLOCKING
#endif
#define BLOCK_TIMED true
#else
#define BLOCK_TIMED false
#endif

#ifdef TEST_BLOCKING
/*
 * Waiter bookkeeping for the blocking paths (nsTimeout >= 0).
 *
 * waiting[ch * NPROC + p] is p registered as a waiter on ch, which the
 * implementation does by queueing on the channel's chanGe/chanPe/chanHe
 * list. signalled[p] stands in for the condition variable p waits on.
 *
 * A wake here signals EVERY registered waiter on the channel. The
 * implementation instead wakes the head of a FIFO queue, so this model
 * does NOT represent waiter ordering: it cannot find a "woke the wrong
 * waiter" bug, and it proves nothing about arrival-order fairness.
 * What it does represent is registration-before-unlock, which is what
 * makes a wakeup impossible to miss.
 */
#ifdef TEST_FAIRNESS
#define NPROC 4
#else
#define NPROC 2
#endif

bool waiting[NCHANS * NPROC];
bool signalled[NPROC];

#ifdef TEST_FAIRNESS
/* Ordered waiter queues: gq/pq hold thread ids, front first, so a wake can
 * take the head rather than broadcasting. nq_get/nq_put are their lengths. */
byte gq[NCHANS * NPROC];
byte pq[NCHANS * NPROC];
#endif

/*
 * Waiter-queue occupancy, the model of chan.c's chanGe/chanPe flags.
 * A thread proceeds only if the Store allows it AND either nobody is queued
 * on its own side, or somebody is queued on the OTHER side. That second
 * disjunct is the documented escape from strict arrival order: the opposite
 * waiter is about to change the Store anyway, so barging costs nothing.
 *   chan.c:492  CanGet && (chanGe || !chanPe)
 *   chan.c:528  CanPut && (chanPe || !chanGe)
 */
byte nq_get[NCHANS];
byte nq_put[NCHANS];

#define MAY_GET(c) (channels[c].state == ST_HAS_ITEM \
                    && (nq_get[c] == 0 || nq_put[c] > 0))
#define MAY_PUT(c) (channels[c].state == ST_EMPTY \
                    && (nq_put[c] == 0 || nq_get[c] > 0))
byte wk_p;                     /* wake_waiters scratch, used only under atomic */

inline enq_slot(c, o) {
  if
  :: o == OP_GET -> nq_get[c]++
  :: o == OP_PUT -> nq_put[c]++
  :: else -> skip
  fi
}

inline deq_slot(c, o) {
  if
  :: o == OP_GET -> nq_get[c]--
  :: o == OP_PUT -> nq_put[c]--
  :: else -> skip
  fi
}

/* Counts Puts that have landed, incremented under the channel's lock so
 * it cannot be read stale by a thread holding that lock. */
byte items_put = 0;

inline note_put() {
  items_put++
}

inline wake_waiters(ch) {
  atomic {
    wk_p = 0;
    do
    :: wk_p < NPROC ->
       if
       :: waiting[ch * NPROC + wk_p] -> signalled[wk_p] = true
       :: else -> skip
       fi;
       wk_p++
    :: wk_p >= NPROC -> break
    od
  }
}
#else
#define MAY_GET(c) (channels[c].state == ST_HAS_ITEM)
#define MAY_PUT(c) (channels[c].state == ST_EMPTY)

inline wake_waiters(ch) {
  skip
}

inline note_put() {
  skip
}
#endif

/*
 * Lock operations
 */
inline lock_chan(ch) {
  atomic {
    (channels[ch].lock == 0) -> channels[ch].lock = _pid + 1
  }
}

inline unlock_chan(ch) {
  d_step { channels[ch].lock = 0 }
}

inline trylock_chan(ch, success) {
  atomic {
    if
    :: channels[ch].lock == 0 ->
       channels[ch].lock = _pid + 1;
       success = 1
    :: else ->
       success = 0
    fi
  }
}

/*
 * Lock ladder for N channels
 * Acquires locks in ascending index order
 * Uses trylock for all but first, releases all and retries on failure
 *
 * This models the critical deadlock-prevention mechanism in chan.c
 */
inline lock_ladder_3(success) {
  byte ll_i;
  byte ll_got;
  bool ll_done;

  ll_done = false;
  do
  :: ll_done == false ->
     /* Lock channel 0 (blocking) */
     lock_chan(0);

     /* Try lock channel 1 */
     trylock_chan(1, ll_got);
     if
     :: ll_got == 0 ->
        unlock_chan(0);
        ll_done = false  /* retry */
     :: ll_got == 1 ->
        /* Try lock channel 2 */
        trylock_chan(2, ll_got);
        if
        :: ll_got == 0 ->
           unlock_chan(1);
           unlock_chan(0);
           ll_done = false  /* retry */
        :: ll_got == 1 ->
           ll_done = true;
           success = 1
        fi
     fi
  :: ll_done == true -> break
  od
}

/*
 * Unlock in descending order
 */
inline unlock_ladder_3() {
  unlock_chan(2);
  unlock_chan(1);
  unlock_chan(0)
}

/*
 * chanAll for 3 channels
 * ops[i] is the operation for channel i
 * status[i] is set to the result
 * Returns AL_OP if all succeeded, AL_EVT if event occurred
 *
 * Key semantics from chan.c:
 *   j |= 2: event (shutdown, or demand-style check succeeded)
 *   j |= 1: would block (can't do operation yet)
 *   j == 0: all operations can proceed
 */
inline chanAll_3(ops, status, result) {
  byte ca_i;
  byte ca_j;
  bool ca_success;
  bool ca_has_event;
  bool ca_would_block;

  result = AL_ERR;
  chanall_in_progress++;

  /* Lock ladder */
  lock_ladder_3(ca_success);
  assert(ca_success == 1);

  /* Check all operations */
  ca_has_event = false;
  ca_would_block = false;

  ca_i = 0;
  do
  :: ca_i < NCHANS ->
     if
     /* Shutdown check */
     :: channels[ca_i].state == ST_SHUT ->
        ca_has_event = true

     /* Get operation */
     :: ops[ca_i] == OP_GET ->
        if
        :: channels[ca_i].state == ST_SHUT ->
           ca_has_event = true
        :: channels[ca_i].state == ST_HAS_ITEM ->
           skip  /* can proceed */
        :: channels[ca_i].state == ST_EMPTY ->
           ca_would_block = true
        fi

     /* Put operation */
     :: ops[ca_i] == OP_PUT ->
        if
        :: channels[ca_i].state == ST_SHUT ->
           ca_has_event = true
        :: channels[ca_i].state == ST_EMPTY ->
           skip  /* can proceed */
        :: channels[ca_i].state == ST_HAS_ITEM ->
           ca_would_block = true
        fi

     /* Nop */
     :: ops[ca_i] == OP_NOP ->
        skip

     :: else -> skip
     fi;
     ca_i++
  :: ca_i >= NCHANS -> break
  od;

  if
  :: ca_has_event ->
     /* Event occurred - set status, don't perform operations */
     ca_i = 0;
     do
     :: ca_i < NCHANS ->
        if
        :: channels[ca_i].state == ST_SHUT -> status[ca_i] = OS_SHT
        :: else -> status[ca_i] = OS_NOP
        fi;
        ca_i++
     :: ca_i >= NCHANS -> break
     od;
     result = AL_EVT;
     chanall_events++;
     unlock_ladder_3()

  :: ca_would_block && ca_has_event == false ->
     /* Would block, no event: nsTimeout < 0 completes nothing at all.
      * The implementation leaves every status untouched here; OS_NOP is
      * this model's stand-in, since a status starts at OS_NOP. */
     ca_i = 0;
     do
     :: ca_i < NCHANS ->
        status[ca_i] = OS_NOP;
        ca_i++
     :: ca_i >= NCHANS -> break
     od;
     result = AL_TMO;
     unlock_ladder_3()

  :: ca_would_block == false && ca_has_event == false ->
     /* ALL operations can proceed - do them ATOMICALLY */
     atomic {
       ca_i = 0;
       do
       :: ca_i < NCHANS ->
          if
          :: ops[ca_i] == OP_GET ->
             /* Verify precondition */
             assert(channels[ca_i].state == ST_HAS_ITEM);
             channels[ca_i].state = ST_EMPTY;
             status[ca_i] = OS_GET
          :: ops[ca_i] == OP_PUT ->
             /* Verify precondition */
             assert(channels[ca_i].state == ST_EMPTY);
             channels[ca_i].state = ST_HAS_ITEM;
             status[ca_i] = OS_PUT
          :: ops[ca_i] == OP_NOP ->
             status[ca_i] = OS_NOP
          :: else ->
             status[ca_i] = OS_NOP
          fi;
          ca_i++
       :: ca_i >= NCHANS -> break
       od;
       /* publish the result with the states it describes: a reader that
        * samples between the two would see a half-reported transaction */
       result = AL_OP
     };
     chanall_completed++;
     unlock_ladder_3()
  fi;

  /* Atomicity, as a flag the atomicity claim can observe: see chanAll_2 */
  ca_i = 0;
  do
  :: ca_i < NCHANS ->
     if
     :: result == AL_OP
     && (ops[ca_i] == OP_GET || ops[ca_i] == OP_PUT)
     && status[ca_i] != ops[ca_i] ->
        atomicity_violated = true
     :: result != AL_OP
     && (status[ca_i] == OS_GET || status[ca_i] == OS_PUT) ->
        atomicity_violated = true
     :: else -> skip
     fi;
     ca_i++
  :: ca_i >= NCHANS -> break
  od;

  chanall_in_progress--
}

/*
 * Simpler 2-channel version for clearer verification
 */
inline lock_ladder_2(ch0, ch1, success) {
  byte ll_got;
  bool ll_done;

  ll_done = false;
  do
  :: ll_done == false ->
     lock_chan(ch0);
     trylock_chan(ch1, ll_got);
     if
     :: ll_got == 0 ->
        unlock_chan(ch0)
        /* retry */
     :: ll_got == 1 ->
        ll_done = true;
        success = 1
     fi
  :: ll_done == true -> break
  od
}

inline unlock_ladder_2(ch0, ch1) {
  unlock_chan(ch1);
  unlock_chan(ch0)
}

/*
 * chanAll for 2 specific channels with specific operations
 */
inline chanAll_2(ch0, op0, ch1, op1, status0, status1, result) {
  bool ca_success;
  bool ca_has_event;
  bool ca_would_block;

  result = AL_ERR;

  lock_ladder_2(ch0, ch1, ca_success);
  assert(ca_success == 1);

  ca_has_event = false;
  ca_would_block = false;

  /* Check channel 0 */
  if
  :: channels[ch0].state == ST_SHUT -> ca_has_event = true
  :: op0 == OP_GET && channels[ch0].state == ST_EMPTY -> ca_would_block = true
  :: op0 == OP_PUT && channels[ch0].state == ST_HAS_ITEM -> ca_would_block = true
  :: else -> skip
  fi;

  /* Check channel 1 */
  if
  :: channels[ch1].state == ST_SHUT -> ca_has_event = true
  :: op1 == OP_GET && channels[ch1].state == ST_EMPTY -> ca_would_block = true
  :: op1 == OP_PUT && channels[ch1].state == ST_HAS_ITEM -> ca_would_block = true
  :: else -> skip
  fi;

  if
  :: ca_has_event ->
     if
     :: channels[ch0].state == ST_SHUT -> status0 = OS_SHT
     :: else -> status0 = OS_NOP
     fi;
     if
     :: channels[ch1].state == ST_SHUT -> status1 = OS_SHT
     :: else -> status1 = OS_NOP
     fi;
     result = AL_EVT;
     unlock_ladder_2(ch0, ch1)

  :: ca_would_block && ca_has_event == false ->
     status0 = OS_NOP;
     status1 = OS_NOP;
     result = AL_TMO;
     unlock_ladder_2(ch0, ch1)

  :: ca_would_block == false && ca_has_event == false ->
     /* Atomic execution of both operations */
     atomic {
       if
       :: op0 == OP_GET ->
          assert(channels[ch0].state == ST_HAS_ITEM);
          channels[ch0].state = ST_EMPTY;
          status0 = OS_GET
       :: op0 == OP_PUT ->
          assert(channels[ch0].state == ST_EMPTY);
          channels[ch0].state = ST_HAS_ITEM;
          status0 = OS_PUT
       :: else -> status0 = OS_NOP
       fi;
       if
       :: op1 == OP_GET ->
          assert(channels[ch1].state == ST_HAS_ITEM);
          channels[ch1].state = ST_EMPTY;
          status1 = OS_GET
       :: op1 == OP_PUT ->
          assert(channels[ch1].state == ST_EMPTY);
          channels[ch1].state = ST_HAS_ITEM;
          status1 = OS_PUT
       :: else -> status1 = OS_NOP
       fi;
       /* publish the result with the states it describes: a reader that
        * samples between the two would see a half-reported transaction */
       result = AL_OP
     };
     chanall_completed++;
     unlock_ladder_2(ch0, ch1)
  fi;

  /* Atomicity, as a flag the atomicity claim can observe: a status may
   * claim a completed Get or Put ONLY on a committed transaction, and a
   * committed transaction must claim every operation it was asked for. */
  if
  :: result == AL_OP ->
     if
     :: ((op0 == OP_GET || op0 == OP_PUT) && status0 != op0) ->
        atomicity_violated = true
     :: ((op1 == OP_GET || op1 == OP_PUT) && status1 != op1) ->
        atomicity_violated = true
     :: else -> skip
     fi
  :: else ->
     if
     :: (status0 == OS_GET || status0 == OS_PUT) ->
        atomicity_violated = true
     :: (status1 == OS_GET || status1 == OS_PUT) ->
        atomicity_violated = true
     :: else -> skip
     fi
  fi
}

#ifdef TEST_BLOCKING
/*
 * chanAll for 2 channels on a BLOCKING nsTimeout (>= 0).
 *
 * Where chanAll_2 resolves in one pass, this one loops: scan under the
 * lock ladder, and if any operation would block, register as a waiter on
 * every channel, release the ladder, wait, then scan again from scratch.
 *
 * Two things are load-bearing and are what this model exists to check:
 *   - registration happens while the ladder is still held, so a state
 *     change cannot slip between "decided to wait" and "visible as a
 *     waiter" (a missed wakeup);
 *   - a wake NEVER commits anything by itself. It only causes a re-scan,
 *     and the commit is gated on that fresh scan finding every operation
 *     satisfiable -- which is how all-or-none survives waiting.
 *
 * timed: if true the wait may instead expire, returning AL_TMO having
 * performed nothing (the nsTimeout > 0 path).
 */
inline chanAll_2b(ch0, op0, ch1, op1, status0, status1, result, me, timed) {
  bool cb_success;
  bool cb_has_event;
  bool cb_would_block;
  bool cb_done;

  result = AL_ERR;
  cb_done = false;

  do
  :: cb_done -> break
  :: else ->
     lock_ladder_2(ch0, ch1, cb_success);
     assert(cb_success == 1);

     cb_has_event = false;
     cb_would_block = false;

     if
     :: channels[ch0].state == ST_SHUT -> cb_has_event = true
     :: op0 == OP_GET && channels[ch0].state != ST_SHUT && !MAY_GET(ch0) ->
        cb_would_block = true
     :: op0 == OP_PUT && channels[ch0].state != ST_SHUT && !MAY_PUT(ch0) ->
        cb_would_block = true
     :: else -> skip
     fi;
     if
     :: channels[ch1].state == ST_SHUT -> cb_has_event = true
     :: op1 == OP_GET && channels[ch1].state != ST_SHUT && !MAY_GET(ch1) ->
        cb_would_block = true
     :: op1 == OP_PUT && channels[ch1].state != ST_SHUT && !MAY_PUT(ch1) ->
        cb_would_block = true
     :: else -> skip
     fi;

     if
     :: cb_has_event ->
        if
        :: channels[ch0].state == ST_SHUT -> status0 = OS_SHT
        :: else -> status0 = OS_NOP
        fi;
        if
        :: channels[ch1].state == ST_SHUT -> status1 = OS_SHT
        :: else -> status1 = OS_NOP
        fi;
        result = AL_EVT;
        unlock_ladder_2(ch0, ch1);
        cb_done = true

     :: cb_would_block == false && cb_has_event == false ->
        atomic {
          if
          :: op0 == OP_GET ->
             assert(channels[ch0].state == ST_HAS_ITEM);
             channels[ch0].state = ST_EMPTY;
             status0 = OS_GET
          :: op0 == OP_PUT ->
             assert(channels[ch0].state == ST_EMPTY);
             channels[ch0].state = ST_HAS_ITEM;
             status0 = OS_PUT
          :: else -> status0 = OS_NOP
          fi;
          if
          :: op1 == OP_GET ->
             assert(channels[ch1].state == ST_HAS_ITEM);
             channels[ch1].state = ST_EMPTY;
             status1 = OS_GET
          :: op1 == OP_PUT ->
             assert(channels[ch1].state == ST_EMPTY);
             channels[ch1].state = ST_HAS_ITEM;
             status1 = OS_PUT
          :: else -> status1 = OS_NOP
          fi;
          result = AL_OP
        };
        wake_waiters(ch0);
        wake_waiters(ch1);
        chanall_completed++;
        unlock_ladder_2(ch0, ch1);
        cb_done = true

     :: cb_would_block && cb_has_event == false ->
        /* register on every channel while the ladder is still held */
        waiting[ch0 * NPROC + me] = true;
        waiting[ch1 * NPROC + me] = true;
        enq_slot(ch0, op0);
        enq_slot(ch1, op1);
        signalled[me] = false;
        unlock_ladder_2(ch0, ch1);
        if
        :: timed ->
           atomic {
             deq_slot(ch0, op0);
             deq_slot(ch1, op1)
           };
           status0 = OS_NOP;
           status1 = OS_NOP;
           result = AL_TMO;
           cb_done = true
        :: signalled[me] ->
           /* woken: leave the queue and scan again as a fresh arrival */
           atomic {
             deq_slot(ch0, op0);
             deq_slot(ch1, op1)
           }
        fi
     fi
  od;

  atomic {
    waiting[ch0 * NPROC + me] = false;
    waiting[ch1 * NPROC + me] = false
  };

  /* same atomicity invariant as chanAll_2 */
  if
  :: result == AL_OP ->
     if
     :: ((op0 == OP_GET || op0 == OP_PUT) && status0 != op0) ->
        atomicity_violated = true
     :: ((op1 == OP_GET || op1 == OP_PUT) && status1 != op1) ->
        atomicity_violated = true
     :: else -> skip
     fi
  :: else ->
     if
     :: (status0 == OS_GET || status0 == OS_PUT) ->
        atomicity_violated = true
     :: (status1 == OS_GET || status1 == OS_PUT) ->
        atomicity_violated = true
     :: else -> skip
     fi
  fi
}
#endif

/*
 * Single channel operation (for concurrent interference testing)
 */
inline chanOp(ch, op, status) {
  lock_chan(ch);
  if
  :: channels[ch].state == ST_SHUT ->
     status = OS_SHT
  :: op == OP_GET && MAY_GET(ch) ->
     channels[ch].state = ST_EMPTY;
     status = OS_GET;
     wake_waiters(ch)
  :: op == OP_PUT && MAY_PUT(ch) ->
     channels[ch].state = ST_HAS_ITEM;
     status = OS_PUT;
     note_put();
     wake_waiters(ch)
  :: else ->
     status = OS_NOP
  fi;
  unlock_chan(ch)
}

/*
 * Test: Two threads doing chanAll on overlapping channels
 * Thread 1: chanAll(GET ch0, PUT ch1)
 * Thread 2: chanAll(PUT ch0, GET ch1)
 *
 * This creates contention and tests:
 * - Lock ladder prevents deadlock
 * - Atomicity is preserved
 * - No partial completion
 */
byte t1_result;
byte t1_s0, t1_s1;
byte t2_result;
byte t2_s0, t2_s1;
bool t1_done = false;
bool t2_done = false;

proctype thread1() {
  chanAll_2(0, OP_GET, 1, OP_PUT, t1_s0, t1_s1, t1_result);

  /* Verify atomicity: if AL_OP, both must have succeeded */
  if
  :: t1_result == AL_OP ->
     assert(t1_s0 == OS_GET);
     assert(t1_s1 == OS_PUT)
  :: t1_result == AL_TMO ->
     /* Nothing completed: neither status may claim an operation */
     assert(t1_s0 != OS_GET);
     assert(t1_s1 != OS_PUT)
  :: t1_result == AL_EVT ->
     /* Either both failed or got shutdown */
     skip
  :: else -> skip
  fi;

  t1_done = true
}

proctype thread2() {
  chanAll_2(0, OP_PUT, 1, OP_GET, t2_s0, t2_s1, t2_result);

  /* Verify atomicity: if AL_OP, both must have succeeded */
  if
  :: t2_result == AL_OP ->
     assert(t2_s0 == OS_PUT);
     assert(t2_s1 == OS_GET)
  :: t2_result == AL_TMO ->
     /* Nothing completed: neither status may claim an operation */
     assert(t2_s0 != OS_PUT);
     assert(t2_s1 != OS_GET)
  :: t2_result == AL_EVT ->
     skip
  :: else -> skip
  fi;

  t2_done = true
}

/*
 * Test: chanAll vs single chanOp interference
 * One thread does chanAll(GET ch0, GET ch1)
 * Another thread does single PUT on ch0
 * Another thread does single PUT on ch1
 *
 * chanAll should either:
 * - Get both (if both have items)
 * - Get neither (if either is empty)
 */
byte t3_result;
byte t3_s0, t3_s1;
bool t3_done = false;
bool t4_done = false;
bool t5_done = false;

proctype chanall_getter() {
  byte status0, status1, result;

  chanAll_2(0, OP_GET, 1, OP_GET, status0, status1, result);
  t3_s0 = status0;
  t3_s1 = status1;
  t3_result = result;

  /* Key atomicity check */
  if
  :: result == AL_OP ->
     /* If succeeded, BOTH gets must have worked */
     assert(status0 == OS_GET);
     assert(status1 == OS_GET)
  :: result == AL_TMO ->
     /* Nothing completed: neither may show as GET */
     assert(status0 != OS_GET);
     assert(status1 != OS_GET)
  :: result == AL_EVT ->
     /* If failed, neither should show as GET */
     /* (unless shutdown, in which case SHT is ok) */
     assert(status0 != OS_GET || status1 != OS_GET ||
            channels[0].state == ST_SHUT || channels[1].state == ST_SHUT)
  :: else -> skip
  fi;

  t3_done = true
}

proctype single_putter_0() {
  byte status;
  chanOp(0, OP_PUT, status);
  t4_done = true
}

proctype single_putter_1() {
  byte status;
  chanOp(1, OP_PUT, status);
  t5_done = true
}

#ifdef TEST_FAIRNESS
/*
 * Fairness scenario: arrival order, its documented escape, and whether the
 * escape is load-bearing.
 *
 * Two blocking Gets and two blocking Puts on ONE Channel. No extra arriving
 * thread is needed, because a WOKEN thread IS an arrival: chan.c's WAKE
 * dequeues the waiter it signals (chan.c:215-221), so when that thread next
 * looks at the Channel it holds no place in line.
 *
 * The interleaving that matters:
 *   G1,G2 queue on the empty ch0.        get queue: G1,G2
 *   P1 Puts, fills ch0, wakes+dequeues G1.  get queue: G2
 *   P2 arrives, ch0 is full, so P2 queues.  put queue: P2
 *   G1 now re-examines ch0. It is no longer queued, G2 is ahead of it, and
 *   a Put is waiting -- so the escape decides whether G1 may take the item.
 *
 * Strict arrival order would send G1 back behind G2. Nobody would then take
 * the item G1 was woken for, and P2 would wait on a Channel that stays full.
 * Whether that actually wedges is what fair_all_complete answers.
 */
byte fair_st[NPROC];
bool fair_done[NPROC];

inline fair_enq(c, o, who) {
  if
  :: o == OP_GET ->
     gq[c * NPROC + nq_get[c]] = who;
     nq_get[c]++
  :: o == OP_PUT ->
     pq[c * NPROC + nq_put[c]] = who;
     nq_put[c]++
  :: else -> skip
  fi
}

/* Wake exactly one waiter, from the front of the queue, dequeuing it --
 * this is WAKE, not a broadcast. */
inline fair_wake_get(c) {
  atomic {
    if
    :: nq_get[c] > 0 ->
       signalled[gq[c * NPROC]] = true;
       wk_p = 0;
       do
       :: wk_p + 1 < nq_get[c] ->
          gq[c * NPROC + wk_p] = gq[c * NPROC + wk_p + 1];
          wk_p++
       :: else -> break
       od;
       nq_get[c]--
    :: else -> skip
    fi
  }
}

inline fair_wake_put(c) {
  atomic {
    if
    :: nq_put[c] > 0 ->
       signalled[pq[c * NPROC]] = true;
       wk_p = 0;
       do
       :: wk_p + 1 < nq_put[c] ->
          pq[c * NPROC + wk_p] = pq[c * NPROC + wk_p + 1];
          wk_p++
       :: else -> break
       od;
       nq_put[c]--
    :: else -> skip
    fi
  }
}

/* A blocking single-Channel operation: chanOp with nsTimeout == 0. */
inline fair_op(c, o, who) {
  bool fo_done;
  bool fo_woken;

  fo_done = false;
  fo_woken = false;
  do
  :: fo_done -> break
  :: else ->
     lock_chan(c);
     if
     :: o == OP_GET && (MAY_GET(c) || (fo_woken && channels[c].state == ST_HAS_ITEM)) ->
        channels[c].state = ST_EMPTY;
        fair_st[who] = OS_GET;
        fair_wake_put(c);
        unlock_chan(c);
        fo_done = true
     :: o == OP_PUT && (MAY_PUT(c) || (fo_woken && channels[c].state == ST_EMPTY)) ->
        channels[c].state = ST_HAS_ITEM;
        fair_st[who] = OS_PUT;
        fair_wake_get(c);
        unlock_chan(c);
        fo_done = true
     :: else ->
        fair_enq(c, o, who);
        signalled[who] = false;
        unlock_chan(c);
        signalled[who];
        /* Woken. chan.c's post-wait rescan (chan.c:827) tests the Store
         * ONLY -- it does not re-apply the arrival-order clause. The item
         * was handed to us; sending us behind waiters who were not woken
         * would leave it untaken. */
        fo_woken = true
     fi
  od
}

proctype fair_getter(byte who) {
  fair_op(0, OP_GET, who);
  fair_done[who] = true
}

proctype fair_putter(byte who) {
  fair_op(0, OP_PUT, who);
  fair_done[who] = true
}

init {
  atomic {
    channels[0].state = ST_EMPTY;
    channels[0].lock = 0;
    channels[1].state = ST_EMPTY;
    channels[1].lock = 0;
    channels[2].state = ST_EMPTY;
    channels[2].lock = 0
  };
  run fair_getter(0);
  run fair_getter(1);
  run fair_putter(2);
  run fair_putter(3);
  (fair_done[0] && fair_done[1] && fair_done[2] && fair_done[3])
}

/* THE question: does every queued thread eventually get served? */
ltl fair_all_complete {
  <> (fair_done[0] && fair_done[1] && fair_done[2] && fair_done[3])
}

/* Nobody reports the wrong operation. */
ltl fair_ops_sane {
  [] ((fair_done[0] -> fair_st[0] == OS_GET)
      && (fair_done[1] -> fair_st[1] == OS_GET)
      && (fair_done[2] -> fair_st[2] == OS_PUT)
      && (fair_done[3] -> fair_st[3] == OS_PUT))
}

/* A one-item Store never holds two. */
ltl fair_store_bounded {
  [] (channels[0].state == ST_EMPTY || channels[0].state == ST_HAS_ITEM)
}
#else
#ifdef TEST_BLOCKING
/*
 * Blocking scenario: one chanAll(GET ch0, GET ch1) against two channels
 * that both start EMPTY, so it MUST take the wait path at least once,
 * and two independent putters that each fill one channel.
 *
 * The interesting interleaving is the one where the blocker wakes after
 * only the first putter has run: it must decline to commit, re-register,
 * and wait again.
 */
byte b_result;
byte b_s0, b_s1;
bool b_done = false;
bool f0_done = false;
bool f1_done = false;

proctype blocker() {
  chanAll_2b(0, OP_GET, 1, OP_GET, b_s0, b_s1, b_result, 0, BLOCK_TIMED);
  b_done = true
}

proctype filler_0() {
  byte s;
  chanOp(0, OP_PUT, s);
  f0_done = true
}

proctype filler_1() {
  byte s;
  chanOp(1, OP_PUT, s);
  f1_done = true
}

init {
  atomic {
    channels[0].state = ST_EMPTY;
    channels[0].lock = 0;
    channels[1].state = ST_EMPTY;
    channels[1].lock = 0;
    channels[2].state = ST_EMPTY;
    channels[2].lock = 0
  };

  run blocker();
  run filler_0();
  run filler_1();

  (b_done && f0_done && f1_done)
}

/* A blocked chanAll is eventually woken and finishes: this is the
 * property that fails if registration ever moves after the unlock. */
ltl blocked_completes {
  <> (b_done)
}

/* Waiting never weakens all-or-none: a commit is still both Gets or none.
 * Stated as an implication so it holds for the expiring wait too. */
ltl blocked_commits_all {
  [] ((b_result == AL_OP) -> (b_s0 == OS_GET && b_s1 == OS_GET))
}

/* An expired wait performed nothing (nsTimeout > 0 scenario; vacuous
 * in the indefinite one, where the wait cannot expire). */
ltl timed_expiry_takes_nothing {
  [] ((b_result == AL_TMO) -> (b_s0 != OS_GET && b_s1 != OS_GET))
}

/* A single filled channel must never be enough to commit: a wake with one
 * Put outstanding has to send the blocker back to waiting. Stated against
 * the Puts themselves, not the fillers' done flags -- those are set after
 * chanOp returns, so they lag the state change the blocker acts on. */
ltl no_commit_on_partial_wake {
  [] ((b_result == AL_OP) -> (items_put == 2))
}
#else
/*
 * Initialize and run tests
 */
init {
  atomic {
    channels[0].state = ST_EMPTY;
    channels[0].lock = 0;
    channels[1].state = ST_EMPTY;
    channels[1].lock = 0;
    channels[2].state = ST_EMPTY;
    channels[2].lock = 0
  };

  /* Test 1: Two competing chanAll operations */
  /* Setup: ch0 has item, ch1 empty */
  /* Thread 1 wants: GET ch0, PUT ch1 - should succeed */
  /* Thread 2 wants: PUT ch0, GET ch1 - can't both succeed simultaneously */
  atomic {
    channels[0].state = ST_HAS_ITEM;
    channels[1].state = ST_EMPTY
  };

  run thread1();
  run thread2();

  /* Wait for completion */
  (t1_done && t2_done);

  /* Verify: at most one chanAll should have fully succeeded with these states */
  /* because they have opposite requirements on the same channels */
}

/*
 * Alternative init for interference test
 */
/* Uncomment to test:
init {
  atomic {
    channels[0].state = ST_EMPTY;
    channels[0].lock = 0;
    channels[1].state = ST_EMPTY;
    channels[1].lock = 0
  };

  run chanall_getter();
  run single_putter_0();
  run single_putter_1();

  (t3_done && t4_done && t5_done)
}
*/

/*
 * LTL Properties
 */

/* No deadlock - automatic */

/* Eventually completes (under fairness) */
ltl completion {
  <> (t1_done && t2_done)
}

/* Conservation: operations are consistent */
/* chanAll(GET,PUT) on initially (HAS_ITEM, EMPTY) should result in (EMPTY, HAS_ITEM) */
ltl state_consistency {
  [] ((t1_result == AL_OP && t1_s0 == OS_GET && t1_s1 == OS_PUT) ->
      (channels[0].state != ST_HAS_ITEM || channels[1].state != ST_EMPTY ||
       t2_result == AL_OP))
}
#endif /* TEST_BLOCKING */
#endif /* TEST_FAIRNESS */

/* Atomicity: chanAll never leaves partial state. Scenario independent --
 * every chanAll inline maintains atomicity_violated. */
ltl atomicity {
  [] (!atomicity_violated)
}
