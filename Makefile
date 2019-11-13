CFLAGS = -Wall -Wextra -Wpedantic -I. -DHAVE_CONDATTR_SETCLOCK -Os -g

all: chan.o chanFifo.o chanSock.o primes sockproxy

clean:
	rm -f chan.o chanFifo.o chanSock.o primes sockproxy

chan.o: chan.c chan.h
chanFifo.o: chanFifo.c chanFifo.h chan.h
chanSock.o: chanSock.c chanSock.h chan.h

primes: example/primes.c chan.h chanFifo.h chan.o chanFifo.o
	$(CC)  $(CFLAGS) -o primes example/primes.c chan.o chanFifo.o -lpthread
sockproxy: example/sockproxy.c chan.h chanSock.h chan.o chanSock.o
	$(CC)  $(CFLAGS) -o sockproxy example/sockproxy.c chan.o chanSock.o -lpthread
