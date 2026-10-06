/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_ALLOCATOR_H
#define ROCKETNET_ALLOCATOR_H

#include "rocketnet.h"

// Layer 1 of the message layer: the region (docs/CHANNELS.md section 4).
//
// Each connection -- every server slot, and the client -- owns one
// contiguous region. The owner makes ONE rocketnet_malloc at create for all
// of its regions (a server max_clients of them, a client one), and carves
// each region at connect: every fixed table the channel layers need, the
// send store and the receive ring, each at a 64-byte aligned offset that
// rn_region_layout computes from the configuration by the section 14
// formula. Nothing here allocates after create (R1). When a bounded thing
// is full the answer is a refusal, counted (MESSAGE_SEND_STORE_FULL,
// MESSAGE_SEND_QUEUE_FULL), never growth.
//
// The entry types below are the layouts the layers above write. Their sizes
// are the section 14 working figures, pinned here with static asserts, so
// the formula and the code cannot drift apart: a later layer that needs a
// field changes a struct, the assert fails, and the formula changes with it.

// ----------------------------------------------------------------
//                 the entry types, one per carve-out
// ----------------------------------------------------------------

// layer 4, per channel, sender side (docs/CHANNELS.md 7.1)
struct rn_channel_send_t
{
    uint32_t ordered_write;                 // next ordering index for an ordered send
    uint32_t sequenced_write;               // next sequencing index; zeroed by every ordered send
    uint32_t queue_head, queue_tail;        // FIFO of queued send descriptors (layer 6)
    uint16_t queued[4];                     // queued count per priority (layer 6)
    uint32_t ready_prev, ready_next;        // this channel's link in its level's ready list (layer 6)
};

// layer 4, per channel, receiver side (7.1)
struct rn_channel_receive_t
{
    uint32_t ordered_read;                  // next ordering index to deliver
    uint32_t sequenced_watermark;           // lowest sequencing index still deliverable in this epoch
    uint32_t held_sequenced;                // descriptor of the newest held sequenced message, or none
    uint32_t pad;
};

// layer 3, the sender's per-number table, indexed number % W (6.1)
struct rn_send_number_t
{
    uint64_t number;                        // the full message number this slot holds
    uint64_t ring_start;                    // absolute receive-ring position (6.2)
    double last_send;                       // time of the last write, for the resend FIFO
    uint32_t descriptor;                    // the send descriptor this number belongs to
    uint16_t fragment;                      // fragment index inside a block, 0 otherwise
    uint8_t ring_units;                     // ring span in 16-byte units; ring_end = ring_start + 16 * ring_units
    uint8_t acked;
};

// layer 3, the receiver's descriptors, indexed number % W (6.1)
struct rn_receive_descriptor_t
{
    uint64_t number;
    uint32_t ring;                          // receive-ring offset in 16-byte units
    uint32_t bytes;
    uint32_t ordering;
    uint32_t sequencing;
    uint16_t channel;
    uint8_t code;
    uint8_t live;
    uint16_t block_have;                    // fragments of this block received so far
    uint16_t block_fragments;               // fragment count of this block, 0 when not a block
};

// layer 4, the held-ordered index: 2W open-addressed entries keyed by
// (channel, ordering), each naming a number (6.1, 7.2). zero is empty.
struct rn_held_entry_t
{
    uint64_t number;
    uint32_t ordering;
    uint16_t channel;
    uint16_t live;
};

// layer 6, one queued send (9.1). Q of them, pooled here: a send that finds
// none free is refused and counted as MESSAGE_SEND_QUEUE_FULL.
struct rn_send_descriptor_t
{
    uint64_t first_number;                  // the number the first fragment took, once numbered
    uint64_t ring_start;                    // the ring span reserved for a holdable record
    double queued_time;
    uint32_t chunk;                         // the send store chunk (or a block's first chunk)
    uint32_t bytes;
    uint32_t serial;                        // the receipt serial (7.4)
    uint32_t ordering;
    uint32_t sequencing;
    uint32_t next;                          // the channel FIFO link, and the pool's free list
    uint16_t channel;
    uint16_t fragments;                     // 1 for a single record, k for a block
    uint16_t fragments_acked;
    uint8_t reliability;
    uint8_t priority;
    uint8_t flags;
    uint8_t pad[7];
};

// layer 3, the sent-packet ring (6.4)
struct rn_sent_packet_t
{
    uint64_t packet_id;
    double send_time;
    uint32_t ref_first;                     // first entry in the reference ring
    uint16_t ref_count;
    uint16_t unreliable_receipts;
    uint32_t receipt_first;
    uint32_t pad;
};

// layer 3, the 2P open-addressed index from packet_id to sent entry (6.4).
// packet_id 0 is empty: the transport never issues it.
struct rn_sent_index_entry_t
{
    uint64_t packet_id;
    uint32_t entry;
    uint32_t pad;
};

// layer 7, one delivered message for the current update (10.1). data points
// into the transport's batch (zero copy) or into the receive ring.
struct rn_delivery_t
{
    const uint8_t * data;
    uint32_t bytes;
    uint16_t channel;
    uint8_t code;
    uint8_t flags;
    int32_t client_index;
    uint32_t pad;
};

// layer 4, the Ru pool holding an unreliable sequenced message ahead of its
// epoch (7.3): one wire payload of data behind a 16-byte header
struct rn_sequenced_hold_t
{
    uint32_t ordering;
    uint32_t sequencing;
    uint16_t bytes;
    uint16_t channel;
    uint8_t live;
    uint8_t pad[3];
    uint8_t data[ROCKETNET_MAX_PAYLOAD_BYTES];
};

// layer 4, the receipt ring (7.4)
struct rn_receipt_t
{
    uint32_t serial;
    uint32_t status;                        // ROCKETNET_RECEIPT_*
    uint64_t packet_id;
};

// ----------------------------------------------------------------
//                       the layout of one region
// ----------------------------------------------------------------

// the parts of a region, in carve order (docs/CHANNELS.md section 14)
enum
{
    RN_REGION_CHANNEL_SEND = 0,             // C   x rn_channel_send_t
    RN_REGION_CHANNEL_RECEIVE,              // C   x rn_channel_receive_t
    RN_REGION_SEND_NUMBERS,                 // W   x rn_send_number_t
    RN_REGION_RECEIVE_DESCRIPTORS,          // W   x rn_receive_descriptor_t
    RN_REGION_RECEIVED_BITS,                // W   bits
    RN_REGION_HELD_INDEX,                   // 2W  x rn_held_entry_t
    RN_REGION_RECEIVE_RING,                 // B   bytes
    RN_REGION_SEND_DESCRIPTORS,             // Q   x rn_send_descriptor_t
    RN_REGION_SEND_STORE,                   // S   bytes, three slab classes
    RN_REGION_SENT_PACKETS,                 // P   x rn_sent_packet_t
    RN_REGION_SENT_INDEX,                   // 2P  x rn_sent_index_entry_t
    RN_REGION_SENT_REFS,                    // 16P x uint32_t
    RN_REGION_DELIVERIES,                   // W + Du x rn_delivery_t
    RN_REGION_SEQUENCED_HOLDS,              // Ru  x rn_sequenced_hold_t
    RN_REGION_RECEIPTS,                     // Rq  x rn_receipt_t
    RN_REGION_NUM_PARTS
};

#define RN_REGION_ALIGNMENT                     ROCKETNET_ALLOCATION_ALIGNMENT

// the send store's three classes (section 4): fixed chunks of 128, 512 and
// the block stride, each class a third of S by bytes
#define RN_SEND_STORE_NUM_CLASSES               3
#define RN_SEND_STORE_NONE                      0xFFFFFFFFu

struct rn_region_layout_t
{
    // the configuration the layout was computed from, validated
    int num_channels;                       // C
    int window_messages;                    // W
    int receive_ring_bytes;                 // B
    int max_message_bytes;
    int send_buffer_bytes;                  // S
    int send_queue_messages;                // Q
    int packets_in_flight;                  // P
    int unreliable_deliveries;              // Du
    int sequenced_holds;                    // Ru
    int receipt_ring;                       // Rq

    // each part's count of entries, bytes, and 64-aligned offset in the region
    uint64_t part_entries[RN_REGION_NUM_PARTS];
    uint64_t part_bytes[RN_REGION_NUM_PARTS];
    uint64_t part_offset[RN_REGION_NUM_PARTS];

    // the send store's geometry, offsets inside the store part
    int store_chunk_bytes[RN_SEND_STORE_NUM_CLASSES];
    int store_chunks[RN_SEND_STORE_NUM_CLASSES];
    uint64_t store_class_offset[RN_SEND_STORE_NUM_CLASSES];
    uint64_t store_links_offset;            // the 1152 class's chain links, inside its own third

    uint64_t formula_bytes;                 // section 14's formula, unpadded
    uint64_t region_bytes;                  // exact: every part at 64-byte alignment; a multiple of 64
};

// Validates the configuration (the table's ranges, then section 14's cross
// rules) and computes the layout. NULL config means the defaults. Refuses
// loudly, never clamps: false, with the reason logged. Allocation-free.
bool rn_region_layout( const struct rocketnet_message_config_t * config, struct rn_region_layout_t * layout );

// the message table's field descriptors (rocketnet_config.cpp)
const struct rocketnet_config_field_t * rocketnet_message_config_fields( int * num_fields );

// ----------------------------------------------------------------
//                            the send store
// ----------------------------------------------------------------

// A slab of S bytes in three classes of fixed chunks, each with an intrusive
// free list: alloc and free are O(1), there is no fragmentation and no
// coalescing (section 4). A message of up to 1152 bytes takes one chunk of
// the smallest class that fits; a larger one is a block and takes
// ceil(bytes / 1152) chunks of the 1152 class, chained, one per fragment
// (section 8). A send the store cannot satisfy is refused whole and counted.
// A chunk handle is the class in the top two bits and the index below.

struct rn_send_store_t
{
    uint8_t * chunk_base[RN_SEND_STORE_NUM_CLASSES];
    uint32_t * links;                       // the 1152 class: free list and block chain, one per chunk
    uint32_t free_head[RN_SEND_STORE_NUM_CLASSES];
    int free_count[RN_SEND_STORE_NUM_CLASSES];
    int chunks[RN_SEND_STORE_NUM_CLASSES];
    int chunk_bytes[RN_SEND_STORE_NUM_CLASSES];
    int max_message_bytes;
    uint64_t * counters;                    // the owner's counter values, or NULL
};

// one chunk for a message of 1..1152 bytes; RN_SEND_STORE_NONE when the
// class is empty (counted) or bytes is outside that range (not counted: a
// bad argument is the caller's refusal to name)
uint32_t rn_send_store_alloc( struct rn_send_store_t * store, int bytes );

// a block of 1153..max_message_bytes bytes: ceil(bytes / 1152) chained
// chunks of the 1152 class, all or none; returns the first chunk, or
// RN_SEND_STORE_NONE (counted when the class has too few free)
uint32_t rn_send_store_alloc_block( struct rn_send_store_t * store, int bytes );

// frees a chunk from rn_send_store_alloc, or a whole block from its first chunk
void rn_send_store_free( struct rn_send_store_t * store, uint32_t chunk );

uint8_t * rn_send_store_chunk_data( const struct rn_send_store_t * store, uint32_t chunk );

int rn_send_store_chunk_bytes( const struct rn_send_store_t * store, uint32_t chunk );

// the next chunk in a block's chain, RN_SEND_STORE_NONE after the last
uint32_t rn_send_store_block_next( const struct rn_send_store_t * store, uint32_t chunk );

// ----------------------------------------------------------------
//                       one connection's region
// ----------------------------------------------------------------

struct rn_region_t
{
    uint8_t * base;
    struct rn_region_layout_t layout;

    // the typed carve-outs
    struct rn_channel_send_t * channel_send;
    struct rn_channel_receive_t * channel_receive;
    struct rn_send_number_t * send_numbers;
    struct rn_receive_descriptor_t * receive_descriptors;
    uint64_t * received_bits;
    struct rn_held_entry_t * held_index;
    uint8_t * receive_ring;
    struct rn_send_descriptor_t * send_descriptors;
    struct rn_sent_packet_t * sent_packets;
    struct rn_sent_index_entry_t * sent_index;
    uint32_t * sent_refs;
    struct rn_delivery_t * deliveries;
    struct rn_sequenced_hold_t * sequenced_holds;
    struct rn_receipt_t * receipts;

    struct rn_send_store_t store;

    // the send descriptor pool: a free list through rn_send_descriptor_t::next
    uint32_t descriptor_free_head;
    int descriptor_free_count;
};

// Carves a region at connect, inside memory the owner allocated at create
// (layout->region_bytes of it, 64-byte aligned): points every carve-out,
// zeroes the fixed tables, and rebuilds the store's free lists and the
// descriptor pool. Allocation-free; O(region). The send store and the
// receive ring are not cleared: nothing reads them before writing them.
// counters may be NULL.
void rn_region_carve( struct rn_region_t * region, const struct rn_region_layout_t * layout, void * memory, uint64_t * counters );

// a send descriptor from the pool, or RN_SEND_STORE_NONE, counted as MESSAGE_SEND_QUEUE_FULL
uint32_t rn_region_alloc_descriptor( struct rn_region_t * region );

void rn_region_free_descriptor( struct rn_region_t * region, uint32_t descriptor );

// true when [p, p + bytes) lies inside the region
bool rn_region_contains( const struct rn_region_t * region, const void * p, uint64_t bytes );

// ----------------------------------------------------------------
//                         the owner's arena
// ----------------------------------------------------------------

// One rocketnet_malloc for all of an owner's regions, at create: a server
// passes max_clients, a client 1. Nothing else allocates until destroy.
struct rn_arena_t
{
    void * context;
    uint8_t * memory;
    int num_regions;
    struct rn_region_layout_t layout;
};

// false, with nothing allocated, when the config is refused, the total
// overflows, or the allocation fails
bool rn_arena_create( struct rn_arena_t * arena, void * context, const struct rocketnet_message_config_t * config, int num_regions );

void rn_arena_destroy( struct rn_arena_t * arena );

// region i's memory, layout.region_bytes long and 64-byte aligned
void * rn_arena_region_memory( const struct rn_arena_t * arena, int region_index );

#endif // #ifndef ROCKETNET_ALLOCATOR_H
