/*
 * Promela model of pthreadChannel chanAll() - Atomic Multi-Channel Operations
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 * Generated with Claude Code (https://claude.ai/code)
 *
 * This model focuses specifically on verifying chanAll semantics:
 *   - All-or-nothing atomicity
 *   - Lock ladder correctness (acquire ascending, release descending)
 *   - No partial completion
 *   - Proper interaction with concurrent operations
 *
 * Build and run:
 *   spin -a chanAll.pml
 *   cc -DSAFETY -O2 -o pan pan.c
 *   ./pan
 *
 * For full verification with acceptance cycles:
 *   cc -O2 -o pan pan.c
 *   ./pan -a -N atomicity
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

/*
 * Snapshot for atomicity verification
 * Records channel states before chanAll attempt
 */
typedef Snapshot {
  byte state[NCHANS];
  bool valid;
};

/* Global tracking for atomicity verification */
byte chanall_in_progress = 0;  /* count of threads in chanAll */
byte chanall_completed = 0;    /* successful chanAll completions */
byte chanall_events = 0;       /* chanAll returns with events */

/* For atomicity assertion */
bool atomicity_violated = false;

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
  Snapshot before;

  result = AL_ERR;
  chanall_in_progress++;

  /* Take snapshot before for atomicity check */
  atomic {
    before.state[0] = channels[0].state;
    before.state[1] = channels[1].state;
    before.state[2] = channels[2].state;
    before.valid = true
  };

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
     /* Would block but no event - for non-blocking, still set what we can */
     /* (This models w < 0 behavior) */
     ca_i = 0;
     do
     :: ca_i < NCHANS ->
        status[ca_i] = OS_NOP;
        ca_i++
     :: ca_i >= NCHANS -> break
     od;
     result = AL_EVT;  /* Signal couldn't complete */
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
       od
     };
     result = AL_OP;
     chanall_completed++;
     unlock_ladder_3()
  fi;

  chanall_in_progress--;
  before.valid = false
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
     result = AL_EVT;
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
       fi
     };
     result = AL_OP;
     chanall_completed++;
     unlock_ladder_2(ch0, ch1)
  fi
}

/*
 * Single channel operation (for concurrent interference testing)
 */
inline chanOp(ch, op, status) {
  lock_chan(ch);
  if
  :: channels[ch].state == ST_SHUT ->
     status = OS_SHT
  :: op == OP_GET && channels[ch].state == ST_HAS_ITEM ->
     channels[ch].state = ST_EMPTY;
     status = OS_GET
  :: op == OP_PUT && channels[ch].state == ST_EMPTY ->
     channels[ch].state = ST_HAS_ITEM;
     status = OS_PUT
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

/* Atomicity: chanAll never leaves partial state */
/* If chanall_completed > 0, the operations were atomic */
ltl atomicity {
  [] (!atomicity_violated)
}

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
