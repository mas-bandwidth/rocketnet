
client_backend_xdp.o: client_backend_xdp.c client_backend_shared.h client_backend_constants.h
	clang -D__linux__ -D__BPF__ -O2 -g -Ilibbpf/src -target bpf -c client_backend_xdp.c -o client_backend_xdp.o
