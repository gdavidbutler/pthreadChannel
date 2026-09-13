/*
 * Promela model of pthreadChannel chan.c
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 *
 * Generated with Claude Code (https://claude.ai/code)
 *
 * Models the core synchronization logic to verify:
 *   - Deadlock freedom (the default safety run)
 *   - Conservation: a Get requires a prior Put
 *   - Progress and termination of the worker scenario, under fairness
 *
 * Scenarios, selected at spin time:
 *   (default)    two workers, each Put then Get, over two Channels
 *   -DTEST_ONE   a producer and one chan_one call
 *   -DTEST_ALL   a producer and one chan_all call
 *   -DTEST_SHUT  a producer, a consumer, and a shutter -- the only
 *                scenario in which ST_SHUT occurs at all
 *
 * Until these scenarios were wired up, init ran the workers ALONE: the
 * chan_one, chan_all and chan_shut models below were never executed by any
 * verification run, and no shutdown branch anywhere was reachable.
 *
 * Blocking is a waiter count plus a blocking guard, not a condition
 * variable. So this model does not represent waking a particular waiter and
 * says nothing about arrival order or starvation; chanAll.pml
 * -DTEST_FAIRNESS covers that. Written as a spin instead of a guard, the
 * wait loop is always enabled and weak fairness admits an infinite
 * non-productive cycle, which is what made termination unprovable here.
 *
 * Build and run:
 *   spin [-DTEST_ONE|-DTEST_ALL|-DTEST_SHUT] -a chan.pml
 *   cc -DSAFETY -O2 -o pan pan.c
 *   ./pan               # safety: deadlock freedom, assertions
 *   ./pan -N conservation
 *
 * For liveness (default scenario, needs fairness and no -DSAFETY):
 *   cc -O2 -o pan pan.c
 *   ./pan -a -f -N termination
 */

/* Configuration - keep small for tractable state space */
#define NCHANS 2      /* number of channels */
#define NPROCS 2      /* number of worker processes */
#define NOPS   2      /* operations per process before done */

/* Channel states (models chanSs_t) */
#define ST_EMPTY    0  /* can put, cannot get */
#define ST_HAS_ITEM 1  /* can get, cannot put (single-item store) */
#define ST_SHUT     2  /* shutdown */

/* Operation types (models chanOp_t) */
#define OP_NOP 0
#define OP_SHT 1
#define OP_GET 2
#define OP_PUT 3

/* Operation status (models chanOs_t) */
#define OS_NOP 0
#define OS_SHT 1
#define OS_GET 2
#define OS_PUT 3
#define OS_TMO 4

/* chanAll status (models chanAl_t) */
#define AL_ERR 0
#define AL_EVT 1
#define AL_OP  2
#define AL_TMO 3

/*
 * Channel structure
 * Abstracts struct chan to essential state
 */
typedef Channel {
  byte state;        /* ST_EMPTY, ST_HAS_ITEM, ST_SHUT */
  byte open_count;   /* chanOpen not yet chanClose (chan.c c->c) */
  bool freed;        /* the deallocating chanClose has run */
  byte get_waiters;  /* count of threads waiting to get */
  byte put_waiters;  /* count of threads waiting to put */
  byte lock;         /* 0 = unlocked, N = locked by process N */
};

Channel channels[NCHANS];

/* Channels deallocated so far */
byte total_freed = 0;

/*
 * chanOpen / chanClose.
 *
 * open_count models chan.c's c->c, which counts references BEYOND the
 * creator's: chanCreate leaves it 0 with one holder, chanOpen increments,
 * and chanClose decrements while non-zero. The chanClose that finds it 0 is
 * the last holder's, and that one deallocates (chan.c:366-402).
 */
inline chan_open(ch) {
  atomic {
    assert(!channels[ch].freed);      /* no reference to a freed Channel */
    channels[ch].open_count++
  }
}

inline chan_close(ch) {
  atomic {
    assert(!channels[ch].freed);      /* no double close */
    if
    :: channels[ch].open_count > 0 ->
       channels[ch].open_count--
    :: else ->
       /* Last holder. chan.c wakes every waiter queue and spins until all
        * five have drained before it frees. A thread blocked on a Channel
        * necessarily holds a reference of its own, so reaching this point
        * with a waiter queued means someone operated on a Channel they had
        * not opened. */
       assert(channels[ch].get_waiters == 0);
       assert(channels[ch].put_waiters == 0);
       channels[ch].freed = true;
       total_freed++
    fi
  }
}

/* Statistics for verification */
byte total_gets = 0;
byte total_puts = 0;
byte total_ops = 0;
bool all_done = false;

/* Process completion tracking */
bool proc_done[NPROCS];

/*
 * Lock acquisition - models pthread_mutex_lock
 */
inline lock_chan(ch) {
  atomic {
    (channels[ch].lock == 0) -> channels[ch].lock = _pid + 1
  }
}

/*
 * Lock release - models pthread_mutex_unlock
 */
inline unlock_chan(ch) {
  channels[ch].lock = 0
}

/*
 * Try lock - models pthread_mutex_trylock
 * Sets 'got_lock' to result
 */
inline trylock_chan(ch, got_lock) {
  atomic {
    if
    :: channels[ch].lock == 0 ->
       channels[ch].lock = _pid + 1;
       got_lock = 1
    :: else ->
       got_lock = 0
    fi
  }
}

/*
 * Lock ladder for multiple channels
 * Acquire locks in ascending order, retry on failure
 * Models the trylock/yield loop in chanOne/chanAll
 */
inline lock_ladder(n, arr, success) {
  byte ll_i;
  byte ll_got;
  bool ll_retry;

  success = 0;
  do
  :: success == 0 ->
     ll_retry = false;
     ll_i = 0;
     do
     :: ll_i < n ->
        if
        :: ll_i == 0 ->
           lock_chan(arr[ll_i]);
           ll_i++
        :: ll_i > 0 ->
           trylock_chan(arr[ll_i], ll_got);
           if
           :: ll_got == 1 -> ll_i++
           :: ll_got == 0 ->
              /* Release all and retry - models sched_yield loop */
              byte ll_j = 0;
              do
              :: ll_j < ll_i ->
                 unlock_chan(arr[ll_j]);
                 ll_j++
              :: ll_j >= ll_i -> break
              od;
              ll_retry = true;
              break
           fi
        fi
     :: ll_i >= n -> break
     od;
     if
     :: ll_retry == false -> success = 1
     :: ll_retry == true -> skip  /* retry the outer loop */
     fi
  :: success == 1 -> break
  od
}

/*
 * Release locks in descending order
 */
inline unlock_ladder(n, arr) {
  byte ul_i = n;
  do
  :: ul_i > 0 ->
     ul_i--;
     unlock_chan(arr[ul_i])
  :: ul_i == 0 -> break
  od
}

/*
 * Wake waiting threads on a channel
 * Models the WAKE macro
 */
inline wake_getters(ch) {
  /* In real impl, signal condition variable */
  /* Here we just note waiters can proceed */
  skip
}

inline wake_putters(ch) {
  skip
}

/*
 * chanOp on single channel (simplified)
 * Models blocking Get or Put
 */
inline chan_op(ch, op, status) {
  assert(!channels[ch].freed);        /* no use after free */
  lock_chan(ch);

  if
  :: channels[ch].state == ST_SHUT ->
     status = OS_SHT;
     unlock_chan(ch)

  :: op == OP_GET && channels[ch].state == ST_HAS_ITEM ->
     channels[ch].state = ST_EMPTY;
     status = OS_GET;
     total_gets++;
     wake_putters(ch);
     unlock_chan(ch)

  :: op == OP_PUT && channels[ch].state == ST_EMPTY ->
     channels[ch].state = ST_HAS_ITEM;
     status = OS_PUT;
     total_puts++;
     wake_getters(ch);
     unlock_chan(ch)

  :: op == OP_GET && channels[ch].state == ST_EMPTY ->
     /* Must wait */
     channels[ch].get_waiters++;
     unlock_chan(ch);
     /* Block until the Channel can serve us. This guard is the model of
      * pthread_cond_wait: it is NOT enabled while the Store is empty, so
      * the waiter contributes no states while it waits. Written as a spin
      * (do :: true -> lock; recheck; unlock od) the loop is always enabled,
      * and weak fairness then admits an infinite non-productive cycle --
      * which is why liveness could not be stated here before. */
     do
     :: (channels[ch].state != ST_EMPTY) ->
        lock_chan(ch);
        if
        :: channels[ch].state == ST_SHUT ->
           channels[ch].get_waiters--;
           status = OS_SHT;
           unlock_chan(ch);
           break
        :: channels[ch].state == ST_HAS_ITEM ->
           channels[ch].get_waiters--;
           channels[ch].state = ST_EMPTY;
           status = OS_GET;
           total_gets++;
           wake_putters(ch);
           unlock_chan(ch);
           break
        :: else ->
           unlock_chan(ch)
           /* spin/wait */
        fi
     od

  :: op == OP_PUT && channels[ch].state == ST_HAS_ITEM ->
     /* Must wait */
     channels[ch].put_waiters++;
     unlock_chan(ch);
     /* see the Get side above: a blocking guard, not a spin */
     do
     :: (channels[ch].state != ST_HAS_ITEM) ->
        lock_chan(ch);
        if
        :: channels[ch].state == ST_SHUT ->
           channels[ch].put_waiters--;
           status = OS_SHT;
           unlock_chan(ch);
           break
        :: channels[ch].state == ST_EMPTY ->
           channels[ch].put_waiters--;
           channels[ch].state = ST_HAS_ITEM;
           status = OS_PUT;
           total_puts++;
           wake_getters(ch);
           unlock_chan(ch);
           break
        :: else ->
           unlock_chan(ch)
        fi
     od
  fi
}

/*
 * chanOne - operate on first available channel
 * Models the select-style behavior
 *
 * arr: array of channel indices
 * ops: array of operations (OP_GET or OP_PUT)
 * n: count
 * result_idx: which channel succeeded (0-based)
 * result_status: operation status
 */
inline chan_one(n, arr, ops, result_idx, result_status) {
  byte co_i;
  byte co_j;
  byte co_found;
  bool co_success;
  bool co_need_cleanup;

  result_idx = 255;
  result_status = OS_NOP;
  co_need_cleanup = false;

  /* First pass: check without blocking */
  co_i = 0;
  do
  :: co_i < n ->
     lock_chan(arr[co_i]);
     if
     :: channels[arr[co_i]].state == ST_SHUT ->
        result_idx = co_i;
        result_status = OS_SHT;
        unlock_chan(arr[co_i]);
        break
     :: ops[co_i] == OP_GET && channels[arr[co_i]].state == ST_HAS_ITEM ->
        channels[arr[co_i]].state = ST_EMPTY;
        result_idx = co_i;
        result_status = OS_GET;
        total_gets++;
        unlock_chan(arr[co_i]);
        break
     :: ops[co_i] == OP_PUT && channels[arr[co_i]].state == ST_EMPTY ->
        channels[arr[co_i]].state = ST_HAS_ITEM;
        result_idx = co_i;
        result_status = OS_PUT;
        total_puts++;
        unlock_chan(arr[co_i]);
        break
     :: else ->
        unlock_chan(arr[co_i]);
        co_i++
     fi
  :: co_i >= n -> break
  od;

  /* If no immediate success, must wait (simplified) */
  if
  :: result_idx == 255 ->
     /* Register as waiter on all channels */
     co_i = 0;
     do
     :: co_i < n ->
        lock_chan(arr[co_i]);
        if
        :: ops[co_i] == OP_GET -> channels[arr[co_i]].get_waiters++
        :: ops[co_i] == OP_PUT -> channels[arr[co_i]].put_waiters++
        :: else -> skip
        fi;
        unlock_chan(arr[co_i]);
        co_i++
     :: co_i >= n -> break
     od;

     /* Wait loop */
     do
     :: co_need_cleanup == false ->
        co_i = 0;
        do
        :: co_i < n && co_need_cleanup == false ->
           lock_chan(arr[co_i]);
           if
           :: channels[arr[co_i]].state == ST_SHUT ->
              if
              :: ops[co_i] == OP_GET -> channels[arr[co_i]].get_waiters--
              :: ops[co_i] == OP_PUT -> channels[arr[co_i]].put_waiters--
              :: else -> skip
              fi;
              result_idx = co_i;
              result_status = OS_SHT;
              unlock_chan(arr[co_i]);
              co_need_cleanup = true;
              break
           :: ops[co_i] == OP_GET && channels[arr[co_i]].state == ST_HAS_ITEM ->
              channels[arr[co_i]].get_waiters--;
              channels[arr[co_i]].state = ST_EMPTY;
              result_idx = co_i;
              result_status = OS_GET;
              total_gets++;
              unlock_chan(arr[co_i]);
              co_need_cleanup = true;
              break
           :: ops[co_i] == OP_PUT && channels[arr[co_i]].state == ST_EMPTY ->
              channels[arr[co_i]].put_waiters--;
              channels[arr[co_i]].state = ST_HAS_ITEM;
              result_idx = co_i;
              result_status = OS_PUT;
              total_puts++;
              unlock_chan(arr[co_i]);
              co_need_cleanup = true;
              break
           :: else ->
              unlock_chan(arr[co_i]);
              co_i++
           fi
        :: co_i >= n || co_need_cleanup == true -> break
        od
     :: co_need_cleanup == true -> break
     od;

     /* Unregister from remaining channels */
     co_j = 0;
     do
     :: co_j < n ->
        if
        :: co_j != result_idx ->
           lock_chan(arr[co_j]);
           if
           :: ops[co_j] == OP_GET && channels[arr[co_j]].get_waiters > 0 ->
              channels[arr[co_j]].get_waiters--
           :: ops[co_j] == OP_PUT && channels[arr[co_j]].put_waiters > 0 ->
              channels[arr[co_j]].put_waiters--
           :: else -> skip
           fi;
           unlock_chan(arr[co_j])
        :: else -> skip
        fi;
        co_j++
     :: co_j >= n -> break
     od
  :: else -> skip
  fi
}

/*
 * chanAll - atomic all-or-nothing operation
 * Models the core chanAll semantics
 *
 * Returns AL_OP if all succeeded, AL_EVT if any failed
 */
inline chan_all(n, arr, ops, result) {
  byte ca_i;
  byte ca_j;
  bool ca_can_do;
  bool ca_has_event;
  bool ca_success;

  result = AL_ERR;

  /* Lock ladder - acquire all locks in order */
  lock_ladder(n, arr, ca_success);
  assert(ca_success == 1);

  /* Check if all operations can proceed */
  ca_can_do = true;
  ca_has_event = false;
  ca_i = 0;
  do
  :: ca_i < n ->
     if
     :: channels[arr[ca_i]].state == ST_SHUT ->
        ca_has_event = true;
        ca_can_do = false
     :: ops[ca_i] == OP_GET && channels[arr[ca_i]].state != ST_HAS_ITEM ->
        ca_can_do = false
     :: ops[ca_i] == OP_PUT && channels[arr[ca_i]].state != ST_EMPTY ->
        ca_can_do = false
     :: else -> skip
     fi;
     ca_i++
  :: ca_i >= n -> break
  od;

  if
  :: ca_has_event ->
     /* Event occurred (shutdown), report but don't operate */
     result = AL_EVT;
     unlock_ladder(n, arr)

  :: ca_can_do == false && ca_has_event == false ->
     /* Nothing waits here, so this is the nsTimeout < 0 call: it reports a
      * timeout having operated nothing. AL_EVT would claim an event
      * occurred, which is a different report (chan.h chanAlEvt vs
      * chanAlTmo) and would hide a would-block behind a shutdown. */
     result = AL_TMO;
     unlock_ladder(n, arr)

  :: ca_can_do == true ->
     /* All operations can proceed - do them atomically */
     ca_i = 0;
     do
     :: ca_i < n ->
        if
        :: ops[ca_i] == OP_GET ->
           assert(channels[arr[ca_i]].state == ST_HAS_ITEM);
           channels[arr[ca_i]].state = ST_EMPTY;
           total_gets++
        :: ops[ca_i] == OP_PUT ->
           assert(channels[arr[ca_i]].state == ST_EMPTY);
           channels[arr[ca_i]].state = ST_HAS_ITEM;
           total_puts++
        :: else -> skip
        fi;
        ca_i++
     :: ca_i >= n -> break
     od;
     result = AL_OP;
     unlock_ladder(n, arr)
  fi
}

/*
 * chanShut - shutdown a channel
 */
inline chan_shut(ch) {
  lock_chan(ch);
  channels[ch].state = ST_SHUT;
  /* Wake all waiters */
  channels[ch].get_waiters = 0;
  channels[ch].put_waiters = 0;
  unlock_chan(ch)
}

/*
 * Initialize channels
 */
init {
  byte i;

  /* Initialize all channels */
  atomic {
    i = 0;
    do
    :: i < NCHANS ->
       channels[i].state = ST_EMPTY;
       channels[i].open_count = 0;   /* created: one holder, no extra refs */
       channels[i].freed = false;
       channels[i].get_waiters = 0;
       channels[i].put_waiters = 0;
       channels[i].lock = 0;
       i++
    :: i >= NCHANS -> break
    od;

    i = 0;
    do
    :: i < NPROCS ->
       proc_done[i] = false;
       i++
    :: i >= NPROCS -> break
    od
  };

  /* Start the scenario */
  atomic {
#ifdef TEST_ONE
    run producer(0);
    run test_chan_one()
#elif defined(TEST_ALL)
    run producer(0);
    run test_chan_all()
#elif defined(TEST_SHUT)
    run producer(0);
    run consumer(0);
    run shutter(0)
#else
    run worker(0);
    run worker(1)
#endif
  };

#if !defined(TEST_ONE) && !defined(TEST_ALL) && !defined(TEST_SHUT)
  /* The creator drops its own reference once the workers are done. Every
   * worker reference is gone by then, so these are the deallocating
   * chanCloses. */
  all_done;
  chan_close(0);
  chan_close(1)
#endif
}

/*
 * Worker process - alternates put then get (realistic usage)
 * Each worker: put on "their" channel, get from "other" channel
 * This models producer/consumer pairing
 */
proctype worker(byte id) {
  byte op_count = 0;
  byte status;
  byte my_ch;
  byte other_ch;

  /* Worker 0 puts to channel 0, gets from channel 1 */
  /* Worker 1 puts to channel 1, gets from channel 0 */
  my_ch = id;
  other_ch = 1 - id;

  /* hold a reference for as long as this thread touches them */
  chan_open(my_ch);
  chan_open(other_ch);

  do
  :: op_count < NOPS ->
     /* First put, then get - ensures no livelock */
     chan_op(my_ch, OP_PUT, status);
     if
     :: status == OS_PUT -> total_ops++
     :: status == OS_SHT -> break
     :: else -> skip
     fi;

     chan_op(other_ch, OP_GET, status);
     if
     :: status == OS_GET -> total_ops++
     :: status == OS_SHT -> break
     :: else -> skip
     fi;

     op_count++

  :: op_count >= NOPS -> break
  od;

  chan_close(other_ch);
  chan_close(my_ch);

  proc_done[id] = true;

  /* Check if all processes done */
  if
  :: proc_done[0] && proc_done[1] ->
     all_done = true
  :: else -> skip
  fi
}

/*
 * Test process for chanOne
 */
proctype test_chan_one() {
  byte arr[2];
  byte ops[2];
  byte result_idx;
  byte result_status;

  arr[0] = 0;
  arr[1] = 1;
  ops[0] = OP_GET;
  ops[1] = OP_PUT;

  chan_one(2, arr, ops, result_idx, result_status);

  assert(result_status != OS_NOP || channels[0].state == ST_SHUT || channels[1].state == ST_SHUT)
}

/*
 * Test process for chanAll
 */
proctype test_chan_all() {
  byte arr[2];
  byte ops[2];
  byte result;

  arr[0] = 0;
  arr[1] = 1;
  ops[0] = OP_GET;
  ops[1] = OP_PUT;

  chan_all(2, arr, ops, result);

  /* Atomicity is asserted INSIDE chan_all, under the lock ladder, where the
   * preconditions are stable. It cannot be restated from out here: a state
   * sampled without the lock can change before chan_all takes the ladder.
   * This proctype used to record channels[0..1].state beforehand and assert
   * against it, and reported a false violation the moment the proctype was
   * actually run -- it had never been run. */
  assert(result == AL_OP || result == AL_EVT || result == AL_TMO)
}

/*
 * Producer process - only puts
 */
proctype producer(byte ch) {
  byte status;
  byte count = 0;

  do
  :: count < NOPS ->
     chan_op(ch, OP_PUT, status);
     if
     :: status == OS_PUT -> count++
     :: status == OS_SHT -> break
     :: else -> skip
     fi
  :: count >= NOPS -> break
  od
}

/*
 * Consumer process - only gets
 */
proctype consumer(byte ch) {
  byte status;
  byte count = 0;

  do
  :: count < NOPS ->
     chan_op(ch, OP_GET, status);
     if
     :: status == OS_GET -> count++
     :: status == OS_SHT -> break
     :: else -> skip
     fi
  :: count >= NOPS -> break
  od
}

/*
 * Shutdown process - shuts down a channel after delay
 */
proctype shutter(byte ch) {
  byte i;
  /* Let some operations happen first */
  i = 0;
  do
  :: i < 3 -> i++
  :: i >= 3 -> break
  od;
  chan_shut(ch)
}

/*
 * LTL Properties
 */

/* No deadlock - SPIN checks this automatically */
/* Run with: ./pan -DSAFETY */

/* Progress: every operation the workers set out to do completes. The two
 * workers each do NOPS rounds of Put-then-Get, so 4 * NOPS in all. The
 * previous form, [] (total_ops < 255 -> <> (total_ops > 0)), asked only
 * that SOME operation ever complete, and stayed true forever after the
 * first one. */
/* total_ops is counted by the worker proctype, so this claim -- like
 * termination -- belongs to the scenario that runs workers. The other
 * scenarios are covered by conservation, the default safety run, and the
 * assertions inside chan_all / test_chan_one. */
#if !defined(TEST_ONE) && !defined(TEST_ALL) && !defined(TEST_SHUT)
ltl progress {
  <> (total_ops == 4 * NOPS)
}
#endif

/* Fairness: if a process is trying, it eventually succeeds */
/* (weak fairness - checked with ./pan -a -f) */

/* chanAll atomicity assertion is inline in chan_all */

/* Eventually all workers complete (under fairness). all_done is set by the
 * worker proctype, so this claim belongs to the scenarios that run one. */
#if !defined(TEST_ONE) && !defined(TEST_ALL) && !defined(TEST_SHUT)
ltl termination {
  <> all_done
}
#endif

/* Conservation: a Get requires a prior Put, so this is the tight bound.
 * It was written with +NCHANS of slack, which no defect could exceed until
 * it had duplicated NCHANS items. */
ltl conservation {
  [] (total_gets <= total_puts)
}

#if !defined(TEST_ONE) && !defined(TEST_ALL) && !defined(TEST_SHUT)
/* Reference counting: every Channel is deallocated, and exactly once. The
 * assertions in chan_open / chan_close / chan_op carry the rest -- no use
 * after free, no close of a freed Channel, and no deallocation while a
 * thread is still queued on it. */
ltl refcount_freed { <> (total_freed == NCHANS) }
#endif
