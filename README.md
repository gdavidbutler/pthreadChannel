## pthreadChannel

Yet another implementation of a "Channel" construct for POSIX threads (pthreads).

### Channel

This library provides a [Channel](https://en.wikipedia.org/wiki/Channel_(programming)) style programming environment for C.
A Channel is an anonymous, pthread coordinating, Store of pointer (void *) sized items.

* Channels, by default, store a single item. (For more, see [store](#store).)
* Channels only support intra-process exchanges. (For inter-process, see [blob](#blob).)
* Any number of pthreads can Put/Get on a Channel.
  * Monitoring of Channel demand is supported. (See [squint](#example) for an example of [lazy evaluation](https://en.wikipedia.org/wiki/Lazy_evaluation).)
* A pthread can Put/Get on any number of Channels.
  * Unicast (One) and Multicast (All) operations are supported. (See [squint](#example).)
* The canonical Channel use is a transfer of a pointer to heap. (Delegating locking complexities to a heap management implementation e.g. realloc and free.) (See [primes](#example).)
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
* Channels distribute items fairly unless under pressure:
  * If there are waiting Gets, a new Get goes to the end of the line
    * unless there are also waiting Puts (as waiting Gets won't wait long)
      * then an item is opportunistically Get instead of waiting.
  * If there are waiting Puts, a new Put goes to the end of the line
    * unless there are also waiting Gets (as waiting Puts won't wait long)
      * then an item is opportunistically Put instead of waiting.

Find the API in chan.h:

* chanInit(...)
  * Each process must call this before using Channels.
* chanCreate(...)
  * Allocate an Open chan_t (initialize reference count at 1, pair with chanClose).
* chanOpen(...)
  * Open a chan_t (increment reference count, pair with chanClose).
* chanShut(...)
  * Shutdown a chan_t (afterwards chanOpPut fails and chanOpGet is always non-blocking).
* chanClose(...)
  * Close a chan_t (decrement reference count, deallocate at 0).
* chanOp(...)
  * perform an operation on a Channel
* chanOne(...)
  * Perform one operation (the first available) on an array of Channels.
* chanAll(...)
  * Perform all operations on an array of Channels. (See [atomic broadcast](https://en.wikipedia.org/wiki/Atomic_broadcast).)

### Store

In classic CSPs, a low latency synchronous rendezvous (Get and Put block till the other arrives to exchange an item)
works well when coded in machine or assembler language (using jump instructions).
If a Store is needed (queue, stack, etc.), it is implemented as another CSP.

Modern CSPs (e.g. "coroutines", "functions", etc.) with "local" variables and recursion support, have a high context switch cost.
But native support in modern processors (call/return instructions) makes it acceptable.
A Store is still implemented as another CSP.

However pthreads require operating system support and context switches are prohibitively expensive for simple Stores.
Therefore Stores are implemented as function callbacks executed within pthreads' contexts.

A Store can be provided on a chanCreate() call.
If none is provided, a Channel contains a single item.
This is best (lowest latency) when the cost of processing an item dominates the cost of a context switch.
But as the processing cost decreases toward the context switch cost, Stores can drastically decrease context switching.
Therefore, a Store's size depends on how much latency can be tolerated in the quest for efficiency.
(See [queueing theory](https://en.wikipedia.org/wiki/Queueing_theory).)

NOTE: These implementations preallocate heap with a maximum size to provide "back pressure" propagation semantics.

A maximum sized Channel FIFO Store implementation is provided.
When a context is created, a size is allocated.

A maximum sized, latency sensitive, Channel FIFO Store implementation is provided.
When a context is created, a maximum is allocated and starts at initial.
To balance latency and efficiency size is adjusted by:
* Before a Put, if the Store is empty and there are no waiting Gets, the size is decremented.
* After a Put, if the Store is full and there are waiting Gets, the size is incremented.
* After a Get, if the Store is empty and there are no waiting Puts, the size is decremented.
* Before a Get, if the Store is full and there are waiting Puts, the size is incremented.

A maximum sized Channel LIFO Store implementation is provided.
When a context is created, a size is allocated.

TODO: Priority Store

Find the API in chanStr.h:

* allocate a store context
  * chanFifoSa(...) FIFO
  * chanFlsoSa(...) FI-latency-sensitive-FO
  * chanLifoSa(...) LIFO
* deallocate a store context
  * chanFifoSd(...) FIFO
  * chanFlsoSd(...) FI-latency-sensitive-FO
  * chanLifoSd(...) LIFO
* implement a store (chanSi_t)
  * chanFifoSi(...) FIFO
  * chanFlsoSi(...) FI-latency-sensitive-FO
  * chanLifoSi(...) LIFO

### Blob

A Blob is a length specified collection of octets used as a discrete unit of communication, i.e. a message.

To support inter-process exchanges, blobs can be transported via message transfer mechanisms.
[Since a pthread can't both wait in a pthread_cond_wait() and in poll()/select()/etc., a pair of blocking reader and writer pthreads are used.]

For two common cases of sockets and pipes:

* socket: use shutdown() on inClose and outClose and close() on finClose
* pipe: use close() on inClose and outClose and no finClose

Several "framing" methods are supported:

* chanBlbNf
  * No framing. Writes are Blob size. Reads are, within a specified maximum, sized by the amount read.
* chanBlbNs
  * Read and write framed using [Netstring](https://en.wikipedia.org/wiki/Netstring).
* chanBlbN0
  * Read and write framed using [NETCONF](https://en.wikipedia.org/wiki/NETCONF)1.0.
* chanBlbN1
  * Read and write framed using [NETCONF](https://en.wikipedia.org/wiki/NETCONF)1.1.
* chanBlbH1
  * Read framed using [HTTP/1.x](https://en.wikipedia.org/wiki/Hypertext_Transfer_Protocol) on headers Transfer-Encoding (chunked) and Content-Length.
Blob flow (repeats):
    * Header Blob
    * If "Transfer-Encoding:chunked" header:
      * Non-zero chunk Blob (repeats)
      * Zero chunk Blob (includes trailer)
    * Else if "Content-Length:" header:
      * Non-zero content Blob

Find the API in chanBlb.h:

* chanBlb(...)
  * Blob exchange over ingress and egress Channels.

### Dependencies:

* chan.c:
  * chan.h
  * pthread.h
    * pthread_once()
    * pthread_key_create()
    * pthread_setspecific()
    * pthread_getspecific()
    * pthread_mutex_init()
    * pthread_mutex_lock()
    * pthread_mutex_trylock()
    * pthread_mutex_unlock()
    * pthread_mutex_destroy()
    * pthread_cond_init()
    * pthread_cond_wait()
    * pthread_cond_timedwait()
    * pthread_cond_signal()
    * pthread_cond_destroy()
    * pthread_condattr_init()
    * pthread_condattr_setclock()
    * pthread_condattr_destroy()
    * pthread_yield() - MacOS pthread_yield_np()
* chanStr.c:
  * chan.h
  * chanStr.h
* chanBlb.c:
  * chan.h
  * chanBlb.h
  * pthread.h
    * pthread_create()
    * pthread_detach()
    * pthread_cleanup_push()
    * pthread_cleanup_pop()

### Serialize

TODO: create examples

* [XML parser](https://github.com/gdavidbutler/xmlTrivialCallbackParser) and [XML dom](https://github.com/gdavidbutler/xmlTrivialDom)
* [JSON parser](https://github.com/gdavidbutler/jsonTrivialCallbackParser) and [JSON dom](https://github.com/gdavidbutler/jsonTrivialDom)

### Example

* primes
  * Modeled on primes.c from [libtask](https://swtch.com/libtask/).
(It is a bit more complex because of pthread's API and Channel's features.)
* sockproxy
  * Modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanBlb()s back-to-back, with Channels reversed.
  * Sockproxy needs numeric values for socket type (-T, -t) and family type (-F, -f).
  * The options protocol type (-P, -p), service type (-S, -s) and host name (-H, -h) can be symbolic (see getaddrinfo()).
  * Upper case options are for the "server" side, lower case options are for the "client" side.
  * For most BSD compatible socket libraries, SOCK_STREAM is 1 and AF_INET is 2.
  * For example, to listen (because of the server SOCK_STREAM socket type) for connections on any IPv4 stream socket on service 2222 and connect them to any IPv4 stream socket on service ssh at host localhost (letting the system choose the protocol):
    1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
    1. ssh -p 2222 user@localhost
* pipeproxy
  * Copy stdin to stdout through chanBlb() preserving read boundaries using Netstring framing
* squint
  * Implementation of [M. Douglas McIlroy's "Squinting at Power Series"](https://swtch.com/~rsc/thread/squint.pdf).

### Building

Use "make" or review the file "Makefile" to build.
