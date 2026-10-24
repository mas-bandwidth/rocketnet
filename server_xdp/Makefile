# Builds server_xdp.o by hand, from this directory. The library build compiles
# the same object with the same flags in cmake/EmbedXdp.cmake and embeds it;
# a server host needs neither this Makefile nor clang.
MULTIARCH := $(shell $(CC) -dumpmachine | sed 's/-pc-/-/;s/-unknown-/-/')
# ROCKETNET_HISTORY_PACKETS sets the kernel replay window; pass the library's
# value when it is not the default (make ROCKETNET_HISTORY_PACKETS=8192).
ROCKETNET_HISTORY_PACKETS ?= 4096
# the replay map holds every client: pass the library build's value
# (make ROCKETNET_MAX_CLIENTS=10000); the default is the header's.
ROCKETNET_MAX_CLIENTS ?= $(shell sed -n 's/^\#define ROCKETNET_MAX_CLIENTS  *\([0-9][0-9]*\).*/\1/p' ../sdk/rocketnet.h)
server_xdp.o: server_xdp.c ../sdk/rocketnet_wire.h ../sdk/rocketnet_xdp_limits.h ../sdk/rocketnet_history.h
	clang -D__linux__ -D__BPF__ -DROCKETNET_HISTORY_PACKETS=$(ROCKETNET_HISTORY_PACKETS) -DROCKETNET_MAX_CLIENTS=$(ROCKETNET_MAX_CLIENTS) -ffreestanding -O2 -g -I/usr/include/$(MULTIARCH) -I../sdk -target bpf -c server_xdp.c -o server_xdp.o
