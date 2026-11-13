/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SEND_QUEUE_H
#define ROCKETNET_SDK_ROCKETNET_SEND_QUEUE_H

// The arithmetic every per-queue server send shares (the io_uring per-queue send, and
// the XDP transport's own send staging, which carries the same arithmetic): which queue a destination's packets go
// through, the packet id that carries that queue so finish and abort find it
// again, the claim of a bank's next slot, and the slot a finish or abort
// pointer names. All pure, no platform.
// proofs/tla/RNUringQueueBankLayout.tla models them at the code's layout:
// QueueOf, PidOf, Start (the claim) and SlotFrom.

#include <stdint.h>
#include <stddef.h>
#include "rocketnet_address.h"

#if defined( __cplusplus )
#include <atomic>

// the queue for a destination: a hash of its address bytes and port, so
// every packet to one client goes through one queue and leaves in the order
// it was staged (proofs/tla/RNUringQueueBankLayout.tla QueueOf)

inline int rocketnet_send_queue_of( const rocketnet_address_t * to, int num_queues )
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t * address = reinterpret_cast<const uint8_t *>( &to->data );
    const int address_bytes = to->type == ROCKETNET_ADDRESS_IPV6 ? 16 : 4;
    for ( int i = 0; i < address_bytes; i++ )
    {
        hash ^= address[i];
        hash *= 0x100000001B3ULL;
    }

    hash ^= (uint8_t) ( to->port & 0xFF );
    hash *= 0x100000001B3ULL;
    hash ^= (uint8_t) ( to->port >> 8 );
    hash *= 0x100000001B3ULL;
    return (int) ( hash % (uint64_t) num_queues );
}

// the packet id of the serial-th start on a queue: a multiple of the queue
// count plus the queue, so the id's remainder by the queue count is the
// queue (proofs/tla/RNUringQueueBankLayout.tla PidOf, IdRule "mul")

inline uint64_t rocketnet_send_queue_packet_id( uint64_t serial, int num_queues, int queue )
{
    return serial * (uint64_t) num_queues + (uint64_t) queue;
}

inline int rocketnet_send_queue_of_packet( uint64_t packet_id, int num_queues )
{
    return (int) ( packet_id % (uint64_t) num_queues );
}

// Claims the next slot of a bank. A claim at or past limit is refused and
// counted in refused. The count is 64 bits: a bank never takes 2^64 claims
// without a reset, so it never wraps back onto a slot it handed out
// (proofs/tla/RNUringQueueBankLayout.tla Start, ClaimCmp "ge",
// CounterWrap 0). Returns the slot index or -1.

inline int rocketnet_send_queue_claim( std::atomic<uint64_t> & num_packets, int limit, std::atomic<uint64_t> & refused )
{
    const uint64_t index = num_packets.fetch_add( 1 );
    if ( index >= (uint64_t) limit )
    {
        refused.fetch_add( 1, std::memory_order_relaxed );
        return -1;
    }

    return (int) index;
}

#endif // #if defined( __cplusplus )

// the slot a pointer names in a bank of slots of stride bytes, from its byte
// offset past the bank's first slot: a negative, unaligned or past-the-end
// offset is refused (-1) BEFORE it is divided, since C division truncates
// toward zero and would map -stride+1..-1 to slot 0
// (proofs/tla/RNUringQueueBankLayout.tla SlotFrom, SlotRule "check")

static inline int rocketnet_send_queue_slot( ptrdiff_t offset, int stride, int slots )
{
    if ( offset < 0 || offset % stride != 0 )
    {
        return -1;
    }
    const ptrdiff_t slot = offset / stride;
    if ( slot >= slots )
    {
        return -1;
    }
    return (int) slot;
}

#endif // ROCKETNET_SDK_ROCKETNET_SEND_QUEUE_H
