CFLAGS = -I. -IStr -IBlb -Os -g
SQLITE_INC =
SQLITE_LIB = -lsqlite3
SQLITE_CFLAGS = $(SQLITE_INC) $(CFLAGS)

all: chan.o chanStrFIFO.o chanStrFLSO.o chanStrLIFO.o chanBlb.o sockproxy pipeproxy squint floydWarshall

clean:
	rm -f chan.o chanStrFIFO.o chanStrFLSO.o chanStrLIFO.o chanBlb.o chanStrBlbSQL.o sockproxy pipeproxy squint floydWarshall chanStrBlbSQLtest

sockproxy: example/sockproxy.c chan.h Blb/chanBlb.h chan.o chanBlb.o
	$(CC) $(CFLAGS) -o sockproxy example/sockproxy.c chan.o chanBlb.o -lpthread

pipeproxy: example/pipeproxy.c chan.h Blb/chanBlb.h chan.o chanBlb.o chanStrFIFO.o
	$(CC) $(CFLAGS) -o pipeproxy example/pipeproxy.c chan.o chanBlb.o chanStrFIFO.o -lpthread

squint: example/squint.c chan.h chan.o
	$(CC) $(CFLAGS) -o squint example/squint.c chan.o -lpthread

floydWarshall: example/floydWarshall.c chan.h chan.o
	$(CC) $(CFLAGS) -Iexample -D_GNU_SOURCE -DFWMAIN -DFWEQL -DFWBLK -o floydWarshall example/floydWarshall.c chan.o -lpthread

chanStrBlbSQLtest: example/chanStrBlbSQLtest.c example/chanStrBlbSQL.h chan.h Str/chanStrFIFO.h Blb/chanBlb.h chanStrBlbSQL.o chanStrFIFO.o chanBlb.o
	$(CC) $(SQLITE_CFLAGS) -Iexample -o chanStrBlbSQLtest example/chanStrBlbSQLtest.c chanStrBlbSQL.o chanStrFIFO.o chanBlb.o chan.o $(SQLITE_LIB)

chanStrBlbSQL.o: example/chanStrBlbSQL.c example/chanStrBlbSQL.h chan.h Str/chanStrFIFO.h Blb/chanBlb.h
	$(CC) $(SQLITE_CFLAGS) -Iexample -c example/chanStrBlbSQL.c

# for MacOS
#	$(CC) $(CFLAGS) -c chan.c
# for setclock
#	$(CC) $(CFLAGS) -DHAVE_CONDATTR_SETCLOCK -c chan.c
# for GNU
#	$(CC) $(CFLAGS) -D_GNU_SOURCE -DHAVE_CONDATTR_SETCLOCK -c chan.c
chan.o: chan.c chan.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DHAVE_CONDATTR_SETCLOCK -c chan.c

chanStrFIFO.o: Str/chanStrFIFO.c Str/chanStrFIFO.h chan.h
	$(CC) $(CFLAGS) -c Str/chanStrFIFO.c

chanStrFLSO.o: Str/chanStrFLSO.c Str/chanStrFLSO.h chan.h
	$(CC) $(CFLAGS) -c Str/chanStrFLSO.c

chanStrLIFO.o: Str/chanStrLIFO.c Str/chanStrLIFO.h chan.h
	$(CC) $(CFLAGS) -c Str/chanStrLIFO.c

chanBlb.o: Blb/chanBlb.c Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlb.c

check: squint
	./squint
