CFLAGS = -I. -Os -g

all: chan.o chanStr.o chanBlb.o sockproxy pipeproxy squint floydWarshall

clean:
	rm -f chan.o chanStr.o chanBlb.o sockproxy pipeproxy squint floydWarshall

sockproxy: example/sockproxy.c chan.h chanBlb.h chan.o chanBlb.o
	$(CC) $(CFLAGS) -o sockproxy example/sockproxy.c chan.o chanBlb.o -lpthread

pipeproxy: example/pipeproxy.c chan.h chanBlb.h chan.o chanBlb.o
	$(CC) $(CFLAGS) -o pipeproxy example/pipeproxy.c chan.o chanBlb.o -lpthread

squint: example/squint.c chan.h chan.o chanStr.o
	$(CC) $(CFLAGS) -o squint example/squint.c chan.o chanStr.o -lpthread

floydWarshall: example/floydWarshall.c chan.h chan.o chanStr.o
	$(CC) $(CFLAGS) -Iexample -D_GNU_SOURCE -DFWMAIN -DFWEQL -DFWBLK -o floydWarshall example/floydWarshall.c chan.o -lpthread

# for MacOS change to
#	$(CC) $(CFLAGS) -D_GNU_SOURCE -c chan.c
chan.o: chan.c chan.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DHAVE_CONDATTR_SETCLOCK -c chan.c

chanStr.o: chanStr.c chanStr.h chan.h
	$(CC) $(CFLAGS) -c chanStr.c

chanBlb.o: chanBlb.c chanBlb.h chan.h
	$(CC) $(CFLAGS) -c chanBlb.c

check: squint
	./squint
