/* Nightly ESBMC harnesses over the actual ack and sequence SDK code.
   Each harness is selected with --function; a successful run proves only the
   symbolic domain and unwind stated in results.md. */
#include "../esbmc_shim.h"
#include "rocketnet_protocol.cpp"
#include "rocketnet_ack.cpp"
#include <stdlib.h>
#include <string.h>

#ifndef ACK_BUILD_HIGHEST_BOUND
#define ACK_BUILD_HIGHEST_BOUND 4096
#endif

#ifndef ACK_APPLY_SENT_BOUND
#define ACK_APPLY_SENT_BOUND 8
#endif

#ifndef ACK_READ_MAX_BYTES
#define ACK_READ_MAX_BYTES ROCKETNET_RECEIVE_CEILING
#endif

#ifndef ROCKETNET_ESBMC
#define __CPROVER_assume(c) ((void)(c))
#define __CPROVER_assert(c, m) ((void)(c), (void)(m))
static int nondet_int(void) { return 0; }
static unsigned nondet_uint(void) { return 0; }
static uint64_t nondet_uint64(void) { return 0; }
#endif

void harness_ack_read(void)
{
    struct rocketnet_ack_block_t block;
    uint8_t zero[1] = {0};
    __CPROVER_assert(rocketnet_ack_block_read(&block, zero, 0, nondet_uint64()) == 0,
                     "zero-byte block is refused without reading");
    int n = nondet_int();
    __CPROVER_assume(n >= 1 && n <= ACK_READ_MAX_BYTES);
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    __CPROVER_assume(data != NULL);
    uint64_t highest = nondet_uint64();
    int got = rocketnet_ack_block_read(&block, data, n, highest);
    __CPROVER_assert(got >= 0 && got <= n, "decoded length fits exact input object");
    if (n < ROCKETNET_ACK_BLOCK_MIN_BYTES)
        __CPROVER_assert(got == 0, "short block refused");
    if (got != 0)
        __CPROVER_assert(block.num_ranges >= 0 && block.num_ranges <= ROCKETNET_ACK_BLOCK_MAX_RANGES,
                         "decoded range count fits block arrays");
    free(data);
}

void harness_ack_reconstruct(void)
{
    uint32_t wire = nondet_uint();
    uint64_t highest = nondet_uint64();
    uint64_t largest = rocketnet_reconstruct_ack_largest(wire, highest);
    __CPROVER_assert((uint32_t)largest == wire, "reconstructed low bits equal wire value");
}

void harness_ack_write(void)
{
    struct rocketnet_ack_block_t block;
    memset(&block, 0, sizeof(block));
    block.num_ranges = nondet_int();
    __CPROVER_assume(block.num_ranges >= 0 && block.num_ranges <= ROCKETNET_ACK_BLOCK_MAX_RANGES);
    block.first_range_length = nondet_uint();
    __CPROVER_assume(block.first_range_length <= 0xFFFFu);
    block.largest = nondet_uint64();
    for (int i = 0; i < block.num_ranges; ++i) {
        block.range_gap[i] = 1;
        block.range_length[i] = 1;
    }
    const int capacity = ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * block.num_ranges;
    uint8_t *output = (uint8_t *)malloc((size_t)capacity);
    __CPROVER_assume(output != NULL);
    int written = rocketnet_ack_block_write(output, &block);
    __CPROVER_assert(written == capacity, "writer uses exactly its documented capacity");
    free(output);
}

void harness_ack_build(void)
{
    struct rocketnet_replay_protection_t replay;
#if ACK_BUILD_HIGHEST_BOUND <= 64
    memset(&replay, 0, sizeof(replay));
    replay.window[0] = nondet_uint64();
#else
    for (int i = 0; i < ROCKETNET_REPLAY_WINDOW_BITS / 64; ++i)
        replay.window[i] = nondet_uint64();
#endif
    // The replay invariant: the highest opened sequence is marked received.
    replay.highest_opened = nondet_uint64();
    __CPROVER_assume(replay.highest_opened <= ACK_BUILD_HIGHEST_BOUND);
    if (replay.highest_opened != 0) {
        uint64_t slot = replay.highest_opened % ROCKETNET_REPLAY_WINDOW_BITS;
        replay.window[slot / 64] |= 1ull << (slot % 64);
    }
    int capacity = nondet_int();
    __CPROVER_assume(capacity >= ROCKETNET_ACK_BLOCK_MIN_BYTES && capacity <= ROCKETNET_RECEIVE_CEILING);
    uint8_t *output = (uint8_t *)malloc((size_t)capacity);
    __CPROVER_assume(output != NULL);
    int written = rocketnet_ack_block_build(output, capacity, &replay);
    __CPROVER_assert(written >= ROCKETNET_ACK_BLOCK_MIN_BYTES && written <= capacity,
                     "built block fits exact output object");
    free(output);
}

void harness_ack_apply(void)
{
    // Read a symbolic block first. Only an accepted block reaches the actual
    // apply path. The sent history is bounded to eight sequences for this run.
    uint8_t data[ROCKETNET_ACK_BLOCK_MIN_BYTES];
    struct rocketnet_ack_block_t block;
    int n = rocketnet_ack_block_read(&block, data, sizeof(data), ACK_APPLY_SENT_BOUND);
    __CPROVER_assume(n != 0);
    static struct rocketnet_ack_endpoint_t endpoint;
    endpoint.highest_sequence = ACK_APPLY_SENT_BOUND;
    endpoint.sent_bits_checked = 0;
    for (uint64_t sequence = 1; sequence <= ACK_APPLY_SENT_BOUND; ++sequence) {
        const uint32_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
        endpoint.sent[slot].sequence = sequence;
        endpoint.sent[slot].packet_id = sequence;
        endpoint.sent[slot].send_time = 999.0;
        endpoint.sent[slot].has_packet_id = 1;
        endpoint.sent_held[slot / 64] |= 1ull << (slot % 64);
        endpoint.sent_unacked[slot / 64] |= 1ull << (slot % 64);
    }
    uint64_t ids[ACK_APPLY_SENT_BOUND];
    int count = 0;
    uint64_t unknown = 0, full = 0;
    int added = rocketnet_ack_endpoint_process_block(&endpoint, &block, 1000.0,
                                                      ids, ACK_APPLY_SENT_BOUND, &count, &unknown, &full);
    __CPROVER_assert(added >= 0 && count >= 0 && count <= ACK_APPLY_SENT_BOUND,
                     "accepted block cannot overfill acknowledgment batch");
}
