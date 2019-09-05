CFLAGS = -Wall -Wextra -Wpedantic -I. -Os -g
#CFLAGS = -Wall -Wextra -Wpedantic -I. -Os -DNDEBUG

all: chan.o chanFifo.o chanSock.o primes

clean:
	rm -f chan.o chanFifo.o chanSock.o primes

chan.o: chan.c chan.h
chanFifo.o: chanFifo.c chanFifo.h chan.h
chanSock.o: chanSock.c chanSock.h chan.h

primes: example/primes.c chan.h chanFifo.h chan.o chanFifo.o
	cc $(CFLAGS) -o primes example/primes.c chan.o chanFifo.o -lpthread
