# rocketnet fuzz targets (the discovery half), built against the development
# library. Two of the three targets mint a connect token with the published
# development backend secret key (rocketnet_dev_backend_secret_key), which is
# compiled out of the release library, so they must link the development
# archive and see ROCKETNET_DEV=1. ci.yml's fuzz-smoke covers
# rocketnet_fuzz against the release library; fuzz-nightly.yml drives the
# fourteen targets in TARGETS below (and rocketnet_fuzz_xdp_filter on Linux), with the same flags and link set, plus
# rocketnet_fuzz_differential, which needs the pinned netcode checkout.
#
# The bpf/xdp link deps are Linux only; darwin links the same library without
# them and adds the CoreFoundation and SystemConfiguration frameworks the
# platform layer's interface lookup uses, as CMakeLists.txt does. The `compile` target proves the sources compile against the development
# headers on any host, which is where an undeclared development symbol would
# surface.

# := rather than ?= : GNU make's built-in CXX = g++ would otherwise win the
# default, and g++ has no libFuzzer (-fsanitize=fuzzer) support. A command
# line CXX=... still overrides this, so the nightly job's direct clang++
# invocation is unaffected.
#
# ROCKETNET_FUZZ_CXX names the compiler that links libFuzzer. Apple clang
# ships no libFuzzer runtime, so on a Mac point it at an LLVM toolchain that
# does, for example the LLVM release for macOS ARM64 unpacked at ~/sdk/llvm:
# make ROCKETNET_FUZZ_CXX="$$HOME/sdk/llvm/bin/clang++".
CXX      := $(if $(ROCKETNET_FUZZ_CXX),$(ROCKETNET_FUZZ_CXX),clang++)
SANITIZE ?= -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
FLAGS     = -g -O1 -std=c++17 -ffp-contract=off -DROCKETNET_DEV=1 -I../../sodium -I../../sdk
BUILD    ?= ../../build
LIB       = $(BUILD)/librocketnet_dev.a
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LINKS     = -lpthread -framework CoreFoundation -framework SystemConfiguration
else
LINKS     = -lbpf -lxdp -luring -lpthread
endif

TARGETS = rocketnet_fuzz \
          rocketnet_fuzz_connect_token \
          rocketnet_fuzz_receive_structured \
          rocketnet_fuzz_message \
          rocketnet_fuzz_token_private \
          rocketnet_fuzz_round_trip \
          rocketnet_fuzz_config \
          rocketnet_fuzz_server_transport \
          rocketnet_fuzz_client_transport \
          rocketnet_fuzz_session \
          rocketnet_fuzz_xdp_frame \
          rocketnet_fuzz_uring_recvmsg \
          rocketnet_fuzz_gateway_netlink \
          rocketnet_fuzz_endpoint

# TARGETS is the list every replay and fuzz job names (the per-commit
# fuzz-smoke replay, the macOS and Windows replays, the NDEBUG lists, the CMake
# fuzz_replay_ drivers, fuzz-nightly.yml): tools/docs/check_fuzz_harnesses.py
# fails on a harness source or a corpus directory that no list names.
SOURCES = $(addsuffix .cpp,$(TARGETS))

# The TARGETS harnesses that build everywhere but fuzz only on Linux: off
# Linux their entry point is a no-op (`INITED cov: 1`), so fuzz-nightly.yml's
# macOS and Windows matrices leave them out (the replays keep them: a replay of
# a no-op is cheap and keeps the build honest). check_fuzz_harnesses.py holds
# the macOS and Windows matrices to TARGETS less this list, and each source
# here to a Linux guard (__linux__ or ROCKETNET_XDP).
LINUX_ONLY_TARGETS = rocketnet_fuzz_uring_recvmsg \
                     rocketnet_fuzz_gateway_netlink

# The XDP filter program (sdk/xdp/server_xdp.c) as host C, through the unit
# suite's shim (tests/xdp_host/), instrumented like the library. GNU C on a
# non-Windows host only; on Darwin the target is not built. XDP_TARGETS is
# the Linux list: fuzz-smoke replays it (xdp_harnesses), CMake builds its
# fuzz_replay_ driver on Linux, and fuzz-nightly.yml fuzzes it on Linux.
XDP_HOST_OBJ = $(BUILD)/xdp_host_fuzz.o
ifeq ($(UNAME_S),Darwin)
XDP_TARGETS =
else
XDP_TARGETS = rocketnet_fuzz_xdp_filter
endif

all: $(TARGETS) $(XDP_TARGETS)

$(XDP_HOST_OBJ): ../xdp_host/server_xdp_host.c ../../sdk/xdp/server_xdp.c
	$(subst clang++,clang,$(CXX)) -g -O1 -std=gnu11 -ffreestanding -ffp-contract=off -D__linux__ -D__BPF__ \
	    -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -I../xdp_host -I../../sdk "-DROCKETNET_XDP_FILTER_SOURCE=\"$(abspath ../../sdk/xdp/server_xdp.c)\"" \
	    -c ../xdp_host/server_xdp_host.c -o $@

rocketnet_fuzz_xdp_filter: rocketnet_fuzz_xdp_filter.cpp $(XDP_HOST_OBJ) $(LIB)
	$(CXX) $(FLAGS) -I.. $(SANITIZE) $< $(XDP_HOST_OBJ) $(LIB) $(LINKS) -o $@

# the pair targets compile the server transport and the loopback driver into
# themselves through rocketnet_fuzz_pair.h, so they rebuild when it changes.
rocketnet_fuzz_server_transport rocketnet_fuzz_client_transport rocketnet_fuzz_session: rocketnet_fuzz_pair.h

$(TARGETS): %: %.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) $< $(LIB) $(LINKS) -o $@

compile: $(SOURCES)
	$(CXX) $(FLAGS) -fsyntax-only $(SOURCES)

clean:
	rm -f $(TARGETS) $(XDP_TARGETS) $(XDP_HOST_OBJ)

.PHONY: all compile clean
