# rocketnet driver bench (#81)
#
# probe          -- static feature report, runnable on any Linux host
# bench_portable -- sendto/recvfrom negative control
# bench_gso      -- sendmmsg/recvmmsg + UDP GSO/GRO
# bench_uring    -- io_uring batched send + multishot recv (needs liburing;
#                   pin it per project: make bench_uring LIBURING=/path/to/liburing-install)
#
# Linux only. The mac is the edit loop; numbers come from real Linux hardware.
#
# darwin targets (#100) -- the macOS driver's own instruments, native on
# the mac (make darwin):
#
# probe_darwin   -- runtime feature report: sendmsg_x/recvmsg_x shapes,
#                   SO_REUSEPORT fan-out distribution, kqueue, buffers
# bench_msgx     -- sendmsg_x/recvmsg_x batching (one syscall per batch)
# bench_kqueue   -- kevent readiness + per-packet recvfrom drain
# bench_nw       -- Network.framework, measured as its own callback shape
#
# bench_portable builds on both and is the shared negative control.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

LIBURING ?=

all: probe bench_portable bench_gso

darwin: probe_darwin bench_portable bench_msgx bench_kqueue bench_nw

probe_darwin: probe_darwin.c
	$(CC) $(CFLAGS) -o probe_darwin probe_darwin.c

bench_msgx: bench_msgx.c bench_common.h
	$(CC) $(CFLAGS) -o bench_msgx bench_msgx.c

bench_kqueue: bench_kqueue.c bench_common.h
	$(CC) $(CFLAGS) -o bench_kqueue bench_kqueue.c

bench_nw: bench_nw.c bench_common.h
	$(CC) $(CFLAGS) -fblocks -o bench_nw bench_nw.c -framework Network

probe: probe.c
	$(CC) $(CFLAGS) -static -o probe probe.c

bench_portable: bench_portable.c bench_common.h
	$(CC) $(CFLAGS) -o bench_portable bench_portable.c

bench_gso: bench_gso.c bench_common.h
	$(CC) $(CFLAGS) -o bench_gso bench_gso.c

bench_uring: bench_uring.c bench_common.h
	$(CC) $(CFLAGS) -I$(LIBURING)/include -o bench_uring bench_uring.c $(LIBURING)/lib/liburing.a

clean:
	rm -f probe bench_portable bench_gso bench_uring probe_darwin bench_msgx bench_kqueue bench_nw

.PHONY: all clean
