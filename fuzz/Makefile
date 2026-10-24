# rocketnet fuzz targets (#124's discovery half), built against the development
# library. Two of the three targets mint a connect token with the published
# development backend secret key (rocketnet_dev_backend_secret_key), which is
# compiled out of the release library, so they must link the development
# archive and see ROCKETNET_DEV=1. ci.yml's fuzz-smoke covers
# rocketnet_fuzz against the release library; fuzz-nightly.yml drives the
# ten targets this Makefile builds, with the same flags and link set, plus
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
FLAGS     = -g -O1 -std=c++17 -ffp-contract=off -DROCKETNET_DEV=1 -I../sodium -I../sdk
BUILD    ?= ../build
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
          rocketnet_fuzz_session
SOURCES = $(addsuffix .cpp,$(TARGETS))

all: $(TARGETS)

# the pair targets compile the server transport and the loopback driver into
# themselves through rocketnet_fuzz_pair.h, so they rebuild when it changes.
rocketnet_fuzz_server_transport rocketnet_fuzz_client_transport rocketnet_fuzz_session: rocketnet_fuzz_pair.h

$(TARGETS): %: %.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) $< $(LIB) $(LINKS) -o $@

compile: $(SOURCES)
	$(CXX) $(FLAGS) -fsyntax-only $(SOURCES)

clean:
	rm -f $(TARGETS)

.PHONY: all compile clean
