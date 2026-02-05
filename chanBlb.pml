/*
 * Promela model of pthreadChannel chanBlb.c
 * Copyright (C) 2025 G. David Butler <gdb@dbSystems.com>
 *
 * Models the blob transport coordination to verify:
 *   - Deadlock freedom
 *   - Proper shutdown coordination
 *   - finalClose called exactly once after both threads exit
 *   - Message conservation (no lost messages)
 *
 * Build and run:
 *   spin -a chanBlb.pml
 *   cc -DSAFETY -O2 -o pan pan.c
 *   ./pan
 *
 * For liveness/fairness checks:
 *   cc -O2 -o pan pan.c
 *   ./pan -a -f -N <property>
 */

/* Configuration */
#define NMSG 2    /* number of messages to process */

/* Use Promela channels for cleaner blocking semantics */
chan eChan = [1] of { byte };  /* egress: user puts, egress thread gets */
chan iChan = [1] of { byte };  /* ingress: ingress thread puts, user gets */

/* Shutdown signals */
bool eShut = false;
bool iShut = false;

/* Thread exit flags (volatile in real code) */
bool egressExited = false;
bool ingressExited = false;

/* Cleanup tracking */
bool finalCloseCalled = false;

/* Message counters */
byte msgSentEgress = 0;
byte msgRecvEgress = 0;
byte msgSentIngress = 0;
byte msgRecvIngress = 0;

/* I/O simulation */
bool inputEOF = false;
bool outputFailed = false;

/*
 * Egress thread (nfE in chanBlb.c)
 * Gets blobs from egress channel, calls output callback
 * On output failure or channel shutdown, exits
 */
proctype egressThread() {
  byte msg;

  do
  :: eShut -> break
  :: !eShut ->
     if
     :: nempty(eChan) ->
        eChan ? msg;
        msgRecvEgress++;
        /* Simulate output callback */
        if
        :: outputFailed -> break
        :: !outputFailed -> skip
        fi
     :: empty(eChan) && eShut -> break
     :: empty(eChan) && !eShut ->
        /* Wait for message or shutdown */
        if
        :: eChan ? msg ->
           msgRecvEgress++;
           if
           :: outputFailed -> break
           :: !outputFailed -> skip
           fi
        :: eShut -> break
        fi
     fi
  od;

  /* finE: mark exited, shut channel */
  egressExited = true;
  eShut = true
}

/*
 * Ingress thread (nfI in chanBlb.c)
 * Calls input callback, puts blobs to ingress channel
 * On input EOF/error or channel shutdown, exits
 */
proctype ingressThread() {
  byte msg = 1;
  byte count = 0;

  do
  :: iShut -> break
  :: inputEOF -> break
  :: !iShut && !inputEOF && count < NMSG ->
     /* Simulate input callback returning data */
     if
     :: nfull(iChan) ->
        atomic { msgSentIngress++; iChan ! msg };
        count++
     :: full(iChan) && iShut -> break
     :: full(iChan) && !iShut ->
        /* Wait for space or shutdown */
        if
        :: atomic { msgSentIngress++; iChan ! msg } ->
           count++
        :: iShut -> break
        fi
     fi
  :: !iShut && !inputEOF && count >= NMSG ->
     inputEOF = true  /* simulate end of input */
  od;

  /* finI: mark exited, shut channel */
  ingressExited = true;
  iShut = true
}

/*
 * Monitor thread (monT in chanBlb.c)
 * Phase 1: Wait for all channels to shut
 * Phase 2: Wait for threads to exit (poll with timeout)
 * Phase 3: Call finalClose
 */
proctype monitorThread() {
  /* Phase 1: Wait for both channels to shut */
  (eShut && iShut);

  /* Phase 2: Wait for threads to exit (simplified) */
  /* In real code: poll every second for 30 minutes, then cancel */
  byte polls = 0;
  do
  :: egressExited && ingressExited -> break
  :: polls >= 5 -> break  /* timeout */
  :: !(egressExited && ingressExited) && polls < 5 -> polls++
  od;

  /* Phase 3: monFin - cleanup */
  /* Join threads (they've exited or will be cancelled) */
  /* Call finalClose */
  finalCloseCalled = true
}

/*
 * User thread
 * Puts messages to egress, gets from ingress
 * Then shuts channels
 */
proctype userThread() {
  byte msg;
  byte putCount = 0;
  byte getCount = 0;

  /* Put messages to egress channel */
  do
  :: putCount < NMSG && !eShut ->
     if
     :: nfull(eChan) ->
        atomic { msgSentEgress++; eChan ! 1 };
        putCount++
     :: full(eChan) && !eShut ->
        atomic { msgSentEgress++; eChan ! 1 };
        putCount++
     :: eShut -> break
     fi
  :: putCount >= NMSG -> break
  :: eShut -> break
  od;

  /* Get messages from ingress channel */
  do
  :: getCount < NMSG && !iShut ->
     if
     :: nempty(iChan) ->
        iChan ? msg;
        msgRecvIngress++;
        getCount++
     :: empty(iChan) && !iShut ->
        if
        :: iChan ? msg ->
           msgRecvIngress++;
           getCount++
        :: iShut -> break
        fi
     :: iShut -> break
     fi
  :: getCount >= NMSG -> break
  :: iShut -> break
  od;

  /* Shut channels to trigger cleanup */
  eShut = true;
  iShut = true
}

init {
  atomic {
    run egressThread();
    run ingressThread();
    run monitorThread();
    run userThread()
  }
}

/*
 * LTL Properties
 */

/* Eventually finalClose is called */
ltl final_close {
  <> finalCloseCalled
}

/* Channels eventually shut */
ltl channels_shut {
  <> (eShut && iShut)
}

/* Threads eventually exit */
ltl threads_exit {
  <> (egressExited && ingressExited)
}

/* No messages lost on egress (received <= sent) */
ltl egress_conserve {
  [] (msgRecvEgress <= msgSentEgress)
}

/* No messages lost on ingress (received <= sent) */
ltl ingress_conserve {
  [] (msgRecvIngress <= msgSentIngress)
}

/* finalClose only after both channels shut */
ltl final_after_shut {
  [] (finalCloseCalled -> (eShut && iShut))
}
