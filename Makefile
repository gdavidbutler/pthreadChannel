CFLAGS = -I. -Os -g

all: chan.o chanFifo.o chanBlb.o primes sockproxy pipeproxy squint

clean:
	rm -f chan.o chanFifo.o chanBlb.o primes sockproxy pipeproxy squint

primes: example/primes.c chan.h chanFifo.h chan.o chanFifo.o
	$(CC) $(CFLAGS) -o primes example/primes.c chan.o chanFifo.o -lpthread

sockproxy: example/sockproxy.c chan.h chanBlb.h chan.o chanBlb.o
	$(CC) $(CFLAGS) -o sockproxy example/sockproxy.c chan.o chanBlb.o -lpthread

pipeproxy: example/pipeproxy.c chan.h chanBlb.h chan.o chanBlb.o
	$(CC) $(CFLAGS) -o pipeproxy example/pipeproxy.c chan.o chanBlb.o -lpthread

squint: example/squint.c chan.h chanFifo.h chan.o chanFifo.o
	$(CC) $(CFLAGS) -o squint example/squint.c chan.o chanFifo.o -lpthread

chan.o: chan.c chan.h
	$(CC) $(CFLAGS) -DHAVE_CONDATTR_SETCLOCK -c chan.c

chanFifo.o: chanFifo.c chanFifo.h chan.h
	$(CC) $(CFLAGS) -c chanFifo.c

chanBlb.o: chanBlb.c chanBlb.h chan.h
	$(CC) $(CFLAGS) -c chanBlb.c

check: primes squint
	./primes 10
	./squint
