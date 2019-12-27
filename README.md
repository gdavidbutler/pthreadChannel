## pthreadChannel

Yet another implementation of a "Channel" construct for POSIX threads (pthreads).

### Channel

A Channel implements a pluggable store of, pointer (void *) sized, messages.

For a background on Channels see Russ Cox's [Bell Labs and CSP Threads](https://swtch.com/~rsc/thread/).

* Channels, by default, store a single message. (See Store, below.)
* Any number of pthreads can Put/Get on a Channel.
* A pthread can Put/Get on any number of Channels.
* Two pthreads can use a Channel in bi-directional (half-duplex) mode (alternating PutWait/Get calls).
* Message semantics should include ownership delegation. E.g.:
  * putting pthread: m = malloc(), init(m), chanPut(chan, m).
  * getting pthread: chanGet(chan, &m), use(m), free(m).
* Channels can be Put/Get on channels!
IMPORANT: chanOpen a chan_t before passing it (delegating chanClose) to eliminate chanClose/chanOpen races. E.g.:
  * requesting pthread: chanOpen(responseChan), chanPut(theirChan, responseChan), response = chanGet(responseChan).
  * responding pthread: responseChan = chanGet(myChan), chanPut(responseChan, response), chanClose(responseChan).

Channels distribute messages fairly under pressure:
* If there are waiting getters, a new getter goes to the end of the line
  * unless there are also waiting putters (as waiting getters won't wait long)
    * then a meesage is opportunistically get instead of waiting.
* If there are waiting putters, a new putters goes to the end of the line
  * unless there are also waiting getters (as waiting putters won't wait long)
    * then a meesage is opportunistically put instead of waiting.

Find the API in chan.h:

* chanCreate
  * Allocate an Open chan_t (reference count = 1, pair with chanClose).
* chanOpen
  * Open a chan_t (increment reference count, pair with chanClose). Should be called on each chan_t before passing to another pthread.
* chanShut
  * Shutdown a chan_t (afterwards chanPut returns 0 and chanGet is non-blocking).
* chanIsShut
  * Is a chan_t shutdown.
* chanClose
  * Close a chan_t (decrement reference count, deallocate on last chanClose). Should be called on each chan_t upon pthread exit.
* chanGet
  * Get a message from a Channel (asynchronously).
* chanPut
  * Put a message to a Channel (asynchronously).
* chanPutWait
  * Put a message to a Channel (synchronously, waiting for a chanGet).
* chanPoll
  * perform a Channel Operation (chanPo_t) on one of an array of Channels, working to satisfy them in the order provided.

### Store

A Channel's store implementation is pluggable.

In classic CSPs, a low latency synchronous rendezvous (get and put block till the other arrives to exchange a message)
works well when coded in machine or assembler language (jump instructions).
If a message store is needed (queue, stack, etc.), it is implemented as another CSP.

Modern CSPs (e.g. "coroutines", "functions", etc.) with "local" variables and supporting recursion, have a higher context switch cost.
But native support in modern processors (call/return instructions) makes it acceptable.
A message store is still implemented as a CSP.

However pthreads require operating system support and context switches are prohibitively expensive for simple message stores.
Therefore Stores are implemented as shared code executed within pthreads' contexts.

A pluggable Store can be provided on a chanCreate call.
If none is provided, a channel stores a single message.
This is best (lowest latency) when the cost of processing a message dominates the cost of a context switch.
But as the processing cost decreases toward the context switch cost, Stores can drastically decrease context switching.
Therefore, a Store's size depends on how much latency can be tolerated in the quest for efficiency.

A dynamic Channel FIFO store implementation is provided.
When a context is created, a maximum size and an initial size are provided.
To balance latency and efficiency the size is adjusted by:
* Before a Put, if the Store is empty and there are no waiting Getters, the size is decremented.
* After a Put, if the Store is full and there are waiting Getters, the size is incremented.

Find the API in chanFifo.h:

* chanFifoSa
  * allocate a chanFifoSc (chanFifo store context)
* chanFifoSd
  * deallocate a chanFifoSc (chanFifo store context)
* chanFifoSi
  * chanFifo store implementation

### Poll/Select

Since a pthread can't both wait in a chanPoll() and in a poll()/select()/etc., support for integrating bound sockets with Channels is provided.

Find the API in chanSock.h:

* chanSock: connect a bound full duplex socket (using read() and write()) to a pair of read and write Channels

### Examples

* primes
  * Example of using chan.h and chanFifo.h is provided in example/primes.c. It is modeled on primes.c from [libtask](https://swtch.com/libtask/).
It is more complex because of pthread's API and various combinations of options.
* powser
  * [TO DO] implementation of [Squinting at Power Series](https://swtch.com/~rsc/thread/squint.pdf).
* sockproxy
  * Example of using chan.h and chanSock.h is provided in example/sockproxy.c. It is modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanSocks back-to-back, with Channels reversed.
  * Sockproxy needs numeric values for socket type (-T, -t) and family type (-F, -f).
  * The options protocol type (-P, -p), service type (-S, -s) and host name (-H, -h) can be symbolic (see getaddrinfo()).
  * Upper case options are for the "server" side, lower case options are for the "client" side.
  * For most BSD compatible socket libraries, SOCK_STREAM is 1 and AF_INET is 2.
  * For example, to listen (because of the server SOCK_STREAM socket type) for connections on any IPv4 stream socket on service 2222 and connect them to any IPv4 stream socket on service ssh at host localhost (letting the system choose the protocol):
    1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
    1. ssh -p 2222 user@localhost

### Building

Use "make" or review the file "Makefile" to build.
