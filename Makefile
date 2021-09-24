CFLAGS  := -D_GNU_SOURCE -DNORAND -DTIMESTAMP -Wall -g -O2 -Iyama/include -Iyama/rdma-core-50mlnx1/build/include '-Wl,-rpath,$$ORIGIN/yama/rdma-core-50mlnx1/build/lib' -Wl,--export-dynamic
LD      := gcc
LDFLAGS := ${LDFLAGS} -Lyama/rdma-core-50mlnx1/build/lib -lrdmacm -libverbs -lrt -lpthread 

yama_starter: yama_starter.o common.o conn.o yama
	${LD} -o $@ yama_starter.o common.o conn.o yama/comm_rcm.o yama/comm.o yama/flow.o yama/logging.o yama/pacer.o ${LDFLAGS}

yama:
	cd yama && ./build.sh

# main: common.o conn.o main.o
# 	${LD} -o $@ $^ ${LDFLAGS}

PHONY: yama clean

clean:
	rm -f *.o main yama_starter
