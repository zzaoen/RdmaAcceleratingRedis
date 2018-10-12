.PHONY: clean

CFLAGS  := -g
LD      := gcc
LDFLAGS := ${LDFLAGS} -lrdmacm -libverbs -lpthread -lhiredis

APPS    := rdma-server

all: ${APPS}

#rdma-client: rdma-common.o rdma-client.o
#	${LD} -o $@ $^ ${LDFLAGS}


rdma-server: rdma-server.o get_clock.o
	${LD} -o $@ $^ ${LDFLAGS}


clean:
	rm -f *.o ${APPS}

