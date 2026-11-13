/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_URING_SEND_QUEUE_H
#define ROCKETNET_SDK_ROCKETNET_URING_SEND_QUEUE_H

// The io_uring server's per-queue send banks (layer L0 of the per-queue
// send): Q queues, each with ROCKETNET_URING_SEND_BANKS banks of slots of
// ROCKETNET_FRAME_STRIDE bytes. start picks the destination's queue
// (rocketnet_send_queue_of, the XDP transport's hash), takes a packet id that
// carries the queue, and claims the next slot of that queue's producer bank;
// finish and abort take the queue from the id and the slot from the pointer,
// tested before any write. turn is send_packets with the fill threads joined:
// each queue that staged turns its producer over to its next bank, reset
// before any fill sees it. The handoff of a turned bank to the queue's
// worker, and the rule that a bank is reused only after every completion of
// its sends, are layer L1, the io_uring transport's own (a later piece).
//
// Portable and pure: no ring, no socket, no thread. The slot count is a
// runtime argument so the trace harness runs it at the model's size; the
// io_uring transport sizes it by rate at create, from
// ROCKETNET_URING_SEND_BANK_SLOTS up to ROCKETNET_URING_SEND_MAX_BANK_SLOTS.
// proofs/tla/RNUringQueueBankLayout.tla models it at this layout; each
// function cites the action it implements.

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_send_queue.h"

#include <atomic>
#include <stdint.h>

#define ROCKETNET_URING_SEND_BANKS                                           3
#define ROCKETNET_URING_SEND_BANK_SLOTS                                   4096    // the fewest slots a transport queue has
#define ROCKETNET_URING_SEND_MAX_BANK_SLOTS                              32768    // the most slots one queue may have
#define ROCKETNET_URING_SEND_MAX_QUEUES                                     32

struct rocketnet_uring_send_slot_t
{
    struct rocketnet_address_t to;
    int packet_bytes;                       // 0: not finished, or aborted
};

struct rocketnet_uring_send_bank_t
{
    std::atomic<uint64_t> num_packets;      // claims since the bank was reset; past slots once full
    struct rocketnet_uring_send_slot_t * slot;
    uint8_t * data;                         // slots * ROCKETNET_FRAME_STRIDE bytes
};

struct rocketnet_uring_send_queue_t
{
    int producer;                           // the bank start claims from; written only by turn
    struct rocketnet_uring_send_bank_t bank[ROCKETNET_URING_SEND_BANKS];
};

struct rocketnet_uring_send_queues_t
{
    void * context;
    int num_queues;
    int slots;
    std::atomic<uint64_t> packet_id;        // the server's id counter: serial of the next start
    std::atomic<uint64_t> refused;          // starts refused: the producer bank was full
    std::atomic<uint64_t> refused_pointer;  // finishes and aborts whose pointer named no slot of the producer bank
    struct rocketnet_uring_send_queue_t * queue;
    void * memory;
};

// NULL when num_queues is outside 1..ROCKETNET_URING_SEND_MAX_QUEUES or
// slots is outside 1..ROCKETNET_URING_SEND_MAX_BANK_SLOTS

struct rocketnet_uring_send_queues_t * rocketnet_uring_send_queues_create( void * context, int num_queues, int slots );

void rocketnet_uring_send_queues_destroy( struct rocketnet_uring_send_queues_t * queues );

// the bytes create allocates, 0 for a count or size out of range

uint64_t rocketnet_uring_send_queues_allocation_bytes( int num_queues, int slots );

// any game thread between two turns. NULL when the destination's producer
// bank is full (counted in refused); packet_id is written either way

uint8_t * rocketnet_uring_send_queues_start( struct rocketnet_uring_send_queues_t * queues, const struct rocketnet_address_t * to, uint64_t * packet_id );

// any game thread between two turns. false when the pointer names no slot of
// the producer bank of the id's queue (counted in refused_pointer): nothing
// is written

bool rocketnet_uring_send_queues_finish( struct rocketnet_uring_send_queues_t * queues, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

bool rocketnet_uring_send_queues_abort( struct rocketnet_uring_send_queues_t * queues, uint64_t packet_id, uint8_t * packet_data );

// the slots of a queue's producer bank the game threads have claimed:
// min(num, slots). Main thread only, with the game threads joined (as turn
// is): the count is read while no game thread claims

int rocketnet_uring_send_queues_staged( const struct rocketnet_uring_send_queues_t * queues, int queue );

// main thread, the game threads' writes joined: queue's producer moves to bank
// next, whose counter is reset before any game thread claims from it. next is
// the bank the queue's exchange handed over (layer L1), which no game thread
// and no worker holds.

void rocketnet_uring_send_queues_turn_to( struct rocketnet_uring_send_queues_t * queues, int queue, int next );

// main thread, the game threads' writes joined: every queue that staged moves its
// producer to its next bank in turn (turn_to the producer + 1) and resets that
// bank. Returns the queues turned. The bank layer alone, as its model's Flush
// has it: the trace walk and the unit suite of L0, where every bank is free
// again before the producer comes back to it. The transport turns through its
// exchange instead.

int rocketnet_uring_send_queues_turn( struct rocketnet_uring_send_queues_t * queues );

#endif // ROCKETNET_SDK_ROCKETNET_URING_SEND_QUEUE_H
