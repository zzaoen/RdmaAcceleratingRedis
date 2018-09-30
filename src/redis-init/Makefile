.PHONY: clean

CFLAGS  := -g
LD      := gcc
LDFLAGS := ${LDFLAGS} -lhiredis

APPS    := redis-init

all: ${APPS}


redis-init: redis-init.o
	${LD} -o $@ $^ ${LDFLAGS}

clean:
	rm -f *.o ${APPS}

