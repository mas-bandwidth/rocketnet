# rocketnet fuzz targets (#124's discovery half), built against the development
# library. Two of the three targets mint a connect token with the published
# development backend secret key (rocketnet_dev_backend_secret_key), which is
# compiled out of the release library, so they must link the development
# archive and see ROCKETNET_DEV=1. ci.yml's fuzz-smoke covers
# rocketnet_fuzz against the release library; fuzz-nightly.yml drives these
# four here.
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
FLAGS     = -g -O1 -std=c++17 -ffp-contract=off -DROCKETNET_DEV=1 -I../sodium -I../sdk -I../test
BUILD    ?= ../build
LIB       = $(BUILD)/librocketnet_dev.a
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LINKS     = -lpthread -framework CoreFoundation -framework SystemConfiguration
else
LINKS     = -lbpf -lxdp -luring -lpthread
endif

TARGETS = rocketnet_fuzz rocketnet_fuzz_connect_token rocketnet_fuzz_receive_structured rocketnet_fuzz_message
SOURCES = rocketnet_fuzz.cpp rocketnet_fuzz_connect_token.cpp rocketnet_fuzz_receive_structured.cpp rocketnet_fuzz_message.cpp

all: $(TARGETS)

rocketnet_fuzz: rocketnet_fuzz.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) rocketnet_fuzz.cpp $(LIB) $(LINKS) -o $@

rocketnet_fuzz_connect_token: rocketnet_fuzz_connect_token.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) rocketnet_fuzz_connect_token.cpp $(LIB) $(LINKS) -o $@

rocketnet_fuzz_receive_structured: rocketnet_fuzz_receive_structured.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) rocketnet_fuzz_receive_structured.cpp $(LIB) $(LINKS) -o $@

rocketnet_fuzz_message: rocketnet_fuzz_message.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) rocketnet_fuzz_message.cpp $(LIB) $(LINKS) -o $@

compile: $(SOURCES)
	$(CXX) $(FLAGS) -fsyntax-only $(SOURCES)

clean:
	rm -f $(TARGETS)

.PHONY: all compile clean
