
server_xdp.o: server_xdp.c
	clang -D__linux__ -D__BPF__ -ffreestanding -O2 -g -I/usr/include/$(shell $(CC) -dumpmachine) -target bpf -c server_xdp.c -o server_xdp.o
