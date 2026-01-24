/*
 * chanStrFIFO.pml - Promela model for FIFO Store verification
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 * Generated with Claude Code (https://claude.ai/code)
 *
 * This models the chanStrFIFO Store implementation which provides
 * a fixed-size circular buffer for channel storage.
 *
 * Key properties to verify:
 *   1. FIFO ordering: items retrieved in order they were stored
 *   2. Conservation: no items lost or duplicated
 *   3. Capacity: respects max size, blocks when full/empty
 *   4. State consistency: head/tail pointers remain valid
 */

/* Store configuration */
#define STORE_SIZE  4   /* circular buffer size */
#define NUM_ITEMS   8   /* total items to transfer */

/* Store state */
byte store[STORE_SIZE];  /* circular buffer (item values 1-255, 0=empty slot) */
byte head;               /* read position */
byte tail;               /* write position */
byte count;              /* number of items in store */

/* For FIFO verification */
byte next_put_value;     /* next value to put (1, 2, 3, ...) */
byte next_get_value;     /* expected next value to get */
byte total_put;          /* count of successful puts */
byte total_get;          /* count of successful gets */

/* Thread completion flags */
bool producer_done;
bool consumer_done;

/* Error flag */
bool fifo_violation;

/*
 * Store can accept a Put (not full)
 */
inline can_put(result) {
  result = (count < STORE_SIZE)
}

/*
 * Store can provide a Get (not empty)
 */
inline can_get(result) {
  result = (count > 0)
}

/*
 * Put an item into the store
 * Precondition: can_put is true
 */
inline do_put(value) {
  atomic {
    store[tail] = value;
    tail = (tail + 1) % STORE_SIZE;
    count = count + 1;
    total_put = total_put + 1
  }
}

/*
 * Get an item from the store
 * Precondition: can_get is true
 * Returns value in 'value' parameter
 */
inline do_get(value) {
  atomic {
    value = store[head];
    store[head] = 0;  /* clear slot for debugging */
    head = (head + 1) % STORE_SIZE;
    count = count - 1;
    total_get = total_get + 1
  }
}

/*
 * Producer: puts NUM_ITEMS items with values 1, 2, 3, ...
 */
proctype producer() {
  byte can;
  byte val;

  do
  :: next_put_value <= NUM_ITEMS ->
     can_put(can);
     if
     :: can ->
        val = next_put_value;
        do_put(val);
        next_put_value = next_put_value + 1
     :: !can ->
        skip  /* would block - let consumer run */
     fi
  :: next_put_value > NUM_ITEMS ->
     break
  od;
  producer_done = true
}

/*
 * Consumer: gets items and verifies FIFO order
 */
proctype consumer() {
  byte can;
  byte val;

  do
  :: total_get < NUM_ITEMS ->
     can_get(can);
     if
     :: can ->
        do_get(val);
        /* Verify FIFO ordering */
        if
        :: val != next_get_value ->
           fifo_violation = true;
           assert(false)  /* FIFO order violated */
        :: else -> skip
        fi;
        next_get_value = next_get_value + 1
     :: !can ->
        skip  /* would block - let producer run */
     fi
  :: total_get >= NUM_ITEMS ->
     break
  od;
  consumer_done = true
}

init {
  atomic {
    head = 0;
    tail = 0;
    count = 0;
    next_put_value = 1;
    next_get_value = 1;
    total_put = 0;
    total_get = 0;
    producer_done = false;
    consumer_done = false;
    fifo_violation = false
  };
  run producer();
  run consumer()
}

/*
 * LTL Properties
 */

/* Both threads eventually complete */
ltl progress { <> (producer_done && consumer_done) }

/* FIFO ordering maintained (no violation flag) */
ltl fifo_order { [] (!fifo_violation) }

/* Conservation: total_get never exceeds total_put */
ltl conservation { [] (total_get <= total_put) }

/* Bounds: count never exceeds STORE_SIZE */
ltl bounds { [] (count <= STORE_SIZE) }

/* Final state: all items transferred */
ltl complete_transfer { [] ((producer_done && consumer_done) ->
                            (total_put == NUM_ITEMS && total_get == NUM_ITEMS)) }
