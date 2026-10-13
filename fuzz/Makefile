# rocketnet fuzz targets (#124's discovery half), built against the development
# library. Two of the three targets mint a connect token with the published
# development backend secret key (rocketnet_dev_backend_secret_key), which is
# compiled out of the release library, so they must link the development
# archive and see ROCKETNET_DEVELOPMENT=1. ci.yml's fuzz-smoke covers
# rocketnet_fuzz against the release library; fuzz-nightly.yml drives these
# three here.
#
# Linux only (libFuzzer + bpf/xdp link deps). The mac is the edit loop; the
# `compile` target proves the sources compile against the development headers
# on any host, which is where an undeclared development symbol would surface.

# := rather than ?= : GNU make's built-in CXX = g++ would otherwise win the
# default, and g++ has no libFuzzer (-fsanitize=fuzzer) support. A command
# line CXX=... still overrides this, so the nightly job's direct clang++
# invocation is unaffected.
CXX      := clang++
SANITIZE ?= -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
FLAGS     = -g -O1 -std=c++17 -ffp-contract=off -DROCKETNET_DEVELOPMENT=1 -I../sodium -I../sdk -I../tests/shared
LIB       = ../build/librocketnet_dev.a
LINKS     = -lbpf -lxdp -lpthread

TARGETS = rocketnet_fuzz rocketnet_fuzz_connect_token rocketnet_fuzz_receive_structured
SOURCES = rocketnet_fuzz.cpp rocketnet_fuzz_connect_token.cpp rocketnet_fuzz_receive_structured.cpp

all: $(TARGETS)

rocketnet_fuzz: rocketnet_fuzz.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) rocketnet_fuzz.cpp $(LIB) $(LINKS) -o $@

rocketnet_fuzz_connect_token: rocketnet_fuzz_connect_token.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) rocketnet_fuzz_connect_token.cpp $(LIB) $(LINKS) -o $@

rocketnet_fuzz_receive_structured: rocketnet_fuzz_receive_structured.cpp $(LIB)
	$(CXX) $(FLAGS) $(SANITIZE) rocketnet_fuzz_receive_structured.cpp $(LIB) $(LINKS) -o $@

compile: $(SOURCES)
	$(CXX) $(FLAGS) -fsyntax-only $(SOURCES)

clean:
	rm -f $(TARGETS)

.PHONY: all compile clean
