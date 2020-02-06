CFLAGS=-Wall -Wextra -Wpedantic -I. -DHAVE_CONDATTR_SETCLOCK -Os -g

all: chan.o chanFifo.o chanSer.o primes sockproxy

clean:
	rm -f chan.o chanFifo.o chanSer.o primes sockproxy

chan.o: chan.c chan.h
chanFifo.o: chanFifo.c chanFifo.h chan.h
chanSer.o: chanSer.c chanSer.h chan.h

primes: example/primes.c chan.h chanFifo.h chan.o chanFifo.o
	$(CC) $(CFLAGS) -o primes example/primes.c chan.o chanFifo.o -lpthread

sockproxy: example/sockproxy.c chan.h chanSer.h chan.o chanSer.o
	$(CC) $(CFLAGS) -o sockproxy example/sockproxy.c chan.o chanSer.o -lpthread
