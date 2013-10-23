CC=gcc
ARFLAGS=rcs
CPP=g++
CIFLAGS=
LIBS=
#CMFLAGS=-DDEBUG
CFLAGS=-g -rdynamic -fPIC -Wall -Wformat $(CIFLAGS) $(CMFLAGS)
ARCH=`uname -i`
LIBDIR=

sources = snmpwalk-scheduler.c

all:snmpwalk-scheduler

snmpwalk-scheduler:snmpwalk-scheduler.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBDIR) $(LIBS)

include $(sources:.c=.d)

%.d: %.c
	set -e; rm -f $@; \
	$(CC) -MM $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

.PHONY: clean

clean:
	rm -f snmpwalk-scheduler.d snmpwalk-scheduler.o snmpwalk-scheduler
