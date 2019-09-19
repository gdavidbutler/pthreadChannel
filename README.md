## pthreadChannel
Yet another implementation of a Communicating Sequential Process (CSP) Channel construct for pthreads.

A Channel implements a configurable store of anonymous, pointer sized, messages.
Threads can (optionally block to) push a message in to or pull a message out of a Channel.
When blocking, access to pushing or pulling is granted in a first-come-first-serve basis.

* A Channel holds a single message, by default. (See below.)
* Messages can be pushed waiting on a pull, when needed.
* Any number of threads can push to and pull from a Channel.
* A thread can attempt to push to or pull from many Channels till one can proceed.

This implementation's focus is fair access to messages, relaxed somewhat under pressure.

Find the API in chan.h:

* chanCreate: Allocate an open chan_t (reference count = 1) pair with a chanClose
* chanOpen: Open a chan_t (increment a reference count) pair with a chanClose
* chanShut: Shutdown a chan_t (afterwards Push returns 0 and Pull is non-blocking)
* chanIsShut: Is a chan_t shutdown (a 0 return from a blocking chanPoll usually indicates a chan_t is Shut)
* chanClose: Close a chan_t, (decrement a reference count) deallocate on last Close
* chanPull: Pull a message from a Channel
* chanPush: Push a message to a Channel
* chanPushWait: Push a message to a Channel, waiting till it has been pulled
* chanPoll: perform a Channel operation on one of an array of Channels, working to satisfy them in the order provided.

A Channel's store implementation is configurable.
In classic CSP, a low latency synchronous rendezvous works well when coded in machine or assembler code (jumping instead of context switching).
If a store is needed, it is implemented in another process.
However, modern processors provide native support for context frames (supporting local variables and recursive invocation).
Resulting in "light weight" process context switching (e.g. setjmp()/longjmp(), makecontext()/swapcontext(), etc.)
POSIX threads have an even greater context (process) switch cost.
Therefore, simple stores should not be implemented in separate threads.
A solution is to implement stores as shared code executed within contexts of threads.

If provided, a Channel invokes a store implementation (while a mutex lock is held.)
The implementation can control store latency, priority, etc.
A Channel FIFO store implementation is provided.

Find the API in chanFifo.h:

* chanFifoSa: allocate a chanFifoSc (chanFifo context)
* chanFifoSd: deallocate a chanFifoSc (chanFifo context)
* chanFifoSi: chanFifo store implementation

Since a thread can't both wait in a chanPoll() and in a poll()/select()/etc., support for integrating bound sockets with Channels is provided.

Find the API in chanSock.h:

* chanSock: link a bound full duplex socket to a pair of Channels

Use "make" to build.

Some examples:

* primes: Example of using chan.h and chanFifo.h is provided in example/primes.c. It is modeled on primes.c from [libtask](https://swtch.com/libtask/).
It is more complex because of pthread's API and various combinations of options.
* sockproxy: Example of using chan.h and chanSock.h is provided in example/sockproxy.c. It is modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanSocks back-to-back, with Channels reversed.

Note: sockproxy needs numeric values for -T, -F, -t, and -f. For example SOCK_STREAM:1, AF_INET:2:

1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
2. ssh -p 2222 user@localhost
