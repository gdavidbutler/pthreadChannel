CFLAGS = -I. -IStr -IBlb -Os -g
SQLITE_INC =
SQLITE_LIB = -lsqlite3
SQLITE_CFLAGS = $(SQLITE_INC) $(CFLAGS)
KCP = kcp

all: chan.o \
     chanStrFIFO.o chanStrFLSO.o chanStrLIFO.o \
     chanBlb.o \
     chanBlbChnVlq.o chanBlbChnNetstring.o chanBlbChnFcgi.o chanBlbChnNetconf10.o chanBlbChnNetconf11.o chanBlbChnHttp1.o \
     chanBlbTrnFd.o chanBlbTrnFdStream.o chanBlbTrnFdDatagram.o \
     sockproxy pipeproxy datagramchat squint floydWarshall

clean:
	rm -f chan.o
	rm -f chanStrFIFO.o chanStrFLSO.o chanStrLIFO.o
	rm -f chanBlb.o
	rm -f chanBlbChnVlq.o chanBlbChnNetstring.o chanBlbChnFcgi.o chanBlbChnNetconf10.o chanBlbChnNetconf11.o chanBlbChnHttp1.o
	rm -f chanBlbTrnFd.o chanBlbTrnFdStream.o chanBlbTrnFdDatagram.o
	rm -f sockproxy pipeproxy datagramchat squint floydWarshall
	rm -f chanBlbTrnKcp.o
	rm -f chanStrBlbSQL.o
	rm -f chanStrBlbSQLtest

sockproxy: example/sockproxy.c chan.h Blb/chanBlb.h Blb/chanBlbTrnFd.h Blb/chanBlbTrnFdStream.h chan.o chanBlb.o chanBlbTrnFd.o chanBlbTrnFdStream.o
	$(CC) $(CFLAGS) -o sockproxy example/sockproxy.c chan.o chanBlb.o chanBlbTrnFd.o chanBlbTrnFdStream.o -lpthread

pipeproxy: example/pipeproxy.c chan.h Blb/chanBlb.h Blb/chanBlbChnVlq.h Blb/chanBlbTrnFd.h chan.o chanStrFIFO.o chanBlb.o chanBlbTrnFd.o
	$(CC) $(CFLAGS) -o pipeproxy example/pipeproxy.c chan.o chanStrFIFO.o chanBlb.o chanBlbChnVlq.o chanBlbTrnFd.o -lpthread

datagramchat: example/datagramchat.c chan.h Blb/chanBlb.h Blb/chanBlbTrnFdDatagram.h chan.o chanBlb.o chanBlbTrnFdDatagram.o
	$(CC) $(CFLAGS) -o datagramchat example/datagramchat.c chan.o chanBlb.o chanBlbTrnFdDatagram.o -lpthread

squint: example/squint.c chan.h chan.o
	$(CC) $(CFLAGS) -o squint example/squint.c chan.o -lpthread

floydWarshall: example/floydWarshall.c chan.h chan.o
	$(CC) $(CFLAGS) -Iexample -D_GNU_SOURCE -DFWMAIN -DFWEQL -DFWBLK -o floydWarshall example/floydWarshall.c chan.o -lpthread

chanStrBlbSQLtest: example/chanStrBlbSQLtest.c example/chanStrBlbSQL.h chan.h Str/chanStrFIFO.h Blb/chanBlb.h chanStrBlbSQL.o chanStrFIFO.o chanBlb.o
	$(CC) $(SQLITE_CFLAGS) -Iexample -o chanStrBlbSQLtest example/chanStrBlbSQLtest.c chanStrBlbSQL.o chanStrFIFO.o chanBlb.o chan.o $(SQLITE_LIB)

chanStrBlbSQL.o: example/chanStrBlbSQL.c example/chanStrBlbSQL.h chan.h Str/chanStrFIFO.h Blb/chanBlb.h
	$(CC) $(SQLITE_CFLAGS) -Iexample -c example/chanStrBlbSQL.c

chanBlbTrnKcp.o: example/chanBlbTrnKcp.c example/chanBlbTrnKcp.h Blb/chanBlb.h chan.h $(KCP)/ikcp.h
	$(CC) $(CFLAGS) -I$(KCP) -c example/chanBlbTrnKcp.c

# for MacOS
#	$(CC) $(CFLAGS) -c chan.c
# for setclock
#	$(CC) $(CFLAGS) -DHAVE_CONDATTR_SETCLOCK -c chan.c
# for GNU
#	$(CC) $(CFLAGS) -D_GNU_SOURCE -DHAVE_CONDATTR_SETCLOCK -c chan.c
chan.o: chan.c chan.h
	$(CC) $(CFLAGS) -DHAVE_CONDATTR_SETCLOCK -c chan.c

chanStrFIFO.o: Str/chanStrFIFO.c Str/chanStrFIFO.h chan.h
	$(CC) $(CFLAGS) -c Str/chanStrFIFO.c

chanStrFLSO.o: Str/chanStrFLSO.c Str/chanStrFLSO.h chan.h
	$(CC) $(CFLAGS) -c Str/chanStrFLSO.c

chanStrLIFO.o: Str/chanStrLIFO.c Str/chanStrLIFO.h chan.h
	$(CC) $(CFLAGS) -c Str/chanStrLIFO.c

chanBlb.o: Blb/chanBlb.c Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlb.c

chanBlbChnVlq.o: Blb/chanBlbChnVlq.c Blb/chanBlbChnVlq.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbChnVlq.c

chanBlbChnNetstring.o: Blb/chanBlbChnNetstring.c Blb/chanBlbChnNetstring.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbChnNetstring.c

chanBlbChnFcgi.o: Blb/chanBlbChnFcgi.c Blb/chanBlbChnFcgi.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbChnFcgi.c

chanBlbChnNetconf10.o: Blb/chanBlbChnNetconf10.c Blb/chanBlbChnNetconf10.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbChnNetconf10.c

chanBlbChnNetconf11.o: Blb/chanBlbChnNetconf11.c Blb/chanBlbChnNetconf11.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbChnNetconf11.c

chanBlbChnHttp1.o: Blb/chanBlbChnHttp1.c Blb/chanBlbChnHttp1.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbChnHttp1.c

chanBlbTrnFd.o: Blb/chanBlbTrnFd.c Blb/chanBlbTrnFd.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbTrnFd.c

chanBlbTrnFdStream.o: Blb/chanBlbTrnFdStream.c Blb/chanBlbTrnFdStream.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbTrnFdStream.c

chanBlbTrnFdDatagram.o: Blb/chanBlbTrnFdDatagram.c Blb/chanBlbTrnFdDatagram.h Blb/chanBlb.h chan.h
	$(CC) $(CFLAGS) -c Blb/chanBlbTrnFdDatagram.c

check: squint
	./squint
	./pipeproxy < example/floydWarshall.stdin
	./floydWarshall < example/floydWarshall.stdin
