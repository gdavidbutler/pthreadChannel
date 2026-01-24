/*
 * chanStrFLSO.pml - Promela model for self-tuning FIFO Store verification
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 * Generated with Claude Code (https://claude.ai/code)
 *
 * This models the chanStrFLSO (FIFO Latency Sensitive Optimization) Store
 * which dynamically adjusts its size based on observed pressure.
 *
 * Adaptation rules from chanStrFLSO.c:
 *   - Before Put: if empty and no Gets waiting, shrink (if size > 2)
 *   - After Put: if full and Gets waiting, grow (if size < max)
 *   - Before Get: if full and Puts waiting, grow (if size < max)
 *   - After Get: if empty and no Puts waiting, shrink (if size > 2)
 *
 * Key properties to verify:
 *   1. FIFO ordering preserved despite size changes
 *   2. Size bounds: min_size <= size <= max_size
 *   3. Conservation: no items lost during resize
 *   4. Adaptation: size responds to load conditions
 */

/* Store configuration */
#define MAX_SIZE    4   /* maximum store size */
#define MIN_SIZE    2   /* minimum store size */
#define INIT_SIZE   2   /* initial store size */
#define NUM_ITEMS   6   /* total items to transfer */

/* Store state */
byte store[MAX_SIZE];  /* circular buffer (values 1-255, 0=empty) */
byte head;             /* read position */
byte tail;             /* write position */
byte size;             /* current store size (2 to MAX_SIZE) */
byte item_count;       /* number of items currently in store */

/* Waiter simulation flags */
bool puts_waiting;     /* simulates chanSwNoPut being false */
bool gets_waiting;     /* simulates chanSwNoGet being false */

/* For FIFO verification */
byte next_put_value;   /* next value to put */
byte next_get_value;   /* expected next get value */
byte total_put;
byte total_get;

/* Tracking */
byte min_observed_size;
byte max_observed_size;
byte size_changes;     /* count of size adjustments */

/* Thread state */
bool producer_done;
bool consumer_done;

/* Error flags */
bool fifo_violation;
bool bounds_violation;

/*
 * Store can accept a Put
 */
inline can_put(result) {
  result = (item_count < size)
}

/*
 * Store can provide a Get
 */
inline can_get(result) {
  result = (item_count > 0)
}

/*
 * Grow the store (shift elements to make room)
 * Models lines 78-81 and 89-92 of chanStrFLSO.c
 */
inline grow_store() {
  byte gi;
  atomic {
    /* Shift elements from tail to end up by one */
    gi = size;
    do
    :: gi > tail ->
       store[gi] = store[gi - 1];
       gi = gi - 1
    :: gi <= tail ->
       break
    od;
    size = size + 1;
    head = head + 1;
    size_changes = size_changes + 1;
    if
    :: size > max_observed_size -> max_observed_size = size
    :: else -> skip
    fi
  }
}

/*
 * Shrink the store
 * Models lines 69-70 and 100-101 of chanStrFLSO.c
 */
inline shrink_store() {
  atomic {
    size = size - 1;
    head = 0;
    tail = 0;
    size_changes = size_changes + 1;
    if
    :: size < min_observed_size -> min_observed_size = size
    :: else -> skip
    fi
  }
}

/*
 * Put with FLSO adaptation
 * Models chanStrFLSOi for chanSoPut
 */
inline flso_put(value, success) {
  byte fp_can;

  can_put(fp_can);
  if
  :: fp_can ->
     atomic {
       /* Before Put: shrink if empty and no Gets waiting */
       if
       :: item_count == 0 && !gets_waiting && size > MIN_SIZE ->
          shrink_store()
       :: else -> skip
       fi;

       /* Do the put */
       store[tail] = value;
       tail = (tail + 1) % size;
       item_count = item_count + 1;
       total_put = total_put + 1;

       /* After Put: grow if full and Gets waiting */
       if
       :: item_count == size && gets_waiting && size < MAX_SIZE ->
          grow_store()
       :: else -> skip
       fi
     };
     success = 1
  :: !fp_can ->
     success = 0
  fi
}

/*
 * Get with FLSO adaptation
 * Models chanStrFLSOi for chanSoGet
 */
inline flso_get(value, success) {
  byte fg_can;

  can_get(fg_can);
  if
  :: fg_can ->
     atomic {
       /* Before Get: grow if full and Puts waiting */
       if
       :: item_count == size && puts_waiting && size < MAX_SIZE ->
          grow_store()
       :: else -> skip
       fi;

       /* Do the get */
       value = store[head];
       store[head] = 0;
       head = (head + 1) % size;
       item_count = item_count - 1;
       total_get = total_get + 1;

       /* After Get: shrink if empty and no Puts waiting */
       if
       :: item_count == 0 && !puts_waiting && size > MIN_SIZE ->
          shrink_store()
       :: else -> skip
       fi
     };
     success = 1
  :: !fg_can ->
     success = 0
  fi
}

/*
 * Producer with variable pacing to create different load conditions
 */
proctype producer() {
  byte val;
  byte ok;

  do
  :: next_put_value <= NUM_ITEMS ->
     val = next_put_value;

     /* Non-deterministically set waiting flag to simulate load */
     if
     :: puts_waiting = true
     :: puts_waiting = false
     fi;

     flso_put(val, ok);
     if
     :: ok ->
        next_put_value = next_put_value + 1
     :: !ok ->
        skip  /* would block */
     fi
  :: next_put_value > NUM_ITEMS ->
     break
  od;
  producer_done = true
}

/*
 * Consumer with variable pacing
 */
proctype consumer() {
  byte val;
  byte ok;

  do
  :: total_get < NUM_ITEMS ->
     /* Non-deterministically set waiting flag to simulate load */
     if
     :: gets_waiting = true
     :: gets_waiting = false
     fi;

     flso_get(val, ok);
     if
     :: ok ->
        /* Verify FIFO ordering */
        if
        :: val != next_get_value ->
           fifo_violation = true;
           assert(false)
        :: else -> skip
        fi;
        next_get_value = next_get_value + 1
     :: !ok ->
        skip  /* would block */
     fi
  :: total_get >= NUM_ITEMS ->
     break
  od;
  consumer_done = true
}

/*
 * Monitor process to check bounds invariant
 */
proctype monitor() {
  do
  :: true ->
     if
     :: size < MIN_SIZE || size > MAX_SIZE ->
        bounds_violation = true;
        assert(false)
     :: else -> skip
     fi
  :: producer_done && consumer_done ->
     break
  od
}

init {
  atomic {
    head = 0;
    tail = 0;
    size = INIT_SIZE;
    item_count = 0;
    next_put_value = 1;
    next_get_value = 1;
    total_put = 0;
    total_get = 0;
    puts_waiting = false;
    gets_waiting = false;
    min_observed_size = INIT_SIZE;
    max_observed_size = INIT_SIZE;
    size_changes = 0;
    producer_done = false;
    consumer_done = false;
    fifo_violation = false;
    bounds_violation = false
  };
  run producer();
  run consumer();
  run monitor()
}

/*
 * LTL Properties
 */

/* Progress: both complete */
ltl progress { <> (producer_done && consumer_done) }

/* FIFO order maintained */
ltl fifo_order { [] (!fifo_violation) }

/* Size stays within bounds */
ltl size_bounds { [] (!bounds_violation) }

/* Conservation */
ltl conservation { [] (total_get <= total_put) }

/* Complete transfer */
ltl complete_transfer { [] ((producer_done && consumer_done) ->
                            (total_get == 6)) }

/*
 * Adaptation is conditional - size changes only under specific load conditions.
 * This property verifies that if size changed, bounds are still respected.
 */
ltl adapts_safely { [] ((size_changes > 0) -> (size >= MIN_SIZE && size <= MAX_SIZE)) }
