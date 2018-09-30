.PHONY: clean

CFLAGS  := -g
# CFLAGS  := -Wall -g
LD      := gcc
LDFLAGS := ${LDFLAGS} -lrdmacm -libverbs -lpthread -lhiredis

APPS    := rdma-client 

all: ${APPS}


rdma-client: rdma-client.o get_clock.o
	${LD} -o $@ $^ ${LDFLAGS}



clean:
	rm -f *.o ${APPS}

