## pthreadChannel

Yet another implementation of a "Channel" construct for POSIX threads (pthreads).

### Channel

A Channel implements a programmable store of anonymous, pointer (void *) sized, messages.

For a background on Channels see Russ Cox's [Bell Labs and CSP Threads](https://swtch.com/~rsc/thread/).

* Channels store a single message, by default. (See Store, below.)
* Any number of pthreads can Put/Get on a Channel.
* A pthread can Put/Get on any number of Channels.
* Message semantics should include ownership transfer. E.g.:
  * putting thread: m = malloc(), init(m), chanPut(chan, m).
  * getting thread: chanGet(chan, &m), use(m), free(m).
* Channels can be Put/Get on channels! E.g.:
  * client thread: chanOpen(chan), chanPut(server, chan), response = chanGet(chan).
  * server thread: chan = chanGet(server), chanPut(chan, response), chanClose(chan).

This implementation's focus is store fair access (first-come-first-serve), relaxed somewhat under pressure.

Find the API in chan.h:

* chanInit
  * Initialize the chan API providing pointers to dynamic memory routines with realloc() and free() semantics. Otherwise, use the libc versions.
* chanCreate
  * Allocate an Open chan_t (reference count = 1, pair with chanClose).
* chanOpen
  * Open a chan_t (increment reference count, pair with chanClose). Should be called on each chan_t before passing to another thread.
* chanShut
  * Shutdown a chan_t (afterwards chanPut returns 0 and chanGet is non-blocking).
* chanIsShut
  * Is a chan_t shutdown.
* chanClose
  * Close a chan_t (decrement reference count, deallocate on last chanClose). Should be called on each chan_t upon thread exit.
* chanGet
  * Get a message from a Channel (asynchronously).
* chanPut
  * Put a message to a Channel (asynchronously).
* chanPutWait
  * Put a message to a Channel (synchronously, waiting for a chanGet).
* chanPoll
  * perform a Channel Operation (chanPo_t) on one of an array of Channels, working to satisfy them in the order provided.

### Store

A Channel's store implementation is programmable.

In classic CSPs, a low latency synchronous rendezvous (get and put block till the other arrives to exchange a message)
works well when coded in machine or assembler language (a jumping, from put to get, context switch).
Then, if a message store is desired (queue, stack, etc.), it is implemented as another CSP.

Modern CSPs (e.g. "coroutines", "functions", etc.), supporting "local" variables and recursion, have a much higher context switch overhead.
But native support by modern processors makes it acceptable.

However pthreads require operating system support and context switches are prohibitively expensive for simple message stores.
Therefore stores are implemented as shared code executed within pthreads' contexts.

If provided at chanCreate, a Channel can use a store implementation.
The implementation can control latency, priority, etc.
A Channel FIFO store implementation is provided.

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

* chanSock: connect a bound full duplex socket (using read() and write()) to a pair of Channels

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
