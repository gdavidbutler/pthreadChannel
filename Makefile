CFLAGS = -Wall -Wextra -Wpedantic -I. -Os -g
#CFLAGS = -Wall -Wextra -Wpedantic -I. -Os -DNDEBUG

all: primes

clean:
	rm -f chan.o chanFifo.o

clobber: clean
	rm -f primes

chan.o: chan.c chan.h
chanFifo.o: chanFifo.c chanFifo.h chan.h

primes: example/primes.c chan.h chanFifo.h chan.o chanFifo.o
	cc $(CFLAGS) -o primes example/primes.c chan.o chanFifo.o -lpthread

run: primes
	./primes 100
