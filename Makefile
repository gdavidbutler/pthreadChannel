CFLAGS = -Wall -Wextra -Wpedantic -I. -Os -g
#CFLAGS = -Wall -Wextra -Wpedantic -I. -Os -DNDEBUG

all: chan.o chanFifo.o chanSock.o

example: all primes

clean:
	rm -f chan.o chanFifo.o chanSock.o

clobber: clean
	rm -f primes

chan.o: chan.c chan.h
chanFifo.o: chanFifo.c chanFifo.h chan.h
chanSock.o: chanSock.c chanSock.h chan.h

primes: example/primes.c chan.h chanFifo.h chan.o chanFifo.o
	cc $(CFLAGS) -o primes example/primes.c chan.o chanFifo.o -lpthread

run: primes
	./primes 100
