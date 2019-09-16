## pthreadChannel
Yet another implementation of a Communicating Sequential Process (CSP) "channel" construct for pthreads.

* Any number of threads can send and receive messages (a void *) on a "channel".
* "Channels" are asynchronous, queueing a single message by default. Messages can be sent synchronously when needed.
* Threads can "poll" for progress on multiple "channels".

This implementation's focus is fair access of channels, relaxed somewhat under pressure.

Find the API in chan.h:

* chanCreate: allocate an Open a chan_t (reference count = 1) pair with chanClose
* chanOpen: Open a chan_t (increment a reference count) pair with chanClose
* chanShut: shutdown a chan_t (afterwards Send returns 0 and Recv is non-blocking)
* chanClose: close a chan_t, (decrement a reference count) deallocate on last Close
* chanRecv: receive a message from a channel
* chanSend: send a message to a channel
* chanSendWait: send a synchronous message to a channel (return after it has been received)
* chanPoll: perform a channel operation on one of an array of channels

A low latency single message channel works well in classic CSP implementations, coded in machine or assembler code (jumping instead of context switching).
Therefore, if a queue is required, it is coded as another CSP.
However modern processors provide native support for context frames (supporting local variables and recursive invocation).
As a result, even "light weight" process contexts must be switched (e.g. setjmp()/longjmp(), makecontext()/swapcontext(), etc.)
POSIX threads have an even greater context switch cost.
Therefore, queues should not be implemented in a separeate CSP (thread).
A solution is to implement queues as shared code executed within contexts of threads passing messages on channels.
And programmablity is key for latency management.

If provided, a "channel" invokes a queue implementation (while a mutex lock is held.)
The implementation can control queue latency, priority, etc.
A "channel fifo" queue implementation is provided.

Find the API in chanFifo.h:

* chanFifoQa: allocate a chanFifoQc (chanFifo context)
* chanFifoQd: deallocate a chanFifoQc (chanFifo context)
* chanFifoQi: chanFifo queue implementation

Since a thread can't both wait in a chanPoll() and in a poll()/select()/etc., support for integrating bound sockets with channels is provided.

Find the API in chanSock.h:

* chanSock: link a bound full duplex socket to a pair of read and write channels

Use "make" to build.

Some examples:

* primes: Example of using chan.h and chanFifo.h is provided in example/primes.c. It is modeled on primes.c from [libtask](https://swtch.com/libtask/).
It is more complex because of pthread's API and the various combinations of options.
* sockproxy: Example of using chan.h and chanSock.h is provided in example/sockproxy.c. It is modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanSocks back-to-back, with read and write channels reversed.

Note: sockproxy needs numeric values for -T, -F, -t, and -f. For example SOCK_STREAM:1, AF_INET:2:

1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
2. ssh -p 2222 user@localhost
