## pthreadChannel
Yet another implementation of a Communicating Sequential Process (CSP) "channel" construct for pthreads.

* Any number of threads can send and receive messages (a void *) on a "channel".
* By default, a "channel" queues a single message. For more, a user provided queue implementation is supported.
* Threads can "poll" for progress on multiple "channels".

This implementation's focus is fair access of channels, relaxed somewhat under pressure.

Find the API in chan.h:

* chanAlloc: allocate a chan_t
* chanDone: deallocate a chan_t
* chanRecv: receive a message from a channel
* chanSend: send a message to a channel
* chanSendWait: send a message to a channel and return after it has been received
* chanPoll: do a channel operation on one of an array of channels

If provided, a "channel" invokes a queue implementation (while a mutex lock is held.)
The implementation can control queue latency, priority, etc.
A "channel fifo" queue implementation is provided:

* chanFifoQa: allocate a chanFifoQc (chanFifo context)
* chanFifoQd: deallocate a chanFifoQc (chanFifo context)
* chanFifoQi: chanFifo queue implementation

Use "make" to build.

An example of using this is provided in test/primes.c. It is modeled on [libtask's primes.c](https://swtch.com/libtask/primes.c).
It is more complex because of pthread's API and to demonstrate various combinations of options.
