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

Channels offer a different model: instead of protecting shared data with locks, **transfer ownership of data through channels**. A Put hands off data; a Get receives it. The synchronization is encapsulated within the channel operations themselves. User code contains no mutexes, no condition variables and no barriers.

This library provides:
* **chanOp** - blocking Put or Get on a single channel
* **chanOne** - wait for the first available operation across multiple channels (like select)
* **chanAll** - perform all operations atomically across multiple channels or none

These primitives compose safely. Two correct channel-based components, when connected, remain correct. This is not true of lock-based code, where combining two independently correct modules can introduce deadlocks neither had alone.

The cost is discipline: data must flow through channels rather than be shared. But this discipline is enforced by the API, not by programmer vigilance. The result is parallel code that humans can reason about, review, and maintain.

#### Validation

Testing parallel code is as hard as writing it, bugs hide in rare interleavings. To validate this implementation the [squint](#Examples) example implements [McIlroy's "Squinting at Power Series"](https://swtch.com/~rsc/thread/squint.pdf), a deliberately pathological parallel algorithm. It spawns threads dynamically, uses demand channels to prevent thread explosion, and performs atomic multi-channel operations throughout. Execution order is completely unpredictable, varying with core count, load, and scheduler decisions. Yet after all the chaos (threads creating, exiting, channels opening, shutting, closing) the program must produce the correct coefficients of the tangent power series. It does. This is the proof that the lock ladder (acquire ascending, release descending) prevents deadlocks and that chanAll's all-or-nothing semantics hold under pressure.

#### CSP Heritage

This library draws from [Communicating Sequential Processes](https://en.wikipedia.org/wiki/Communicating_sequential_processes) (CSP), where concurrent processes communicate through channels rather than sharing memory. In classic CSP, implemented in machine or assembler language using jump instructions, a rendezvous (Get and Put block until the other arrives) works well. When buffering is needed (queue, stack, etc.), it is implemented as another CSP.

But modern CSPs (coroutines, functions with local variables and recursion) have higher context switch costs. Modern processors (call/return instructions, etc.) make this acceptable. However, a pthread context switch requires operating system support and the cost is prohibitively high for simple operations like buffering.

This implementation therefore uses **callbacks instead of processes** where a trivial thread would otherwise be required:
* [Store](#Store) callbacks provide buffering without a buffer thread
* [Blob](#Blob) Trn/Chn callbacks bridge channels to I/O without blocking reader/writer threads

The channel semantics remain, but the implementation avoids threads that would do nothing but shuffle data.

### Channel

This library provides a [Channel](https://en.wikipedia.org/wiki/Channel_(programming)) style programming environment in C.
A Channel is an anonymous, pthread coordinating, [Store](#Store) of pointer (void *) sized items.

* Channels facilitate pthread based [SMP](https://en.wikipedia.org/wiki/Symmetric_multiprocessing).
(For [message passing](https://en.wikipedia.org/wiki/Message_passing) see [Blob](#Blob).)
* Channels, by default, can store a single item.
(For more, see [Store](#Store).)
* Any number of pthreads can Put/Get on a Channel.
  * Awareness of Channel demand is supported.
(See an example of [lazy evaluation](https://en.wikipedia.org/wiki/Lazy_evaluation) in [squint](#Examples).)
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
* Channels can be Put/Get on Channels!
This enables RPC-style request/response patterns:

````
     Requester                                      Responder
         │                                              │
         │  1. chanOpen(responseChan)                   │
         │     (increment ref count before sending)     │
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

NOTE: chanOpen the response chan_t before passing it, otherwise the responder's chanClose could deallocate the channel before the requester's Get completes.
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

  This avoids an unnecessary context switch. In effect, the channel switches from interrupt-style (wait for signal) to polling-style (proceed immediately) under load.

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

Find the API in chan.h:

* chanInit
  * A process must supply a thread safe dynamic memory interface (e.g. realloc and free) before using Channels.
* chanCreate
  * Allocate a chan_t, initializing chanOpenCnt to zero. (pair with chanClose).
* chanOpen
  * Open a chan_t, incrementing chanOpenCnt. (pair with chanClose).
* chanShut
  * Shutdown a chan_t (afterwards chanOpPut fails and chanOpGet is always non-blocking).
* chanClose
  * Close a chan_t, decrementing chanOpenCnt. (deallocates at zero chanOpenCnt).
* chanOpenCnt
  * Return current number of chanOpen not yet chanClose.
* chanOp
  * Perform an operation on a Channel
* chanOne
  * Perform one operation (the first available, in array order) on an array of Channels.
* chanAll
  * Perform all (or no) operations on an array of Channels. (See [squint](#Examples).)

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

A Store can be provided on a chanCreate call. The trade-off is latency versus throughput: larger Stores reduce context switching but increase the delay before backpressure propagates to producers.
(See [queueing theory](https://en.wikipedia.org/wiki/Queueing_theory).)

NOTE: These implementations preallocate heap with a maximum size to provide backpressure propagation semantics.

A maximum sized Channel FIFO Store implementation is provided.
When a context is created, a size is allocated.
(See [pipeproxy](#Examples).)

Find the API in Str/chanStrFIFO.h.

A maximum sized, latency sensitive, Channel FIFO Store (FLSO) implementation is provided.
When a context is created, a maximum and initial size are specified; the Store starts at initial size.

**Problem**: Fixed-size buffers force a choice between low latency (small buffer) and high throughput (large buffer). Under variable load, neither choice is optimal. Small buffers can thrash under load. Large buffers can add latency.

**Solution**: Dynamically adjust buffer size based on observed pressure.

**Inspiration**: Consider a sushi cooler in a supermarket. Sushi must be fresh (low latency). The cooler observes producers (staff available to make sushi) and consumers (customers in line for sushi) and adjusts the amount of sushi it can hold. FLSO applies this principle to store sizing.

The adjustment rules, executed within Put/Get operations:
* Before a Put, if the Store is empty and there are no waiting Gets, the size is decremented.
* After a Put, if the Store is full and there are waiting Gets, the size is incremented.
* Before a Get, if the Store is full and there are waiting Puts, the size is incremented.
* After a Get, if the Store is empty and there are no waiting Puts, the size is decremented.

This achieves self-tuning latency/throughput balance without external configuration or monitoring.

Find the API in Str/chanStrFLSO.h.

A maximum sized Channel LIFO Store implementation is provided.
When a context is created, a size is allocated.

Find the API in Str/chanStrLIFO.h.

### Blob

Channels pass pointers for intra-process SMP where threads share memory. But pointers are meaningless across process boundaries or over a network. For inter-process or inter-system communication, data must be transmitted.

A Blob is a length-prefixed array of octets: a message. Blob Stores hold the content itself, not a pointer, enabling:
* **Persistence**: Blobs can survive process restart (see [chanStrBlbSQL](#Examples), which stores blobs in SQLite)
* **Transport**: Blobs can be sent over network connections (see chanBlb below)

Since a pthread can't both wait in a pthread_cond_wait and a poll/select/etc., a pair of blocking reader and writer pthreads are used.
To limit the number of trivial threads, the API provides callback interfaces for both the Channel (Chn) and transport (Trn) sides.

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
                  (pipe, socket, UDP datagram, etc.)
````

* Chn callbacks replace the default Egress/Ingress thread logic to frame/deframe blobs.
* Trn callbacks handle byte I/O; the Trn context may encapsulate protocol state and additional threads.

* chanBlb
  * Blob transport via ingress and egress Channels.

Find the API in Blb/chanBlb.h.

A couple of transport side implementations are provided (file descriptor based interfaces):

* chanBlbTrnFd
  * File descriptor for non-stream interfaces (e.g. pipe, UDP socket), use close() on inClose() and outClose() and no finClose().

* chanBlbTrnFdStream
  * File descriptor for stream interfaces (e.g. TCP socket), use shutdown() on inClose() and outClose() and close() on finClose().

Several Channel side, "framing", implementations are provided (useful with streaming protocols):

* chanBlbChnFcgi
  * Read and write framed using [FastCGI](https://en.wikipedia.org/wiki/FastCGI).

* chanBlbChnNetstring
  * Read and write framed using [Netstring](https://en.wikipedia.org/wiki/Netstring).

* chanBlbChnNetconf10
  * Read and write framed using [NETCONF](https://en.wikipedia.org/wiki/NETCONF) 1.0.
(This flawed, XML specific, framer is only useful for NETCONF before a transition to NETCONF 1.1.)

* chanBlbChnNetconf11
  * Read and write framed using [NETCONF](https://en.wikipedia.org/wiki/NETCONF) 1.1.

* chanBlbChnHttp1
  * Read framed using [HTTP/1.x](https://en.wikipedia.org/wiki/Hypertext_Transfer_Protocol) on headers Transfer-Encoding (chunked) and Content-Length.
Blob flow (repeats):
    * Header Blob
    * If "Transfer-Encoding" header with value "chunked":
      * Non-zero chunk Blob (repeats)
      * Zero chunk Blob (includes trailer)
    * Else if "Content-Length" header:
      * Non-zero content Blob
  * Write is not framed.

### Examples

* sockproxy
  * Modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanBlbs back-to-back, with Channels reversed.
  * Sockproxy needs numeric values for socket type (-T, -t) and family type (-F, -f).
  * The options protocol type (-P, -p), service type (-S, -s) and host name (-H, -h) can be symbolic (see getaddrinfo).
  * Upper case options are for the "server" side, lower case options are for the "client" side.
  * For most BSD compatible socket libraries, SOCK_STREAM is 1 and AF_INET is 2.
  * For example, to listen (because of the server SOCK_STREAM socket type) for connections on any IPv4 stream socket on service 2222 and connect them to any IPv4 stream socket on service ssh at host localhost (letting the system choose the protocol):
    1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
    1. ssh -p 2222 user@localhost
* pipeproxy
  * Copy stdin to stdout through chanBlb pipe file descriptors preserving read boundaries using a FIFO Store and Netstring framing.
* chanStrBlbSQL
  * Demonstrate a SQLite based Channel Blob FIFO Store.
* chanBlbTrnKcp
  * Demonstrate a [KCP](https://github.com/skywind3000/kcp) UDP transport integration.
* squint
  * Implementation of [M. Douglas McIlroy's "Squinting at Power Series"](https://swtch.com/~rsc/thread/squint.pdf).
* floydWarshall
  * Use a Channel coordinating dynamic thread pool to parallelize the [Floyd-Warshall](https://en.wikipedia.org/wiki/Floyd–Warshall_algorithm) all shortest paths algorithm using [blocking](https://github.com/moorejs/APSP-in-parallel) techniques extended to optionally save all equal next hops.

### Building

Use "make" or review the file "Makefile" to build.
