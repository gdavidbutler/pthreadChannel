## pthreadChannel

Yet another implementation of a "Channel" construct for POSIX threads (pthreads).

### Why Channels?

Parallel programming with shared memory is notoriously difficult. The standard approach, protecting shared data with mutexes and coordinating threads with condition variables, requires reasoning about all possible interleavings of concurrent execution.

The problems are well-known:
* **Deadlocks** from inconsistent lock ordering across code paths
* **Race conditions** from forgetting to lock, locking the wrong thing, or locking at the wrong granularity
* **Missed wakeups** from condition variable signal/wait races
* **Priority inversion**, **lock convoys**, and other subtle performance pathologies

Worse, these bugs are non-deterministic. Code that passes extensive testing can fail in production under different timing. A bug may manifest once per million runs, or only on certain hardware.

Channels offer a different model: instead of protecting shared data with locks, **transfer ownership of data through Channels**. A Put deposits, a Get takes — there is no sender, no receiver, no direction. A Channel is a shared Store that any number of pthreads can put into and get from. The synchronization is encapsulated within the Channel operations themselves. User code contains no mutexes, no condition variables and no barriers.

This library provides:
* **chanOp** - blocking Put or Get on a single Channel
* **chanOne** - wait for the first available operation across multiple Channels (like select)
* **chanAll** - perform all operations atomically across multiple Channels or none.
  Reach for chanAll only when atomicity is actually required (one Get cannot proceed
  without another Get also succeeding, etc.); most multi-Channel patterns are cleaner
  as a chanOne loop. See squint's `CHAN_ALL` macro and `-DUSE_CHAN_ALL=0` build,
  which substitutes a chanOne form throughout and is empirically more robust under
  stress than the chanAll form it replaces.

These primitives compose safely. Two correct Channel-based components, when connected, remain correct. This is not true of lock-based code, where combining two independently correct modules can introduce deadlocks neither had alone.

The cost is discipline: data must flow through Channels rather than be shared. But this discipline is enforced by the API, not by programmer vigilance. The result is parallel code that humans can reason about, review, and maintain.

#### CSP Heritage

This library draws from [Communicating Sequential Processes](https://en.wikipedia.org/wiki/Communicating_sequential_processes) (CSP), where concurrent processes communicate through Channels rather than sharing memory. In classic CSP, implemented in machine or assembler language using jump instructions, a rendezvous (Get and Put block until the other arrives) works well. When buffering is needed (queue, stack, etc.), it is implemented as another CSP.

But modern CSPs (coroutines, functions with local variables and recursion) have higher context switch costs. Modern processors (call/return instructions, etc.) make this acceptable. However, a pthread context switch requires operating system support and the cost is prohibitively high for simple operations like buffering.

This implementation therefore uses **callbacks instead of processes** where a trivial thread would otherwise be required:
* [Store](#Store) callbacks provide buffering without a buffer thread
* [Blob](#Blob) bridges a Channel to byte-oriented external software via callbacks that frame and do byte-I/O inline

The Channel semantics remain, but the implementation avoids threads that would do nothing but shuffle data.

### Channel

This library provides a [Channel](https://en.wikipedia.org/wiki/Channel_(programming)) style programming environment in C.
A Channel is an anonymous, pthread coordinating, [Store](#Store) of pointer (void *) sized items.

* Channels facilitate pthread based [SMP](https://en.wikipedia.org/wiki/Symmetric_multiprocessing).
(For integration with byte-oriented external software, see [Blob](#Blob).)
* Channels, by default, can store a single item.
(For more, see [Store](#Store).)
* Any number of pthreads can Put/Get on a Channel.
  * Awareness of Channel demand is supported: an operation can test whether a counterpart is already waiting, without itself committing to block. This enables [lazy evaluation](https://en.wikipedia.org/wiki/Lazy_evaluation) — a producer skips expensive work when no consumer is asking for it. See [squint](#Examples), whose recursive multiply only spawns sub-agents when its output is demanded; the lazy-evaluation pattern is critical to making that algorithm terminate.
* A pthread can Put/Get on any number of Channels.
  * Including either one, or all ([atomic broadcast](https://en.wikipedia.org/wiki/Atomic_broadcast)), of an array of operations.
* The canonical Channel use case is a transfer of a pointer to heap.
  * Putting pthread:
    ````C
    m = malloc(...);
    initialize(m);
    chanOp(chan, &m, chanOpPut);
    ````
  * Getting pthread:
    ````C
    chanOp(chan, &m, chanOpGet);
    use(m);
    free(m);
    ````
* A chan_t is itself a pointer, and Channels carry pointers — so Channels can be put through other Channels. Worth pausing on: chan_t lifetime is **not** malloc/free. It follows POSIX file-descriptor semantics — chanCreate is like `creat`, chanOpen is like `open`/`dup`, chanClose is like `close`. The reference returned by chanCreate is immediately usable; a bare chanCreate followed by chanClose is valid. chanOpen exists for *additional* references beyond the creator's. A thread that wants to keep its own reference past handing the Channel off must chanOpen first, and the Channel is freed only when every reference has been chanClosed. Once that discipline is clear, Channels can be wired into any topology — request/response (RPC) is one application:

````
     Requester                                      Responder
         │                                              │
         │  1. chanOpen(responseChan)                   │
         │                                              │
         │  2. Put responseChan    ┌─────────────┐      │
         ├────────────────────────►│ serviceChan │──────┤ 3. Get responseChan
         │                         └─────────────┘      │
         │                                              │
         │                         ┌─────────────┐      │
         ├◄────────────────────────│ responseChan│◄─────┤ 4. Put response
         │  5. Get response        └─────────────┘      │
         │                                              │
         │                                              │ 6. chanClose(responseChan)
         ▼                                              ▼    (delegated ownership)
````

  The discipline shows in two places: the requester chanOpens the response Channel before passing it (otherwise the responder's chanClose could free the Channel before the requester's Get completes), and the responder chanCloses after Put — ownership of that reference was delegated through the Channel itself.

  * requesting pthread:
    ````C
    chanOpen(responseChan);
    chanOp(serviceChan, &responseChan, chanOpPut);
    chanOp(responseChan, &response, chanOpGet);
    ````
  * responding pthread:
    ````C
    chanOp(serviceChan, &responseChan, chanOpGet);
    chanOp(responseChan, &response, chanOpPut);
    chanClose(responseChan);
    ````
* Pthreads are serviced fairly, avoiding thundering herd:
  * Waiters queue in order; when an item is available, only the next waiter is woken, not all of them.
  * A new arrival goes to the end of the line if others are already waiting.

  However, when both Puts and Gets are waiting, an arriving thread completes opportunistically:
  * If Gets are waiting and Puts are also waiting, a new Get takes an item immediately (a waiting Put is about to fill the Store anyway).
  * If Puts are waiting and Gets are also waiting, a new Put deposits immediately (a waiting Get is about to drain the Store anyway).

  This avoids an unnecessary context switch. In effect, the Channel switches from interrupt-style (wait for signal) to polling-style (proceed immediately) under load.

#### Channel Lifecycle

````
                         chanCreate
                             │
                             ▼
                    ┌─────────────────┐
                    │     Channel     │  openCnt = 0
                    └────────┬────────┘
                             │
         ┌───────────────────┴───────────────────┐
         │                                       │
         ▼                                       ▼
    Give away                               Share (retain reference)
    (pass as-is)                            (chanOpen before pass)
         │                                       │
         │                                       ▼
         │                               ┌───────────────┐
         │                               │  openCnt > 0  │
         │                               └───────┬───────┘
         │                                       │
         │                    ┌──────────────────┤
         │                    │                  │
         │               chanClose          chanClose
         │              (openCnt--)         (openCnt--)
         │                    │                  │
         │                    └──────┬───────────┘
         │                           │
         │                           │ when openCnt → 0
         │                           │
         ▼                           ▼
    ┌─────────────────────────────────────┐
    │  chanClose when openCnt == 0        │
    │            Deallocates              │
    └─────────────────────────────────────┘


            Orthogonal: chanShut
   ┌─────────────────────────────────────┐
   │  Before:  Put/Get block as needed   │
   │  After:   Put fails, Get nonblocking│
   └─────────────────────────────────────┘
````

Find the API in chan.h.

### Store

By default, a Channel can hold a single item. A Put into an empty Channel succeeds immediately; the putter only blocks when attempting a second Put before a Get drains the first. This minimizes latency when producer and consumer rates are balanced.

When rates differ, a larger buffer reduces context switching. The producer can run ahead without blocking on every item. Store callbacks execute within the Put/Get caller's context, providing buffer semantics without a buffer thread (see [CSP Heritage](#csp-heritage)).

```
  Default (single item):        With FIFO Store:

  Put ──► [1] ◄── Get           Put ──► [1][2][3]... ◄── Get
           │                            └──────────┘
     (second Put blocks              (Puts succeed until
      until Get drains)                 Store is full)
```

A bounded Store is what makes **backpressure** work, and backpressure is how Channel-based agents replace traditional rate-matching machinery. When a Store fills, putters block; that block propagates back through the agent topology, slowing upstream producers without explicit flow control, polling, or shared flags. The preallocated maximum size of these Store implementations is what fixes the blocking point — without it, an unbounded queue would absorb pressure that should reach producers.

A Store can be provided on a chanCreate call. The trade-off is latency versus throughput: larger Stores reduce context switching but delay the moment backpressure reaches producers.
(See [queueing theory](https://en.wikipedia.org/wiki/Queueing_theory).)

A maximum sized Channel FIFO Store preserves item order — first put, first got. Use it when items represent a stream that must stay sequenced. (See [pipeproxy](#Examples).)

Find the API in Str/chanStrFIFO.h.

A maximum sized, latency sensitive, Channel FIFO Store (FLSO) implementation is provided.
When a context is created, a maximum and initial size are specified; the Store starts at initial size.

**Problem**: Fixed-size buffers force a choice between low latency (small buffer) and high throughput (large buffer). Under variable load, neither choice is optimal. Small buffers can thrash under load. Large buffers can add latency.

**Solution**: Dynamically adjust buffer size based on observed pressure.

**Inspiration**: Consider a sushi cooler in a supermarket. Sushi must be fresh (low latency). The cooler observes producers (staff available to make sushi) and consumers (customers in line for sushi) and adjusts the amount of sushi it can hold. FLSO applies this principle to Store sizing.

The adjustment rules, executed within Put/Get operations:
* Before a Put, if the Store is empty and there are no waiting Gets, the size is decremented.
* After a Put, if the Store is full and there are waiting Gets, the size is incremented.
* Before a Get, if the Store is full and there are waiting Puts, the size is incremented.
* After a Get, if the Store is empty and there are no waiting Puts, the size is decremented.

This achieves self-tuning latency/throughput balance without external configuration or monitoring.

Find the API in Str/chanStrFLSO.h.

A maximum sized Channel LIFO Store — a stack — is also provided. There is no production use case for it: stack ordering rarely matches what an agent topology wants. It exists to make the framing concrete. A Channel/Store is not a send/receive pipe; it is a Store that pthreads put into and get from, and the ordering policy is the Store's choice. LIFO is a valid Store; stack ordering is a valid policy; the put/get model accommodates it without strain.

Find the API in Str/chanStrLIFO.h.

### Agent Discipline

The library provides primitives; the discipline of using them is where the leverage comes from. An agent is a thread that operates on Channels and nothing else. Get from zero or more Channels, do work, Put to zero or more Channels. It does NOT know where its get items originate, where its put items go, how deep any Store is, or how it fits in the program's topology. The launcher wires agents together with Channels, and the wiring IS the program; multiple paths become parallel execution.

This narrowness is what makes Channel-based code compose. A correct agent stays correct when wired into a new topology because it cannot see the topology.

#### Three-part anatomy

Every agent has three parts:

* **Context struct** — holds the Channels the agent operates on, plus configuration. Set by the launcher once, before `pthread_create`.
* **Thread function** — the main loop. On any chanOp returning anything other than success, chanShut and chanClose every Channel the agent holds, free the context, return.
* **Launcher function** — allocates the context, chanOpens every Channel the agent will use, creates the thread, detaches it. Never joins.

See `conS` / `multS` / `addS` in [squint](#Examples). Reading any one shows the pattern.

#### Shutdown is a cascade

There is no quit message, no done Channel, no `pthread_cancel`, no `pthread_join`. Shutdown propagates through chanShut and reference counting:

1. Some agent decides a part of the topology is finished and chanShuts a Channel.
2. The next chanOp on that Channel — by any agent — returns `chanOsSht`.
3. That agent breaks its main loop, runs its exit path (chanShut and chanClose every Channel it holds, free its context, return), and the thread terminates.
4. Each chanClose drops a reference. Upstream and downstream agents see their own Channels return `chanOsSht` in turn and cascade through the same exit.
5. The last chanClose on each Channel deallocates it.

squint's `printS` chanShuts after 12 coefficients. That single call tears down dozens of dynamically-spawned recursive agents. The cascade IS the protocol.

#### Anti-patterns

Reaching for any of these means the Channel model is not being used as intended:

* **Writing to another agent's state.** If agent A needs agent B's config or key material updated, A puts an update through a Channel; B gets it and updates its own local state. Channels are cheap. Direct memory writes — even "atomic" ones — are shared mutable state.
* **A "done" Channel for signalling termination.** The refcount cascade already handles this. A separate Channel means the refcounting is wrong.
* **Polling Channels.** chanOp in a loop to "drain" before every operation is shared-memory flag work with extra steps. Block in chanOp; let the Channel start and stop you.
* **Mutexes, condition variables, semaphores, atomic flags.** Channels encapsulate all synchronization. If a design requires explicit synchronization, the agent decomposition is wrong.

Find this discipline applied throughout `example/`. squint.c is the canonical case.

### Blob

Channel-based agents are confident inside a program. They become useful only when they integrate with software outside the program: another process via a pipe or socket pair, a network peer, a SQL database, a third-party library with its own I/O loop. Every one of those interfaces is byte-oriented. The `Blb/` subdirectory bridges Channels (which carry `void *`) to byte-oriented external software.

Integration is hard enough to need substantial library support: framing variable-length messages on a stream, owning operating-system resources across the lifetime of a bridge, propagating failure back into Channel state so agents see it, handling half-duplex and full-duplex shapes, packing fragments for lossy transports. `Blb/` is twice the size of `chan.[hc]` because integration is the size of the problem.

#### chanBlb — the bridge agent

A `chanBlb_t` is a length-prefixed octet buffer: the canonical shape for a Channel item destined for or arriving from an external byte interface. `chanBlb()` spawns a bridge — up to two threads — that moves `chanBlb_t *` items between a Channel and application-supplied byte-I/O callbacks. The shape is not obvious from the code:

````
                              chanBlb Instance
    ┌──────────────────────────────────────────────────────────────────┐
    │                                                                  │
    │     Egress Thread                         Ingress Thread         │
    │    ┌─────────────┐                       ┌─────────────┐         │
    │    │  chanOpGet  │                       │  chanOpPut  │         │
    │    │   egress    │                       │   ingress   │         │
    │    │   chan_t    │                       │   chan_t    │         │
    │    └──────┬──────┘                       └──────▲──────┘         │
    │           │                                     │                │
    │           ▼                                     │                │
    │    ┌─────────────┐                       ┌─────────────┐         │
    │    │     Chn     │  Optional Framer      │     Chn     │         │
    │    │   Framer    │  (Netstring, HTTP,    │   Framer    │         │
    │    │  Callback   │   FastCGI, etc.)      │  Callback   │         │
    │    └──────┬──────┘                       └──────┬──────┘         │
    │           │                                     │                │
    │           ▼                                     │                │
    │    ╔═════════════╗                       ╔═════════════╗         │
    │    ║     Trn     ║                       ║     Trn     ║         │
    │    ║  output()   ║                       ║   input()   ║         │
    │    ╚══════╤══════╝                       ╚══════╤══════╝         │
    │           │                                     │                │
    └───────────┼─────────────────────────────────────┼────────────────┘
                │                                     │
                │              Trn Context            │
                │   ┌───────────────────────────────┐ │
                │   │  Simple: FdStream             │ │
                │   │    write(fd)    read(fd)      │ │
                │   │                               │ │
                │   │  Complex: KCP                 │ │
                │   │  ┌─────────────────────────┐  │ │
                │   │  │   KCP Protocol State    │  │ │
                │   │  │  ┌───────────────────┐  │  │ │
                │   │  │  │  poll Thread      │  │  │ │
                │   │  │  │  UDP recv/send    │  │  │ │
                │   │  │  │  ikcp_update      │  │  │ │
                │   │  │  └───────────────────┘  │  │ │
                │   │  └─────────────────────────┘  │ │
                │   └───────────────────────────────┘ │
                │                                     │
                ▼           Transport Layer           │
           ═══════════════════════════════════════════════
                          (pipe, socket, etc.)
````

Two threads, not one and not four. One pthread can't simultaneously wait in `pthread_cond_wait` (Channel side) and `poll`/`select` (transport side), so each direction needs its own. Beyond those two, the framer (Chn) and transport (Trn) callbacks let that thread pair do wire framing and byte-I/O inline — no separate framer thread, no separate buffer-shuffler thread. That's the discipline that keeps integration cheap.

#### Chn — wire framing for streams

Stream transports don't preserve message boundaries; the bridge needs to know how to chop a byte stream into `chanBlb_t` items. A Chn framer fully replaces the thread body for its direction. Built-in framers cover [Variable-Length-Quantity](https://en.wikipedia.org/wiki/Variable-length_quantity) prefixing, [Netstring](https://en.wikipedia.org/wiki/Netstring), [FastCGI](https://en.wikipedia.org/wiki/FastCGI), [NETCONF](https://en.wikipedia.org/wiki/NETCONF) 1.0 and 1.1, [HTTP/1.x](https://en.wikipedia.org/wiki/Hypertext_Transfer_Protocol), and Reed-Solomon erasure coding over datagrams. Custom framers plug in through the same interface.

#### Trn — byte-I/O surfaces

Trn callbacks present a byte-oriented external resource to the bridge. Built-in implementations cover full-duplex stream fds (TCP sockets, stream socketpairs), half-duplex fds (pipes, bound datagram sockets), unbound datagram sockets with dual-stack IPv4/IPv6, and [KCP](https://github.com/skywind3000/kcp)-over-UDP. Application Trn implementations plug in through the same interface.

#### Reed-Solomon over datagrams (chanBlbChnRsec)

The integration case where "external" is a lossy datagram path (UDP under realistic loss). [Forward error correction](https://en.wikipedia.org/wiki/Reed-Solomon_error_correction) absorbs routine loss with zero retransmit round-trips: each message is split into k data shards and m parity shards, any k of which reconstruct the original. Most messages survive without retransmission; integration with UDP becomes practical at loss rates where TCP's head-of-line blocking would dominate latency.

#### chanBlbStrSQL — substrate integration

When "outside the program" is persistent storage rather than another process or a network, the integration shape is a Store with an external substrate. `chanBlbStrSQL` is an example Store whose substrate is SQLite — items survive process restart. The `chanBlb*` prefix marks Stores whose application contract assumes their items are `chanBlb_t`-shaped.

Parameter contracts, ownership discipline, and configuration details are in the headers (`Blb/chanBlb.h`, `Blb/chanBlbChn*.h`, `Blb/chanBlbTrn*.h`).

### Examples

* sockproxy
  * Modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanBlbs back-to-back, with Channels reversed.
  * Supports both stream sockets (using chanBlbTrnFdStream) and bound datagram sockets (using chanBlbTrnFd).
  * Sockproxy needs numeric values for socket type (-T, -t) and family type (-F, -f).
  * The options protocol type (-P, -p), service type (-S, -s) and host name (-H, -h) can be symbolic (see getaddrinfo).
  * Upper case options are for the "server" side, lower case options are for the "client" side.
  * For most BSD compatible socket libraries, SOCK_STREAM is 1, SOCK_DGRAM is 2, and AF_INET is 2.
  * For example, to listen (because of the server SOCK_STREAM socket type) for connections on any IPv4 stream socket on service 2222 and connect them to any IPv4 stream socket on service ssh at host localhost (letting the system choose the protocol):
    1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
    1. ssh -p 2222 user@localhost
* datagramchat and datagramchat-rsec
  * Broadcast chat demonstrating chanBlbTrnFdDatagram with unbound datagram sockets.
  * Each instance binds to a local port and broadcasts messages to configured peers.
  * Received messages display as [host:port]: message.
  * When stdin closes, sends a leave notification to all peers.
  * Example with three instances:
    1. ./datagramchat -l 5001 127.0.0.1:5002 127.0.0.1:5003
    1. ./datagramchat -l 5002 127.0.0.1:5001 127.0.0.1:5003
    1. ./datagramchat -l 5003 127.0.0.1:5001 127.0.0.1:5002
* pipeproxy
  * Copy stdin to stdout through chanBlb pipe file descriptors using a FIFO Store and preserving read boundaries using VLQ framing.
* chanBlbStrSQL
  * Example Store whose substrate is SQLite — items survive process restart.
* chanBlbTrnKcp
  * Demonstrate a [KCP](https://github.com/skywind3000/kcp) UDP transport integration.
* squint
  * Implementation of [M. Douglas McIlroy's "Squinting at Power Series"](https://swtch.com/~rsc/thread/squint.pdf).
* floydWarshall
  * Use a Channel coordinating dynamic thread pool to parallelize the [Floyd-Warshall](https://en.wikipedia.org/wiki/Floyd–Warshall_algorithm) all shortest paths algorithm using [blocking](https://github.com/moorejs/APSP-in-parallel) techniques extended to optionally save all equal next hops.

### Formal Verification with SPIN

The Channel implementation has been formally verified using [SPIN](http://spinroot.com/), a model checker for concurrent systems. SPIN exhaustively explores all possible interleavings of concurrent execution to verify correctness properties.

Promela models verify different aspects of the implementation:

| Model | Purpose | Key Properties Verified |
|-------|---------|------------------------|
| `chan.pml` | General Channel operations | Safety, conservation, deadlock freedom |
| `chanOne.pml` | Select-style first-available | First-match semantics, exactly-one completion |
| `chanAll.pml` | Atomic multi-Channel operations | All-or-nothing atomicity, lock ladder correctness |
| `chanStrFIFO.pml` | FIFO Store | FIFO ordering, conservation, capacity bounds |
| `chanStrFLSO.pml` | Self-tuning FIFO Store | FIFO ordering, size bounds, adaptation safety |
| `chanBlb.pml` | chanBlb bridge coordination | Shutdown ordering, finalClose timing, message conservation |

#### Running Verification

Requires SPIN (`spin` command) and a C compiler. Basic verification:

```bash
# Generate and compile verifier
spin -a chan.pml
cc -DSAFETY -o pan pan.c

# Run safety check
./pan -N atomicity

# For liveness properties (with fairness)
cc -o pan pan.c
./pan -a -f -N completion
```

Each model supports test scenario selection via preprocessor defines:

```bash
# chanOne.pml scenarios
spin -a chanOne.pml                    # default: first-match test
spin -DTEST_COMPETE -a chanOne.pml     # competing threads
spin -DTEST_LADDER -a chanOne.pml      # lock ladder deadlock test

# chanAll.pml scenarios
spin -a chanAll.pml                    # default: competing threads
spin -DTEST_PRODUCER_CONSUMER -a chanAll.pml
```

#### Verification Results

**chan.pml** - General operations (1,498 states with reduced parameters):

| Property | Result | Description |
|----------|--------|-------------|
| `safety` | PASSED | No race conditions or invalid states |
| `conservation` | PASSED | Items neither created nor destroyed |

**chanOne.pml** - Select-style operations (472-2,583 states):

| Property | Result | Description |
|----------|--------|-------------|
| `exactly_one` | PASSED | Each chanOne completes 0 or 1 operations |
| `first_match` | PASSED | Returns first completable operation in array order |
| `progress` | PASSED | Both threads complete (with fairness) |
| `ladder_progress` | PASSED | No deadlock from lock ladder |

**chanAll.pml** - Atomic multi-Channel operations (319-626 states):

| Property | Result | Description |
|----------|--------|-------------|
| `atomicity` | PASSED | All operations complete atomically or none do |
| `completion` | PASSED | Both threads eventually complete (with fairness) |
| `state_consistency` | PASSED | Channel states remain consistent |

**chanStrFIFO.pml** - Fixed-size FIFO Store (1,238 states):

| Property | Result | Description |
|----------|--------|-------------|
| `fifo_order` | PASSED | Items retrieved in order stored |
| `conservation` | PASSED | No items lost or duplicated |
| `bounds` | PASSED | Count never exceeds Store size |
| `progress` | PASSED | Producer/consumer complete (with fairness) |

**chanStrFLSO.pml** - Self-tuning FIFO Store (160,010 states):

| Property | Result | Description |
|----------|--------|-------------|
| `fifo_order` | PASSED | FIFO ordering preserved despite resizing |
| `size_bounds` | PASSED | Size stays within min/max bounds |
| `conservation` | PASSED | No items lost during resize |
| `adapts_safely` | PASSED | Resizing respects bounds |
| `progress` | PASSED | Producer/consumer complete (with fairness) |

**chanBlb.pml** - chanBlb bridge coordination (8,727 states):

| Property | Result | Description |
|----------|--------|-------------|
| `final_close` | PASSED | finalClose is eventually called |
| `channels_shut` | PASSED | Both Channels eventually shut |
| `threads_exit` | PASSED | Both threads eventually exit |
| `egress_conserve` | PASSED | No messages lost on egress path |
| `ingress_conserve` | PASSED | No messages lost on ingress path |
| `final_after_shut` | PASSED | finalClose only called after Channels shut |

The models verify that the lock ladder pattern (acquire locks in ascending order, retry on trylock failure) prevents deadlocks, that chanAll's all-or-nothing semantics hold under concurrent interference, that the Store implementations maintain FIFO ordering and data integrity, and that chanBlb properly coordinates shutdown and cleanup across egress/ingress threads.

#### Runtime Verification

The implementation passes runtime verification with sanitizers:

```bash
# ThreadSanitizer - detects data races
cc -fsanitize=thread -g -O1 -o squint example/squint.c chan.c -I. -lpthread
./squint 10   # No warnings

# AddressSanitizer - detects memory errors
cc -fsanitize=address -g -O1 -o squint example/squint.c chan.c -I. -lpthread
./squint 10   # No warnings

# Memory leak check (macOS)
leaks --atExit -- ./squint 10
```

#### Static Analysis

Clang static analyzer reports no true positives:

```bash
clang --analyze chan.c
```

The analyzer reports potential NULL dereference warnings in the `WAKE` macro. These are **false positives** because the analyzer cannot prove the invariant that waiter queue flags accurately reflect queue contents (when the "not empty" flag is set, a valid waiter pointer exists).

#### Implementation Invariants

The following invariants are maintained by the implementation:

| Invariant | Description | Enforcement |
|-----------|-------------|-------------|
| **Lock ladder** | Locks acquired in ascending Channel address order | `chanAll`, `chanOne` retry on trylock failure |
| **Atomic all-or-none** | `chanAll` completes all operations or none | Single atomic section after lock acquisition |
| **First-match** | `chanOne` returns first satisfiable operation | Sequential scan in array order |
| **Waiter fairness** | Waiters serviced in arrival order | FIFO waiter queues per operation type |
| **Queue/flag consistency** | Empty flags match queue state | Flag set when head==tail, cleared on enqueue |
| **Reference counting** | Channel deallocated when openCnt reaches 0 | `chanOpen`/`chanClose` balance |
| **Shutdown semantics** | After `chanShut`: Put fails, Get drains | `chanSu` flag checked before operations |
| **Store conservation** | Items neither created nor destroyed | Store callbacks maintain item count |
| **FIFO ordering** | Items retrieved in order stored | Circular buffer with head/tail pointers |

#### Informal Verification with squint

Testing parallel code is as hard as writing it, bugs hide in rare interleavings. To validate this implementation the [squint](#Examples) example implements [McIlroy's "Squinting at Power Series"](https://swtch.com/~rsc/thread/squint.pdf), a deliberately pathological parallel algorithm. It spawns threads dynamically, uses demand Channels to prevent thread explosion, and performs atomic multi-Channel operations throughout. Execution order is completely unpredictable, varying with core count, load, and scheduler decisions. Yet after all the chaos (threads creating, exiting, Channels opening, shutting, closing) the program must produce the correct coefficients of the tangent power series. It does. This is the proof that the lock ladder (acquire ascending, release descending) prevents deadlocks and that chanAll's all-or-nothing semantics hold under pressure.

### Building

Use "make" or review the file "Makefile" to build.
