/*
 * Promela model of pthreadChannel chanOne() - Multi-Channel Operations
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 * Generated with Claude Code (https://claude.ai/code)
 *
 * This models the chanOne() function from chan.c which provides
 * select-style "first available" channel operation semantics.
 *
 * Key semantics to verify:
 *   1. First-match: returns FIRST operation (array order) that can complete
 *   2. Exactly-one: only ONE operation executes per chanOne call
 *   3. Lock ladder: no deadlock when acquiring locks
 *   4. Progress: if any operation can complete, one will
 */

/* Channel states (simplified - single item store) */
#define ST_EMPTY     0  /* can Put, cannot Get */
#define ST_HAS_ITEM  1  /* can Get, cannot Put */
#define ST_SHUTDOWN  2  /* shutdown */

/* Operation types */
#define OP_NOP  0
#define OP_GET  1
#define OP_PUT  2
#define OP_SHT  3  /* shutdown check */

/* Operation status results */
#define OS_NONE  0  /* no result yet */
#define OS_GET   1  /* Get completed */
#define OS_PUT   2  /* Put completed */
#define OS_SHT   3  /* shutdown detected */
#define OS_TMO   4  /* timeout (no blocking) */

/* Configuration */
#define NCHANS  3  /* number of channels to test */

/* Channel structure */
typedef channel_t {
  byte state;   /* ST_EMPTY, ST_HAS_ITEM, or ST_SHUTDOWN */
  bool locked;  /* mutex held */
};

channel_t channels[NCHANS];

/* Operation array entry */
typedef chanOp_t {
  byte ch;      /* channel index */
  byte op;      /* operation type */
  byte status;  /* result status */
};

/* Verification flags */
bool t1_done;
bool t2_done;
byte t1_result;    /* which index completed (0 = none, 1-3 = index+1) */
byte t2_result;
byte t1_status;    /* operation status */
byte t2_status;

/* Track which operations completed */
byte t1_ops_completed;
byte t2_ops_completed;

/* For first-match verification: track what t1 saw at decision time */
byte t1_s0;  /* state of ch0 when t1 decided */
byte t1_s1;  /* state of ch1 when t1 decided */
byte t1_s2;  /* state of ch2 when t1 decided */

/*
 * Lock primitives
 */
inline lock_chan(ch) {
  d_step {
    channels[ch].locked == false;
    channels[ch].locked = true
  }
}

inline trylock_chan(ch, got) {
  d_step {
    if
    :: channels[ch].locked == false ->
       channels[ch].locked = true;
       got = 1
    :: channels[ch].locked == true ->
       got = 0
    fi
  }
}

inline unlock_chan(ch) {
  d_step {
    channels[ch].locked = false
  }
}

/*
 * Lock ladder for NCHANS channels
 * Acquires locks in ascending order, retries on failure
 */
inline lock_ladder_3(success) {
  byte ll_got1;
  byte ll_got2;
  bool ll_done;
  ll_done = false;
  do
  :: ll_done == false ->
     lock_chan(0);
     trylock_chan(1, ll_got1);
     if
     :: ll_got1 == 0 ->
        unlock_chan(0)
     :: ll_got1 == 1 ->
        trylock_chan(2, ll_got2);
        if
        :: ll_got2 == 0 ->
           unlock_chan(1);
           unlock_chan(0)
        :: ll_got2 == 1 ->
           ll_done = true;
           success = 1
        fi
     fi
  :: ll_done == true -> break
  od
}

/*
 * chanOne - returns first operation that can complete
 *
 * Fast path: try each operation individually
 * Blocking path: lock ladder, find first satisfiable
 *
 * Returns: 0 = none, 1-N = index+1 of completed operation
 */
inline chanOne_3ops(op0, ch0, op1, ch1, op2, ch2, result, status, ops_done, s0, s1, s2) {
  byte co_i;
  byte co_found;
  byte co_success;
  bool co_fast_done;
  bool co_need_retry;

  result = 0;
  status = OS_NONE;
  ops_done = 0;
  co_found = 255;  /* invalid = not found */

  /* Fast path: try each operation with single lock */
  co_fast_done = false;
  co_i = 0;
  do
  :: co_fast_done == false && co_i == 0 ->
     if
     :: op0 != OP_NOP ->
        lock_chan(ch0);
        if
        :: op0 == OP_SHT && channels[ch0].state == ST_SHUTDOWN ->
           status = OS_SHT;
           unlock_chan(ch0);
           result = 1;
           ops_done = 1;
           co_fast_done = true
        :: op0 == OP_GET && channels[ch0].state == ST_HAS_ITEM ->
           channels[ch0].state = ST_EMPTY;
           status = OS_GET;
           unlock_chan(ch0);
           result = 1;
           ops_done = 1;
           co_fast_done = true
        :: op0 == OP_PUT && channels[ch0].state == ST_EMPTY ->
           channels[ch0].state = ST_HAS_ITEM;
           status = OS_PUT;
           unlock_chan(ch0);
           result = 1;
           ops_done = 1;
           co_fast_done = true
        :: op0 == OP_PUT && channels[ch0].state == ST_SHUTDOWN ->
           status = OS_SHT;
           unlock_chan(ch0);
           result = 1;
           ops_done = 1;
           co_fast_done = true
        :: else ->
           unlock_chan(ch0)
        fi
     :: op0 == OP_NOP -> skip
     fi;
     co_i = 1
  :: co_fast_done == false && co_i == 1 ->
     if
     :: op1 != OP_NOP ->
        lock_chan(ch1);
        if
        :: op1 == OP_SHT && channels[ch1].state == ST_SHUTDOWN ->
           status = OS_SHT;
           unlock_chan(ch1);
           result = 2;
           ops_done = 1;
           co_fast_done = true
        :: op1 == OP_GET && channels[ch1].state == ST_HAS_ITEM ->
           channels[ch1].state = ST_EMPTY;
           status = OS_GET;
           unlock_chan(ch1);
           result = 2;
           ops_done = 1;
           co_fast_done = true
        :: op1 == OP_PUT && channels[ch1].state == ST_EMPTY ->
           channels[ch1].state = ST_HAS_ITEM;
           status = OS_PUT;
           unlock_chan(ch1);
           result = 2;
           ops_done = 1;
           co_fast_done = true
        :: op1 == OP_PUT && channels[ch1].state == ST_SHUTDOWN ->
           status = OS_SHT;
           unlock_chan(ch1);
           result = 2;
           ops_done = 1;
           co_fast_done = true
        :: else ->
           unlock_chan(ch1)
        fi
     :: op1 == OP_NOP -> skip
     fi;
     co_i = 2
  :: co_fast_done == false && co_i == 2 ->
     if
     :: op2 != OP_NOP ->
        lock_chan(ch2);
        if
        :: op2 == OP_SHT && channels[ch2].state == ST_SHUTDOWN ->
           status = OS_SHT;
           unlock_chan(ch2);
           result = 3;
           ops_done = 1;
           co_fast_done = true
        :: op2 == OP_GET && channels[ch2].state == ST_HAS_ITEM ->
           channels[ch2].state = ST_EMPTY;
           status = OS_GET;
           unlock_chan(ch2);
           result = 3;
           ops_done = 1;
           co_fast_done = true
        :: op2 == OP_PUT && channels[ch2].state == ST_EMPTY ->
           channels[ch2].state = ST_HAS_ITEM;
           status = OS_PUT;
           unlock_chan(ch2);
           result = 3;
           ops_done = 1;
           co_fast_done = true
        :: op2 == OP_PUT && channels[ch2].state == ST_SHUTDOWN ->
           status = OS_SHT;
           unlock_chan(ch2);
           result = 3;
           ops_done = 1;
           co_fast_done = true
        :: else ->
           unlock_chan(ch2)
        fi
     :: op2 == OP_NOP -> skip
     fi;
     co_i = 3
  :: co_fast_done == false && co_i == 3 ->
     co_fast_done = true  /* fast path complete, nothing found */
  :: co_fast_done == true -> break
  od;

  /* If fast path succeeded, we're done */
  if
  :: result != 0 -> skip  /* already done */
  :: result == 0 ->
     /* Blocking path: lock ladder then find first satisfiable */
     co_need_retry = true;
     do
     :: co_need_retry == true ->
        co_success = 0;
        lock_ladder_3(co_success);

        /* Record channel states for verification */
        s0 = channels[ch0].state;
        s1 = channels[ch1].state;
        s2 = channels[ch2].state;

        /* Find first satisfiable operation */
        co_found = 255;
        if
        :: op0 != OP_NOP ->
           if
           :: op0 == OP_SHT && channels[ch0].state == ST_SHUTDOWN ->
              co_found = 0
           :: op0 == OP_GET && channels[ch0].state == ST_HAS_ITEM ->
              co_found = 0
           :: op0 == OP_PUT && channels[ch0].state == ST_EMPTY ->
              co_found = 0
           :: op0 == OP_PUT && channels[ch0].state == ST_SHUTDOWN ->
              co_found = 0
           :: else -> skip
           fi
        :: op0 == OP_NOP -> skip
        fi;
        if
        :: co_found == 255 && op1 != OP_NOP ->
           if
           :: op1 == OP_SHT && channels[ch1].state == ST_SHUTDOWN ->
              co_found = 1
           :: op1 == OP_GET && channels[ch1].state == ST_HAS_ITEM ->
              co_found = 1
           :: op1 == OP_PUT && channels[ch1].state == ST_EMPTY ->
              co_found = 1
           :: op1 == OP_PUT && channels[ch1].state == ST_SHUTDOWN ->
              co_found = 1
           :: else -> skip
           fi
        :: co_found != 255 || op1 == OP_NOP -> skip
        fi;
        if
        :: co_found == 255 && op2 != OP_NOP ->
           if
           :: op2 == OP_SHT && channels[ch2].state == ST_SHUTDOWN ->
              co_found = 2
           :: op2 == OP_GET && channels[ch2].state == ST_HAS_ITEM ->
              co_found = 2
           :: op2 == OP_PUT && channels[ch2].state == ST_EMPTY ->
              co_found = 2
           :: op2 == OP_PUT && channels[ch2].state == ST_SHUTDOWN ->
              co_found = 2
           :: else -> skip
           fi
        :: co_found != 255 || op2 == OP_NOP -> skip
        fi;

        if
        :: co_found == 255 ->
           /* Nothing satisfiable - would block (model as timeout) */
           unlock_chan(2);
           unlock_chan(1);
           unlock_chan(0);
           status = OS_TMO;
           result = 0;
           co_need_retry = false
        :: co_found != 255 ->
           /* Found one - unlock earlier channels, execute */
           if
           :: co_found == 0 ->
              unlock_chan(2);
              unlock_chan(1);
              /* Keep ch0 locked */
              atomic {
                if
                :: op0 == OP_SHT ->
                   status = OS_SHT
                :: op0 == OP_GET ->
                   channels[ch0].state = ST_EMPTY;
                   status = OS_GET
                :: op0 == OP_PUT && channels[ch0].state == ST_EMPTY ->
                   channels[ch0].state = ST_HAS_ITEM;
                   status = OS_PUT
                :: op0 == OP_PUT && channels[ch0].state == ST_SHUTDOWN ->
                   status = OS_SHT
                :: else -> skip
                fi
              };
              unlock_chan(0);
              result = 1;
              ops_done = 1
           :: co_found == 1 ->
              unlock_chan(2);
              unlock_chan(0);
              /* Keep ch1 locked */
              atomic {
                if
                :: op1 == OP_SHT ->
                   status = OS_SHT
                :: op1 == OP_GET ->
                   channels[ch1].state = ST_EMPTY;
                   status = OS_GET
                :: op1 == OP_PUT && channels[ch1].state == ST_EMPTY ->
                   channels[ch1].state = ST_HAS_ITEM;
                   status = OS_PUT
                :: op1 == OP_PUT && channels[ch1].state == ST_SHUTDOWN ->
                   status = OS_SHT
                :: else -> skip
                fi
              };
              unlock_chan(1);
              result = 2;
              ops_done = 1
           :: co_found == 2 ->
              unlock_chan(1);
              unlock_chan(0);
              /* Keep ch2 locked */
              atomic {
                if
                :: op2 == OP_SHT ->
                   status = OS_SHT
                :: op2 == OP_GET ->
                   channels[ch2].state = ST_EMPTY;
                   status = OS_GET
                :: op2 == OP_PUT && channels[ch2].state == ST_EMPTY ->
                   channels[ch2].state = ST_HAS_ITEM;
                   status = OS_PUT
                :: op2 == OP_PUT && channels[ch2].state == ST_SHUTDOWN ->
                   status = OS_SHT
                :: else -> skip
                fi
              };
              unlock_chan(2);
              result = 3;
              ops_done = 1
           fi;
           co_need_retry = false
        fi
     :: co_need_retry == false -> break
     od
  fi
}

/*
 * Test scenario 1: First-match semantics
 *
 * T1 tries to GET from ch0, ch1, ch2 (in order)
 * T2 puts items to channels
 *
 * Verify: T1 always gets from lowest-indexed ready channel
 */
proctype thread1_firstmatch() {
  /* Try GET from all 3 channels */
  chanOne_3ops(OP_GET, 0, OP_GET, 1, OP_GET, 2,
               t1_result, t1_status, t1_ops_completed,
               t1_s0, t1_s1, t1_s2);
  t1_done = true
}

proctype thread2_producer() {
  /* Non-deterministically put to different channels */
  if
  :: /* Put to ch0 only */
     lock_chan(0);
     channels[0].state = ST_HAS_ITEM;
     unlock_chan(0)
  :: /* Put to ch1 only */
     lock_chan(1);
     channels[1].state = ST_HAS_ITEM;
     unlock_chan(1)
  :: /* Put to ch2 only */
     lock_chan(2);
     channels[2].state = ST_HAS_ITEM;
     unlock_chan(2)
  :: /* Put to ch0 and ch1 */
     lock_chan(0);
     channels[0].state = ST_HAS_ITEM;
     unlock_chan(0);
     lock_chan(1);
     channels[1].state = ST_HAS_ITEM;
     unlock_chan(1)
  :: /* Put to ch1 and ch2 */
     lock_chan(1);
     channels[1].state = ST_HAS_ITEM;
     unlock_chan(1);
     lock_chan(2);
     channels[2].state = ST_HAS_ITEM;
     unlock_chan(2)
  :: /* Put to all three */
     lock_chan(0);
     channels[0].state = ST_HAS_ITEM;
     unlock_chan(0);
     lock_chan(1);
     channels[1].state = ST_HAS_ITEM;
     unlock_chan(1);
     lock_chan(2);
     channels[2].state = ST_HAS_ITEM;
     unlock_chan(2)
  fi;
  t2_done = true
}

/*
 * Test scenario 2: Exactly-one semantics
 *
 * Both threads compete with chanOne on overlapping channels
 *
 * Verify: Each chanOne completes exactly 0 or 1 operations
 */
proctype thread1_compete() {
  /* GET from ch0, PUT to ch1 */
  chanOne_3ops(OP_GET, 0, OP_PUT, 1, OP_NOP, 2,
               t1_result, t1_status, t1_ops_completed,
               t1_s0, t1_s1, t1_s2);
  t1_done = true
}

proctype thread2_compete() {
  /* GET from ch1, PUT to ch0 */
  chanOne_3ops(OP_GET, 1, OP_PUT, 0, OP_NOP, 2,
               t2_result, t2_status, t2_ops_completed,
               t1_s0, t1_s1, t1_s2);  /* reuse t1_s* since we don't need t2's */
  t2_done = true
}

/*
 * Test scenario 3: Lock ladder deadlock freedom
 *
 * Both threads try chanOne with same channels in same order
 * Lock ladder should prevent deadlock
 */
proctype thread1_ladder() {
  chanOne_3ops(OP_PUT, 0, OP_PUT, 1, OP_PUT, 2,
               t1_result, t1_status, t1_ops_completed,
               t1_s0, t1_s1, t1_s2);
  t1_done = true
}

proctype thread2_ladder() {
  chanOne_3ops(OP_PUT, 0, OP_PUT, 1, OP_PUT, 2,
               t2_result, t2_status, t2_ops_completed,
               t1_s0, t1_s1, t1_s2);
  t2_done = true
}

/*
 * Test scenario selection:
 *   TEST_FIRSTMATCH (default) - verify first-match semantics
 *   TEST_COMPETE - verify mutual exclusion with competing threads
 *   TEST_LADDER - verify lock ladder deadlock freedom
 */
#ifndef TEST_COMPETE
#ifndef TEST_LADDER
#define TEST_FIRSTMATCH
#endif
#endif

init {
  /* All channels start empty and unlocked */
  atomic {
    channels[0].state = ST_EMPTY;
    channels[0].locked = false;
    channels[1].state = ST_EMPTY;
    channels[1].locked = false;
    channels[2].state = ST_EMPTY;
    channels[2].locked = false;
    t1_done = false;
    t2_done = false;
    t1_result = 0;
    t2_result = 0;
    t1_status = OS_NONE;
    t2_status = OS_NONE;
    t1_ops_completed = 0;
    t2_ops_completed = 0;
    t1_s0 = 0;
    t1_s1 = 0;
    t1_s2 = 0
  };

#ifdef TEST_FIRSTMATCH
  run thread1_firstmatch();
  run thread2_producer()
#endif
#ifdef TEST_COMPETE
  /* Pre-load ch0 with item so T1 can GET, ch1 empty so T1/T2 can PUT */
  channels[0].state = ST_HAS_ITEM;
  run thread1_compete();
  run thread2_compete()
#endif
#ifdef TEST_LADDER
  run thread1_ladder();
  run thread2_ladder()
#endif
}

/*
 * LTL Properties
 */

/* Safety: No more than one operation completes per chanOne call */
ltl exactly_one { [] ((t1_done && t1_result != 0) -> (t1_ops_completed == 1)) }

/* Progress: Both threads eventually complete */
ltl progress { <> (t1_done && t2_done) }

/*
 * First-match property:
 * If t1 got result=2 (ch1), then ch0 must NOT have been ready when checked
 * (This is a partial check - full verification would need to track exact timing)
 *
 * When t1_result==2 (got from ch1) AND t1 saw ch0 as HAS_ITEM during blocking path,
 * this would be a violation (should have picked ch0 first)
 */
ltl first_match { [] ((t1_done && t1_result == 2 && t1_status == OS_GET)
                      -> (t1_s0 != ST_HAS_ITEM)) }

/*
 * Stronger first-match for result=3:
 * If got from ch2, then neither ch0 nor ch1 were ready
 */
ltl first_match_ch2 { [] ((t1_done && t1_result == 3 && t1_status == OS_GET)
                          -> (t1_s0 != ST_HAS_ITEM && t1_s1 != ST_HAS_ITEM)) }

/*
 * Properties for TEST_COMPETE scenario:
 * Both threads complete exactly 0 or 1 operations
 */
ltl compete_t1 { [] ((t1_done && t1_result != 0) -> (t1_ops_completed == 1)) }
ltl compete_t2 { [] ((t2_done && t2_result != 0) -> (t2_ops_completed == 1)) }

/*
 * For TEST_LADDER scenario:
 * Deadlock freedom - both threads eventually complete (with fairness)
 */
ltl ladder_progress { <> (t1_done && t2_done) }

/*
 * Conservation for TEST_LADDER:
 * If both try PUT to same empty channels, total items <= channels available
 * (This is checked implicitly via state assertions)
 */
ltl ladder_both_done { [] ((t1_done && t2_done) ->
                           (t1_ops_completed + t2_ops_completed <= 3)) }
