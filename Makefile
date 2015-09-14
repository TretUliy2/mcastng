
WARNS?= 3
PROG= mcastng

SRCS= main.c udp.c http.c config.c log.c
SRCS+= config.h ng-r.h 
LDADD+= -lpthread -lc -lnetgraph


DESTDIR= /usr/local
BINDIR= /bin

MAN=

afterinstall: 
	install -o root -g wheel -m 555   rc/mcastng /usr/local/etc/rc.d
	install -o root -g wheel -m 555   mcastng.cfg-sample /usr/local/etc

.include <bsd.prog.mk>
