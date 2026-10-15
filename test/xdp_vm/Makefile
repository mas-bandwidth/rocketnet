# The XDP kernel tier's two helpers, built outside the CMake build and
# outside ./bin/test. Linux only. run.sh builds them into its run directory:
#
#     make -C test/xdp_vm OUT=<dir>
#
# xdp_vm_attach links libbpf and libxdp, as the library's RocketNet::Bpf and
# RocketNet::Xdp do; xdp_vm_replay needs nothing but libc.

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
OUT     ?= .
SDK     := ../../sdk

XDP_LIBS := $(shell pkg-config --libs libxdp libbpf 2>/dev/null || echo -lxdp -lbpf -lelf -lz)
XDP_CFLAGS := $(shell pkg-config --cflags libxdp libbpf 2>/dev/null)

all: $(OUT)/xdp_vm_attach $(OUT)/xdp_vm_replay

$(OUT)/xdp_vm_attach: xdp_vm_attach.c $(SDK)/rocketnet_wire.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(XDP_CFLAGS) -I$(SDK) -o $@ xdp_vm_attach.c $(XDP_LIBS)

$(OUT)/xdp_vm_replay: xdp_vm_replay.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ xdp_vm_replay.c

clean:
	rm -f $(OUT)/xdp_vm_attach $(OUT)/xdp_vm_replay

.PHONY: all clean
