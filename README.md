## pthreadChannel

Yet another implementation of a "Channel" construct for POSIX threads (pthreads).

### Channel

A Channel is an anonymous, pthread coordinating, Store of pointer (void *) sized items.

For a background on Channels see Russ Cox's [Bell Labs and CSP Threads](https://swtch.com/~rsc/thread/).

* Channels, by default, store a single item. (For more, see [Store](#store), below.)
* Channels only support intra-process exchanges. (For inter-process, see [Blob](#blob), below.)
* Any number of pthreads can Put/Get on a Channel.
* A pthread can Put/Get on any number of Channels.
* Channel semantics can include ownership transfer (to avoid application level locking complexities).
NOTE: Items are discarded on last chanClose(). (To avoid leaking memory, chanGet() till chanOsSht).
  * putting pthread:
    ````C
    m = malloc(...);
    initialize(m);
    chanPut(chan, m);
    ````
  * getting pthread:
    ````C
    chanGet(chan, &m);
    use(m);
    free(m);
    ````
* Channels can be Put/Get on channels!
NOTE: chanOpen a chan_t before passing it (delegating chanClose) to eliminate chanClose/chanOpen races.
  * requesting pthread:
    ````C
    chanOpen(responseChan);
    chanPut(serviceChan, responseChan);
    response = chanGet(responseChan);
    ````
  * responding pthread:
    ````C
    responseChan = chanGet(serviceChan);
    chanPut(responseChan, response);
    chanClose(responseChan);
    ````

Channels distribute items fairly under pressure:
* If there are waiting getters, a new getter goes to the end of the line
  * unless there are also waiting putters (as waiting getters won't wait long)
    * then an item is opportunistically get instead of waiting.
* If there are waiting putters, a new putter goes to the end of the line
  * unless there are also waiting getters (as waiting putters won't wait long)
    * then an item is opportunistically put instead of waiting.

Find the API in chan.h:

* chanCreate(...)
  * Allocate an Open chan_t (initialize reference count to 1, pair with chanClose).
* chanOpen(...)
  * Open a chan_t (increment reference count, pair with chanClose). Should be called on each chan_t before passing to another pthread.
* chanShut(...)
  * Shutdown a chan_t (afterwards chanPut returns 0 and chanGet is non-blocking).
* chanClose(...)
  * Close a chan_t (decrement reference count, deallocate on 0). Should be called on each open chan_t upon pthread exit.
* chanPoll(...)
  * perform a Channel Operation (chanPo_t) on one of an array of Channels, working to satisfy them in the order provided.
* chanSht(...)
  * Shut an a Channel (asynchronously).
* chanGet(...)
  * Get an item from a Channel (asynchronously).
* chanPut(...)
  * Put an item to a Channel (asynchronously).
* chanPutWait(...)
  * Put an item to a Channel (synchronously, waiting for another pthread to chanGet).

### Store

In classic CSPs, a low latency synchronous rendezvous (get and put block till the other arrives to exchange an item)
works well when coded in machine or assembler language (jump instructions).
If a Store is needed (queue, stack, etc.), it is implemented as another CSP.

Modern CSPs (e.g. "coroutines", "functions", etc.) with "local" variables and recursion support, have a higher context switch cost.
But native support in modern processors (call/return instructions) makes it acceptable.
A Store is still implemented as another CSP.

However pthreads require operating system support and context switches are prohibitively expensive for simple Stores.
Therefore Stores are implemented as function callbacks executed within pthreads' contexts.

A Store can be provided on a chanCreate call.
If none is provided, a Channel contains a single item.
This is best (lowest latency) when the cost of processing an item dominates the cost of a context switch.
But as the processing cost decreases toward the context switch cost, Stores can drastically decrease context switching.
Therefore, a Store's size depends on how much latency can be tolerated in the quest for efficiency.

A statically sized Channel FIFO Store implementation is provided.
When a context is created, a size is allocated.

A dynamically sized Channel FIFO Store implementation is provided.
When a context is created, a maximum is allocated and starts at initial.
To balance latency and efficiency size is adjusted by:
* Before a Put, if the Store is empty and there are no waiting Getters, the size is decremented.
* After a Put, if the Store is full and there are waiting Getters, the size is incremented.
* After a Get, if the Store is empty and there are no waiting Putters, the size is decremented.
* Before a Get, if the Store is full and there are waiting Putters, the size is incremented.

Find the API in chanFifo.h:

* allocate a chanFifoSc (chanFifo Store context)
  * chanFifoStSa(...) static
  * chanFifoDySa(...) dynamic
* deallocate a chanFifoSc (chanFifo Store context)
  * chanFifoStSd(...) static
  * chanFifoDySd(...) dynamic
* implement a chanFifo
  * chanFifoStSi(...) static
  * chanFifoDySi(...) dynamic

### Blob

To support inter-process exchanges, blobs can be transported through sockets and pipes.
(Since a pthread can't both wait in a chanPoll() and in a poll()/select()/etc., the classic Unix fork() style (using pthreads) reader / writer technique is used.)

Two "framing" methods are supported:

* chanBlbNf
  * No framing. Read size is count from read() limited by the specified size. Write size is blob size.
* chanBlbNs
  * Read and write framed using [Netstring](https://en.wikipedia.org/wiki/Netstring).

Find the API in chanBlb.h:

* chanSock(...)
  * Blob exchange through a reliable, full duplex, connected socket over read and write Channels.
* chanPipe(...)
  * Blob exchange through reliable, half duplex, read and write pipes over read and write Channels.

### Serialize

TODO: create examples

* [JSON parser](https://github.com/gdavidbutler/jsonTrivialCallbackParser) and [JSON dom](https://github.com/gdavidbutler/jsonTrivialDom)
* [XML parser](https://github.com/gdavidbutler/xmlTrivialCallbackParser) and [XML dom](https://github.com/gdavidbutler/xmlTrivialDom)

### Example

* primes
  * Modeled on primes.c from [libtask](https://swtch.com/libtask/).
(It is a bit more complex because of pthread's API and Channel's features.)
* sockproxy
  * Modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanSocks back-to-back, with Channels reversed.
  * Sockproxy needs numeric values for socket type (-T, -t) and family type (-F, -f).
  * The options protocol type (-P, -p), service type (-S, -s) and host name (-H, -h) can be symbolic (see getaddrinfo()).
  * Upper case options are for the "server" side, lower case options are for the "client" side.
  * For most BSD compatible socket libraries, SOCK_STREAM is 1 and AF_INET is 2.
  * For example, to listen (because of the server SOCK_STREAM socket type) for connections on any IPv4 stream socket on service 2222 and connect them to any IPv4 stream socket on service ssh at host localhost (letting the system choose the protocol):
    1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
    1. ssh -p 2222 user@localhost
* pipeproxy
  * Copy stdin to stdout through chanPipe preserving read boundaries using NetString framing
* powser
  * TODO: Implementation of [Squinting at Power Series](https://swtch.com/~rsc/thread/squint.pdf).

### Building

Use "make" or review the file "Makefile" to build.
