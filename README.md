## pthreadChannel
Yet another implementation of a Communicating Sequential Process (CSP) "channel" construct for pthreads.

* Any number of threads can send and receive messages (a void *) on a "channel".
* By default, a "channel" queues a single message. For more, a user provided queue implementation is supported.
* Threads can "poll" for progress on multiple "channels".

This implementation's focus is fair access of channels, relaxed somewhat under pressure.

Find the API in chan.h:

* chanAlloc: allocate a chan_t
* chanShut: shutdown a chan_t
* chanFree: shutdown and deallocate a chan_t
* chanRecv: receive a message from a channel
* chanSend: send a message to a channel
* chanSendWait: send a message to a channel and return after it has been received
* chanPoll: do a channel operation on one of an array of channels

If provided, a "channel" invokes a queue implementation (while a mutex lock is held.)
The implementation can control queue latency, priority, etc.
A "channel fifo" queue implementation is provided.

Find the API in chanFifo.h:

* chanFifoQa: allocate a chanFifoQc (chanFifo context)
* chanFifoQd: deallocate a chanFifoQc (chanFifo context)
* chanFifoQi: chanFifo queue implementation

Since a thread can't both wait on a chanPoll() and a poll(), support for integrating bound sockets with channels is provided.

Find the API in chanSock.h:

* chanSock: link a bound full duplex socket to a pair of read and write channels

Use "make" to build.

Some examples:

* primes: Example of using chan.h and chanFifo.h is provided in test/primes.c. It is modeled on primes.c from [libtask](https://swtch.com/libtask/).
It is more complex because of pthread's API and to demonstrate various combinations of options.
* sockproxy: Example of using chan.h and chanSock.h is provided in test/sockproxy.c. It is modeled on tcpproxy.c from [libtask](https://swtch.com/libtask/).
Connects two chanSocks back to back, with read and write channels reversed.

Note: sockproxy needs numeric values for -T, -F, -t, and -f. For example SOCK_STREAM:1, AF_INET:2:

1. ./sockproxy -T 1 -F 2 -S 2222 -t 1 -f 2 -h localhost -s ssh &
2. ssh -p 2222 user@localhost
