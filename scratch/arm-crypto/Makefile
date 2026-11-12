# Tracer bullet (scratch, not the product): arm64 ChaCha20 (NEON) + Poly1305
# (radix 2^64) behind the bundle's implementation structs.
#   make                      build ./harness with the repo's strict warnings
#   ./harness vectors wycheproof.txt
#   ./harness diff 10000000 <seed>
#   taskset -c 5 ./harness bench 100000
#   make ct && valgrind --error-exitcode=1 ./harness-ct ct   (Linux, valgrind headers)
CC ?= cc
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion -Werror
CFLAGS = -O3 -DNDEBUG -std=gnu11 $(WARN)

harness: harness.c rn_arm64.c sodium.c sodium.h
	$(CC) $(CFLAGS) -c rn_arm64.c -o rn_arm64.o
	$(CC) $(CFLAGS) harness.c rn_arm64.o -o harness

ct: harness.c rn_arm64.c sodium.c sodium.h
	$(CC) -O2 -g -std=gnu11 $(WARN) -DRN_CTGRIND -c rn_arm64.c -o rn_arm64_ct.o
	$(CC) -O2 -g -std=gnu11 $(WARN) -DRN_CTGRIND harness.c rn_arm64_ct.o -o harness-ct

clean:
	rm -f harness harness-ct *.o
