# Builds server_xdp.o by hand, from this directory. The library build compiles
# the same object with the same flags in cmake/EmbedXdp.cmake and embeds it;
# a server host needs neither this Makefile nor clang.
server_xdp.o: server_xdp.c
	clang -D__linux__ -D__BPF__ -ffreestanding -O2 -g -I/usr/include/$(shell $(CC) -dumpmachine) -I../sdk -target bpf -c server_xdp.c -o server_xdp.o
