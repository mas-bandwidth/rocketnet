# rocketnet driver bench (#81)
#
# probe          -- static feature report, runnable on any Linux host
# bench_portable -- sendto/recvfrom negative control
# bench_gso      -- sendmmsg/recvmmsg + UDP GSO/GRO
# bench_uring    -- io_uring batched send + multishot recv (needs liburing;
#                   pin it per project: make bench_uring LIBURING=/path/to/liburing-install)
#
# Linux only. The mac is the edit loop; numbers come from real Linux hardware.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

LIBURING ?=

all: probe bench_portable bench_gso

probe: probe.c
	$(CC) $(CFLAGS) -static -o probe probe.c

bench_portable: bench_portable.c bench_common.h
	$(CC) $(CFLAGS) -o bench_portable bench_portable.c

bench_gso: bench_gso.c bench_common.h
	$(CC) $(CFLAGS) -o bench_gso bench_gso.c

bench_uring: bench_uring.c bench_common.h
	$(CC) $(CFLAGS) -I$(LIBURING)/include -o bench_uring bench_uring.c $(LIBURING)/lib/liburing.a

clean:
	rm -f probe bench_portable bench_gso bench_uring

.PHONY: all clean
