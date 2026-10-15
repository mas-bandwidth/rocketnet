# The XDP hardware tier's two programs (RUNBOOK.md), built against the
# libraries the CMake build produced, outside the CMake build and outside
# ./bin/test. Linux only, and built anywhere; run only on the dedicated,
# held machine (hold.sh, run.sh).
#
#   make                    # needs ../../build/librocketnet.a and librocketnet_dev.a
#   make BUILD=/path/to/build
#
# xdp_hw_receiver links the shipped library (librocketnet.a), so the path
# under test is the one a game links. xdp_hw_sender links the development
# library for its probe client's development connect token.

ROOT    ?= ../..
BUILD   ?= $(ROOT)/build
CXX     ?= c++
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++17 -ffp-contract=off
INCLUDES = -I$(ROOT)/sdk -I$(BUILD)/generated
LIBS     = -lxdp -lbpf -lelf -lz -lpthread

all: bin/xdp_hw_receiver bin/xdp_hw_sender

bin/xdp_hw_receiver: xdp_hw_receiver.cpp $(BUILD)/librocketnet.a
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -DROCKETNET_DEVELOPMENT=0 $(INCLUDES) -o $@ xdp_hw_receiver.cpp $(BUILD)/librocketnet.a $(LIBS)

bin/xdp_hw_sender: xdp_hw_sender.cpp $(BUILD)/librocketnet_dev.a
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -DROCKETNET_DEVELOPMENT=1 $(INCLUDES) -o $@ xdp_hw_sender.cpp $(BUILD)/librocketnet_dev.a $(LIBS)

clean:
	rm -rf bin

.PHONY: all clean
