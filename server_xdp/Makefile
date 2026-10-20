# Builds server_xdp.o by hand, from this directory. The library build compiles
# the same object with the same flags in cmake/EmbedXdp.cmake and embeds it;
# a server host needs neither this Makefile nor clang.
MULTIARCH := $(shell $(CC) -dumpmachine | sed 's/-pc-/-/;s/-unknown-/-/')
server_xdp.o: server_xdp.c ../sdk/rocketnet_wire.h ../sdk/rocketnet_xdp_limits.h
	clang -D__linux__ -D__BPF__ -ffreestanding -O2 -g -I/usr/include/$(MULTIARCH) -I../sdk -target bpf -c server_xdp.c -o server_xdp.o
