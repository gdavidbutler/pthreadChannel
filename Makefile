CFLAGS = -I. -Os -g
SQLITE_INC =
SQLITE_LIB = -lsqlite3
SQLITE_CFLAGS = $(SQLITE_INC) $(CFLAGS)

all: chan.o chanStrFIFO.o chanStrFLSO.o chanStrLIFO.o chanBlb.o sockproxy pipeproxy squint floydWarshall

clean:
	rm -f chan.o chanStrFIFO.o chanStrFLSO.o chanStrLIFO.o chanBlb.o chanStrBlbSQL.o sockproxy pipeproxy squint floydWarshall chanStrBlbSQLtest

sockproxy: example/sockproxy.c chan.h chanBlb.h chan.o chanBlb.o
	$(CC) $(CFLAGS) -o sockproxy example/sockproxy.c chan.o chanBlb.o -lpthread

pipeproxy: example/pipeproxy.c chan.h chanBlb.h chan.o chanBlb.o chanStrFIFO.o
	$(CC) $(CFLAGS) -o pipeproxy example/pipeproxy.c chan.o chanBlb.o chanStrFIFO.o -lpthread

squint: example/squint.c chan.h chan.o
	$(CC) $(CFLAGS) -o squint example/squint.c chan.o -lpthread

floydWarshall: example/floydWarshall.c chan.h chan.o
	$(CC) $(CFLAGS) -Iexample -D_GNU_SOURCE -DFWMAIN -DFWEQL -DFWBLK -o floydWarshall example/floydWarshall.c chan.o -lpthread

chanStrBlbSQLtest: example/chanStrBlbSQLtest.c example/chanStrBlbSQL.h chan.h chanStrFIFO.h chanBlb.h chanStrBlbSQL.o chanStrFIFO.o chanBlb.o
	$(CC) $(SQLITE_CFLAGS) -Iexample -o chanStrBlbSQLtest example/chanStrBlbSQLtest.c chanStrBlbSQL.o chanStrFIFO.o chanBlb.o chan.o $(SQLITE_LIB)

chanStrBlbSQL.o: example/chanStrBlbSQL.c example/chanStrBlbSQL.h chan.h chanStrFIFO.h chanBlb.h
	$(CC) $(SQLITE_CFLAGS) -Iexample -c example/chanStrBlbSQL.c

# for MacOS change to
#	$(CC) $(CFLAGS) -D_GNU_SOURCE -c chan.c
chan.o: chan.c chan.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DHAVE_CONDATTR_SETCLOCK -c chan.c

chanStrFIFO.o: chanStrFIFO.c chanStrFIFO.h chan.h
	$(CC) $(CFLAGS) -c chanStrFIFO.c

chanStrFLSO.o: chanStrFLSO.c chanStrFLSO.h chan.h
	$(CC) $(CFLAGS) -c chanStrFLSO.c

chanStrLIFO.o: chanStrLIFO.c chanStrLIFO.h chan.h
	$(CC) $(CFLAGS) -c chanStrLIFO.c

chanBlb.o: chanBlb.c chanBlb.h chan.h
	$(CC) $(CFLAGS) -c chanBlb.c

check: squint
	./squint
