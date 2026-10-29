# Builds server_xdp.o by hand, from this directory. The library build compiles
# the same object with the same flags in cmake/EmbedXdp.cmake and embeds it;
# a server host needs neither this Makefile nor clang.
MULTIARCH := $(shell $(CC) -dumpmachine | sed 's/-pc-/-/;s/-unknown-/-/')
# ROCKETNET_HISTORY_PACKETS sets the kernel replay window and
# ROCKETNET_MAX_CLIENTS sizes the replay map: pass the library build's values
# when they are not the defaults (make ROCKETNET_MAX_CLIENTS=10000). Unset, the
# SDK headers' defaults apply, as they do to the library.
XDP_LIMITS := $(if $(ROCKETNET_HISTORY_PACKETS),-DROCKETNET_HISTORY_PACKETS=$(ROCKETNET_HISTORY_PACKETS)) $(if $(ROCKETNET_MAX_CLIENTS),-DROCKETNET_MAX_CLIENTS=$(ROCKETNET_MAX_CLIENTS))
server_xdp.o: server_xdp.c ../rocketnet.h ../rocketnet_wire.h ../rocketnet_xdp_limits.h ../rocketnet_history.h
	clang -D__linux__ -D__BPF__ $(XDP_LIMITS) -ffreestanding -O2 -g -I/usr/include/$(MULTIARCH) -I.. -target bpf -c server_xdp.c -o server_xdp.o
