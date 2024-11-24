## pthreadChannel

Yet another implementation of a "Channel" construct for POSIX threads (pthreads).

### Channel

This library provides a [Channel](https://en.wikipedia.org/wiki/Channel_(programming)) style programming environment in C.
A Channel is an anonymous, pthread coordinating, [Store](#Store) of pointer (void *) sized items.

* Channels facilitate pthread based [SMP](https://en.wikipedia.org/wiki/Symmetric_multiprocessing).
(For [messaging passing](https://en.wikipedia.org/wiki/Message_passing) see [Blob](#Blob).)
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
NOTE: chanOpen a chan_t before passing it (delegating chanClose) to eliminate a deallocating chanClose/chanOpen race.
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
* Pthreads are serviced fairly unless under pressure:
  * If there are waiting Gets, a new Get goes to the end of the line
    * unless there are also waiting Puts (as waiting Gets won't wait long, i.e. don't force a context switch)
      * then an item is opportunistically Get instead of waiting.
  * If there are waiting Puts, a new Put goes to the end of the line
    * unless there are also waiting Gets (as waiting Puts won't wait long, i.e. don't force a context switch)
      * then an item is opportunistically Put instead of waiting.

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

A Channel Store is implemented with callbacks executed within pthreads' contexts.

With classic [CSP](https://en.wikipedia.org/wiki/Communicating_sequential_processes)s,
a rendezvous (Get and Put block till the other arrives to exchange an item)
works well when implemented in machine or assembler language using jump instructions.
If a Store is needed (queue, stack, etc.), it is implemented as another CSP.
But modern CSPs (e.g. "coroutines", "functions", etc.), with local variables and recursion support, have a high context switch cost.
Fortunately, support in modern processors (call/return instructions, etc.) makes the cost acceptable.
However a pthread context switch requires operating system support and context switch cost is prohibitively high for simple Stores.
Therefore, Channel Stores are implemented as function callbacks.

A Store can be provided on a chanCreate call.
If none is provided, a Channel can store a single item.
This is best (lowest latency) when the cost of processing an item dominates the cost of a context switch.
But as the processing cost decreases toward a context switch cost, Stores can drastically decrease context switching.
Therefore, a Store's size depends on how much latency can be tolerated in the quest for efficiency.
(See [queueing theory](https://en.wikipedia.org/wiki/Queueing_theory).)

A Store's initial chanSs_t state is determined at allocation and provided to chanCreate.
Once handed to chanCreate, a Store's chanSs_t state must not change between calls to it's chanSi_t callback.
(See [chanStrBlbSQL](#Examples)).

NOTE: These implementations preallocate heap with a maximum size to provide "back pressure" propagation semantics.

A maximum sized Channel FIFO Store implementation is provided.
When a context is created, a size is allocated.
(See [pipeproxy](#Examples) for an example.)

Find the API in chanStrFIFO.h:

* chanStrFIFOa
  * Allocate a chanStrFIFOc_t of size using supplied realloc, free and item dequeue routines
* chanStrFIFOd
  * Implement a chanSd_t to deallocate a chanStrFIFOc_t
* chanStrFIFOi
  * Implement chanSi_t for chanCreate()

A maximum sized, latency sensitive, Channel FIFO Store (FLSO) implementation is provided.
When a context is created, a maximum is allocated and starts at initial.
To balance latency and efficiency size is adjusted by:
* Before a Put, if the Store is empty and there are no waiting Gets, the size is decremented.
* After a Put, if the Store is full and there are waiting Gets, the size is incremented.
* After a Get, if the Store is empty and there are no waiting Puts, the size is decremented.
* Before a Get, if the Store is full and there are waiting Puts, the size is incremented.

Find the API in chanStrFLSO.h:

* chanStrFLSOa
  * Allocate a chanStrFLSOc_t of max and initial size using supplied realloc, free and item dequeue routines
* chanStrFLSOd
  * Implement a chanSd_t to deallocate a chanStrFLSOc_t
* chanStrFLSOi
  * Implement chanSi_t for chanCreate()

A maximum sized Channel LIFO Store implementation is provided.
When a context is created, a size is allocated.

Find the API in chanStrLIFO.h:

* chanStrLIFOa
  * Allocate a chanStrLIFOc_t of size using supplied realloc, free and item dequeue routines
* chanStrLIFOd
  * Implement a chanSd_t to deallocate a chanStrLIFOc_t
* chanStrLIFOi
  * Implement chanSi_t for chanCreate()

### Blob

A Blob is a length specified collection of octets used as a discrete unit of communication, i.e. a message.

Blobs can be tranported via networking routines.
[Since a pthread can't both wait in a pthread_cond_wait and a poll/select/etc., a pair of blocking reader and writer pthreads are used.]

When applying the API to socket and pipe like intefaces:

* socket: use shutdown on inClose and outClose and close on finClose
* pipe: use close on inClose and outClose and no finClose

Several "framing" methods are provided (useful with streaming protocols):

* chanBlbNf
  * No framing. Writes are Blob size. Reads are, within a specified maximum, sized by the amount read. (Most useful with datagram protocols.)
* chanBlbNs
  * Read and write framed using [Netstring](https://en.wikipedia.org/wiki/Netstring).
* chanBlbN0
  * Read and write framed using [NETCONF](https://en.wikipedia.org/wiki/NETCONF)1.0.
(This flawed, XML specific, framer is only useful for NETCONF before a transition to NETCONF 1.1.)
* chanBlbN1
  * Read and write framed using [NETCONF](https://en.wikipedia.org/wiki/NETCONF)1.1.
* chanBlbH1
  * Read framed using [HTTP/1.x](https://en.wikipedia.org/wiki/Hypertext_Transfer_Protocol) on headers Transfer-Encoding (chunked) and Content-Length.
Blob flow (repeats):
    * Header Blob
    * If "Transfer-Encoding" header with value "chunked":
      * Non-zero chunk Blob (repeats)
      * Zero chunk Blob (includes trailer)
    * Else if "Content-Length" header:
      * Non-zero content Blob
  * Write is not framed.

Find the API in chanBlb.h:

* chanBlb
  * Blob transport via ingress and egress Channels.

### Channel routine dependencies

* chan.c:
  * chan.h
  * pthread.h
    * pthread_once
    * pthread_key_create
    * pthread_setspecific
    * pthread_getspecific
    * pthread_mutex_init
    * pthread_mutex_lock
    * pthread_mutex_trylock
    * pthread_mutex_unlock
    * pthread_mutex_destroy
    * pthread_cond_init
    * pthread_cond_wait
    * pthread_cond_timedwait
    * pthread_cond_signal
    * pthread_cond_destroy
    * pthread_condattr_init
    * pthread_condattr_setclock
    * pthread_condattr_destroy
    * pthread_yield
* chanStr.c:
  * chan.h
  * chanStr.h
* chanBlb.c:
  * chan.h
  * chanBlb.h
  * pthread.h
    * pthread_create
    * pthread_detach
    * pthread_cleanup_push
    * pthread_cleanup_pop

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
  * Copy stdin to stdout through chanBlb preserving read boundaries using a FIFO Store and Netstring framing
* chanStrBlbSQL
  * Demonstrate a SQLite based Channel Blob FIFO Store
* squint
  * Implementation of [M. Douglas McIlroy's "Squinting at Power Series"](https://swtch.com/~rsc/thread/squint.pdf).
* floydWarshall
  * Use a Channel coordinating dynamic thread pool to parallelize the [Floyd-Warshall](https://en.wikipedia.org/wiki/Floyd–Warshall_algorithm) all shortest paths algorithm using [blocking](https://github.com/moorejs/APSP-in-parallel) techniques extended to optionally save all equal next hops.

### Building

Use "make" or review the file "Makefile" to build.
