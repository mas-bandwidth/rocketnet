/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_ALLOCATOR_H
#define ROCKETNET_SDK_ROCKETNET_ALLOCATOR_H

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <stddef.h>
#include <float.h>

// Layer 1 of the message layer: the region (docs/CHANNELS.md 4, sized by 14).
//
// One fixed region per connection: per server slot, and the client's one.
// An arena is the one block an object holds from create, with its regions
// (max_clients for a server, 1 for a client): the one rocketnet_malloc the
// create makes, or a block the caller supplies. Nothing in it is allocated,
// freed or grown again until destroy. A region is carved at connect -- every
// table reset, every free list threaded -- in its own memory, allocating
// nothing. Every queue, window, buffer and block the channel layers need is
// a typed carve-out of the region, sized from the connection's config by
// section 14's formula.
//
// Exhaustion is a counted refusal (MESSAGE_SEND_STORE_FULL,
// MESSAGE_SEND_QUEUE_FULL), never growth, and it ends the connection: the
// region takes a sticky exhausted state with the reason
// ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED, the owner's update reads
// rn_connection_memory_decide and disconnects that connection (a server
// kicks the slot, a client disconnects itself), and rn_region_reset gives
// the slot's next client a fresh region (tla/RNConnectionMemory.tla).
//
// A region is one connection's, under the one-writer-per-connection rule
// (16.1): nothing here locks. The counters it refuses into are its owner's,
// shared by all of an object's regions, so refusals add atomically.

// ----------------------------------------------------------------
//                          the entry types
// ----------------------------------------------------------------

// Each is pinned to section 14's working figure by a static assert in
// rocketnet_allocator.cpp, so the formula and the code cannot drift.

#define RN_DESCRIPTOR_NONE      0xFFFFFFFFu
#define RN_CHUNK_NONE           0xFFFFFFFFu
#define RN_RESEND_NONE          0xFFFFFFFFu

struct rn_channel_send_t                // 40 bytes (7.1): ready membership is a ticket plus the head generation, linked by channel index
{
    uint32_t ordered_write;             // next ordering index for an ordered send
    uint32_t sequenced_write;           // next sequencing index; zeroed by every ordered send
    uint32_t queue_head, queue_tail;    // the first and last descriptor of this channel's reliable FIFO, linked by queue_next; RN_DESCRIPTOR_NONE when empty
    uint16_t queued[4];                 // live queued count per priority (holes are not counted)
    uint32_t ready_prev, ready_next;    // the neighbouring ready channels, RN_DESCRIPTOR_NONE at the ends and when not ready
    uint32_t ready_ticket;              // 0 when this channel is not on the ready list; else its membership ticket
    uint32_t ready_generation;          // generation of the live queue head when this channel joined the ready list
};

struct rn_channel_receive_t             // 24 bytes (7.1)
{
    uint32_t ordered_read;              // next ordering index to deliver
    uint32_t sequenced_watermark;       // lowest sequencing index still deliverable in this epoch
    uint32_t held_sequenced;            // descriptor of the newest held future sequenced message, or RN_DESCRIPTOR_NONE
    uint32_t held_sequenced_first;      // the first number of that hold when it is code 4
    uint32_t block_in_turn;             // the in-turn code-4 block still collecting fragments, or RN_DESCRIPTOR_NONE
    uint32_t block_in_turn_first;       // that block's first number
};

struct rn_send_number_t                 // 32 bytes: the sender's per-number table, indexed number % W (6.1)
{
    uint64_t number;                    // the message number, reconstructed u64
    uint64_t ring_start;                // absolute ring position of this fragment
    double last_send;                   // RN_SEND_NUMBER_NEVER_SENT: never sent; else the last send time (>= 0.0), or a negative failed-send sentinel
    uint32_t descriptor;                // the send descriptor, or RN_DESCRIPTOR_NONE
    uint16_t fragment;                  // index within its block
    uint8_t ring_units;                 // ring span in 16-byte units: ring_end = ring_start + 16 * ring_units
    uint8_t acked;
};

// A real send time is >= 0.0, so no real send can equal this sentinel (or a
// negative sentinel). Whether a number has been sent is never inferred from a
// send time a real send can have (RNResendTiming!Due reads lastSend as a real
// tick, 0 included).
#define RN_SEND_NUMBER_NEVER_SENT   ( -DBL_MAX )

// the number was never sent
static inline bool rn_send_number_never_sent( double last_send ) { return last_send == RN_SEND_NUMBER_NEVER_SENT; }

// unsent (never sent) or a failed send (negative sentinel): due now as a first
// write, in the same class's FIFO
static inline bool rn_send_number_unsent_due( double last_send ) { return last_send < 0.0; }

struct rn_receive_descriptor_t          // 32 bytes: the receiver's descriptors, indexed number % W (6.1)
{
    uint64_t number;
    uint32_t ordering;
    uint32_t sequencing;
    uint32_t ring;                      // offset in the receive ring
    uint32_t bytes;
    uint16_t channel;
    uint8_t code;
    uint8_t live;
    uint16_t block_have;                // fragments received, on a block's first-number descriptor
    uint16_t block_count;
};

struct rn_held_entry_t                  // 16 bytes: the held-ordered index, 2W open-addressed entries (6.1)
{
    uint64_t key;                       // channel << 32 | ordering
    uint32_t number_slot;               // the descriptor slot holding it
    uint32_t used;
};

struct rn_send_descriptor_t             // 80 bytes: one queued or in-flight send (Q of them)
{
    uint32_t chunk;                     // the first store chunk of a block's contiguous run
    uint32_t bytes;
    uint32_t serial;                    // the receipt serial (7.4)
    uint32_t next;                      // retired queue link: always RN_DESCRIPTOR_NONE while queued; flat channel/unreliable FIFOs order live descriptors by enqueue_ticket over this array
    uint32_t ordering;
    uint32_t sequencing;
    uint64_t first_number;              // the first message number, once numbered (6.2)
    uint16_t channel;
    uint8_t reliability;
    uint8_t priority;
    uint8_t in_use;                     // the slot's live bit: 1 while the descriptor is held
    uint8_t flags;                      // RN_SEND_FLAG_*
    uint16_t fragments;
    uint16_t fragments_acked;
    uint16_t schedule_flags;            // RN_SCHEDULE_QUEUED while this descriptor is a live queue entry
    uint8_t reserved[2];                // unused spare bytes
    uint32_t enqueue_ticket;            // 0 not queued; else the enqueue ticket (from 1)
    uint32_t queue_next;                // the next descriptor in its channel's reliable FIFO, RN_DESCRIPTOR_NONE at the tail
    uint32_t order_slot;                // the flat live-span order array's slot at this descriptor's index: the descriptor at that order position, RN_DESCRIPTOR_NONE outside the live segment
    uint32_t generation;                // the slot's generation tag: bumped on every take, never reset by a free
    uint32_t span_start;                // RN_SEND_FLAG_SPAN: the span's byte offset in the send spans (4)
    uint32_t span_ticket;               // RN_SEND_FLAG_SPAN: the span's allocation ticket (order among live spans)
    uint32_t span_pos;                  // RN_SEND_FLAG_SPAN: this span's position in the order array, RN_DESCRIPTOR_NONE when not a live span
    uint32_t free_slot;                 // the flat descriptor free stack's slot at this descriptor's index: the descriptor at that stack depth
};

// a send descriptor's flags

#define RN_SEND_FLAG_SPAN       0x01    // the message lives in one contiguous span of the send spans, not in store chunks
#define RN_SEND_FLAG_RECEIPT_PROMOTED 0x02 // a large public type 5 uses reliable wire code 2 and receives one ACKED outcome
#define RN_SEND_FLAG_RECEIPT_RELIABLE 0x04 // public type 6 or 7 uses reliable wire code 2 or 3 and receives one ACKED outcome
#define RN_SEND_FLAG_CONTROL    0x08    // a layer control record (CONFIG, docs/CHANNELS.md 10.3): code 2 with CONTROL set, never an application message

#define RN_SCHEDULE_QUEUED      0x0001u

struct rn_sent_packet_t                 // 32 bytes: the sent-packet ring, P entries (6.4)
{
    uint64_t packet_id;
    double send_time;
    uint32_t ref_first;                 // first of this packet's references in the reference ring
    uint16_t ref_count;
    uint16_t unreliable_receipts;
    uint32_t receipt_first;
    uint32_t flags;
};

struct rn_sent_index_t                  // 16 bytes: packet_id -> sent ring entry, 2P open-addressed entries (6.1)
{
    uint64_t packet_id;
    uint32_t entry;
    uint32_t used;
};

struct rn_delivery_t                    // 24 bytes: one delivered message, valid until the next update (10.1)
{
    union
    {
        const uint8_t * data;           // into the payload (zero copy) or the receive ring
        uint64_t data_bits;             // pins the field at 8 bytes on a 32-bit target too
    };
    int32_t bytes;
    uint16_t channel;
    uint8_t code;
    uint8_t pad;
    uint32_t serial;
    uint32_t descriptor;                // the receive descriptor slot to release when the update ends, or RN_DESCRIPTOR_NONE (7.2, 8)
};

struct rn_sequenced_hold_t              // 16 hot bytes: metadata for the newest unreliable sequenced hold (7.3)
{
    uint32_t bytes;
    uint32_t ordering;
    uint32_t sequencing;
    uint16_t channel;
    uint8_t used;
    uint8_t code;
};

// Cold bytes live in a separate array, indexed by the same hold slot. Scans
// and view expiry touch only the compact metadata array above.
struct rn_sequenced_payload_t
{
    uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
};

struct rn_receipt_t                     // 16 bytes: the receipt ring, Rq entries (7.4)
{
    uint32_t serial;
    int32_t status;                     // ROCKETNET_RECEIPT_STATUS_*
    uint32_t reserved[2];
};

struct rn_unreliable_entry_t            // 8 bytes: one unreliable queue entry (tla/RNIndexRing.tla)
{
    uint32_t descriptor;                // the queued send descriptor
    uint32_t ticket;                    // its enqueue ticket when pushed; a tombstone once the descriptor's ticket differs
};

struct rn_resend_link_t                 // 12 bytes: resend sequence entry, indexed number % W (6.4)
{
    uint32_t prev;                      // previous slot in its sequence in bits 0..30 (0x7FFFFFFF: the head), linked flag in bit 31; 0 when not in a sequence
    uint32_t next;                      // next slot in its sequence in bits 0..30 (0x7FFFFFFF: the tail), chunk-class flag in bit 31
    uint32_t generation;                // descriptor generation named by this entry; 0 is never a live name
};

// Queue scheduling (tla/RNChannelQueues.tla). A descriptor's enqueue ticket is
// unique on the connection until the counter wraps, and the flat descriptor
// array ordered by that ticket is the channel and unreliable FIFO storage: a
// hole is a ticketless entry an interior cancel leaves in place, and compact
// restamps the live entries of one channel in one pass over its entries once
// holes pass the bound, or once per write pass, never per emitted record.
// Resend capacity stays W: one slot is in at most one resend sequence, linked
// to its neighbours by slot index in rn_resend_link_t, and an acked hole stays
// sequenced until a named removal so a resend walk still counts it.

// ----------------------------------------------------------------
//                        the region's layout
// ----------------------------------------------------------------

enum
{
    RN_PART_CHANNEL_SEND = 0,           // 40 C
    RN_PART_CHANNEL_RECEIVE,            // 24 C (the printed part tag stays 16C; the byte total uses sizeof)
    RN_PART_SEND_NUMBERS,               // 32 W
    RN_PART_SEND_FIRST_SENDS,           // 8 W
    RN_PART_RECEIVE_DESCRIPTORS,        // 32 W
    RN_PART_RECEIVED_BITS,              // W / 8
    RN_PART_HELD_INDEX,                 // 16 * 2W
    RN_PART_RECEIVE_RING,               // B
    RN_PART_SEND_DESCRIPTORS,           // 80 Q
    RN_PART_SEND_STORE,                 // S
    RN_PART_SENT_PACKETS,               // 32 P
    RN_PART_SENT_INDEX,                 // 16 * 2P
    RN_PART_SENT_REFS,                  // 4 * 16P
    RN_PART_DELIVERIES,                 // 24 (W + Du)
    RN_PART_SEQUENCED_HOLDS,            // 1440 Ru (16 hot + 1416 payload + 4 free-stack + 4 delivered list per hold)
    RN_PART_RECEIPTS,                   // 16 Rq
    RN_PART_RESEND_FIFO,                // 16 W
    RN_PART_UNRELIABLE_RING,            // 8 * 2Q
    RN_PART_SEND_SPANS,                 // B
    RN_NUM_PARTS
};

#define RN_STORE_SMALL          0
#define RN_STORE_MEDIUM         1
#define RN_STORE_LARGE          2
#define RN_STORE_NUM_CLASSES    3

#define RN_STORE_SMALL_BYTES    128
#define RN_STORE_MEDIUM_BYTES   512
#define RN_STORE_LARGE_BYTES    ROCKETNET_MESSAGE_FRAGMENT_BYTES

struct rn_region_layout_t
{
    uint64_t offset[RN_NUM_PARTS];      // from the region base, each 64-aligned
    uint64_t bytes[RN_NUM_PARTS];       // the formula's term, exactly
    uint64_t formula_bytes;             // the sum of the terms: section 14's figure
    uint64_t region_bytes;              // the exact region: the formula plus each part's alignment padding
    uint32_t store_chunks[RN_STORE_NUM_CLASSES];
};

// false, with a log line naming the rule, for a refused config. NULL is the defaults.
bool rn_region_layout( const rocketnet_message_config_t * config, rn_region_layout_t * layout );

#if ROCKETNET_DEV
const char * rn_region_part_name( int part );
#endif // #if ROCKETNET_DEV

// the message table's field descriptors, as the transports' (rocketnet_config.h)
struct rocketnet_config_field_t;
const struct rocketnet_config_field_t * rocketnet_message_config_fields( int * num_fields );

// ----------------------------------------------------------------
//                            the send store
// ----------------------------------------------------------------

// S bytes in three classes of fixed chunks, each class a 64-aligned third of
// S by bytes (4). A class's third holds its chunks, then one u32 of
// generation-tagged metadata per chunk (the class's meta array): a live bit,
// a block-head bit, the slot's generation tag and a count field -- the
// physical run length at a block's head chunk, the chunk's distance back to
// its head at an interior chunk. A block takes one contiguous run of chunks,
// so its chunks are addressed by arithmetic on the head, never by a stored
// reference to another chunk, and no byte outside S is spent on them. The
// live and head bits make a double free a refusal, never a corrupted pool
// (CHANNELS section 4). Freeing a block is O(its run length): it scans only
// the block's own run, never a numeric gap. Free chunks are found by a scan
// of the meta array's live bits from the class's scan cursor, so no free
// chain is stored either.
//
// A message up to 1152 bytes takes one chunk of the smallest class that
// fits, and only that class; a larger one is a block and takes
// ceil(bytes / 1152) large chunks from one contiguous run, all or none.
//
// A handle is the class in its top two bits and the chunk index below.

// The pool is lazy (tla/RNLazyPool.tla): chunks [0, fresh) have been
// taken at least once since the init and are free or held; chunks
// [fresh, num_chunks) were never taken, and neither their meta nor their
// bytes are written or read until they are. A take scans the meta array's
// live bits from free_head for a contiguous free run, without touching the
// pages of chunks it does not take.
struct rn_store_class_t
{
    uint8_t * data;
    uint32_t * meta;                    // one generation-tagged u32 per chunk, after the class's chunks
    uint32_t chunk_bytes;
    uint32_t num_chunks;
    uint32_t free_head;                 // the free-run scan cursor: the first chunk index a take examines
    uint32_t free_count;
    uint32_t fresh;
    uint32_t block_tag_next;            // the next generation tag a multi-chunk block's take assigns to all its chunks
};

struct rn_send_store_t
{
    rn_store_class_t classes[RN_STORE_NUM_CLASSES];
};

uint32_t rn_send_store_chunks_for( uint64_t store_bytes, int class_index );

int rn_send_store_class_for( int bytes );

uint32_t rn_send_store_chunks_needed( int bytes );

void rn_send_store_init( rn_send_store_t * store, uint8_t * memory, uint64_t bytes );

// 1 when rn_send_store_alloc of `bytes` would take its chunks now: the one
// test the allocation itself makes (tla/RNSendStore.tla Alloc's guard); pure
bool rn_send_store_fits( const rn_send_store_t * store, int bytes );

uint32_t rn_send_store_alloc( rn_send_store_t * store, int bytes );     // the first chunk's handle, or RN_CHUNK_NONE

bool rn_send_store_free_checked( rn_send_store_t * store, uint32_t handle );     // false: not an allocated block's head, nothing changed

void rn_send_store_free( rn_send_store_t * store, uint32_t handle );    // the whole block

uint8_t * rn_send_store_chunk_data( const rn_send_store_t * store, uint32_t handle );

#if ROCKETNET_DEV
uint32_t rn_send_store_chunk_next( const rn_send_store_t * store, uint32_t handle );
#endif // #if ROCKETNET_DEV

inline int rn_send_store_chunk_class( uint32_t handle ) { return (int) ( handle >> 30 ); }

#if ROCKETNET_DEV
uint32_t rn_send_store_free_chunks( const rn_send_store_t * store, int class_index );

uint32_t rn_send_store_total_chunks( const rn_send_store_t * store, int class_index );

// the chunk slot's current generation tag, or 0 for a handle the store did
// not issue (a stale handle's tag never matches a live slot's next take)
uint32_t rn_send_store_chunk_generation( const rn_send_store_t * store, uint32_t handle );
#endif // #if ROCKETNET_DEV

// ----------------------------------------------------------------
//                              the region
// ----------------------------------------------------------------

struct rn_region_t
{
    uint8_t * base;
    uint64_t bytes;
    rocketnet_counters_t * counters;    // the owner's; refusals count here
    uint64_t carves;                    // connections this region has carried

    uint32_t num_channels;              // C
    uint32_t window;                    // W
    uint32_t ring_bytes;                // B
    uint32_t queue;                     // Q
    uint32_t packets;                   // P
    uint32_t held_entries;              // 2W
    uint32_t sent_index_entries;        // 2P
    uint32_t sent_ref_entries;          // 16P
    uint32_t delivery_entries;          // W + Du
    uint32_t holds;                     // Ru
    uint32_t receipt_entries;           // Rq
    uint32_t max_message_bytes;

    rn_channel_send_t * channel_send;
    rn_channel_receive_t * channel_receive;
    rn_send_number_t * send_numbers;
    double * send_first_sends;          // each number's first send time, indexed number % W like send_numbers; valid once its last_send is not RN_SEND_NUMBER_NEVER_SENT
    rn_receive_descriptor_t * receive_descriptors;
    uint64_t * received_bits;
    rn_held_entry_t * held_index;
    uint8_t * receive_ring;
    rn_send_descriptor_t * send_descriptors;
    uint8_t * store_memory;
    uint64_t store_bytes;
    rn_send_store_t store;
    uint8_t * send_spans;               // the contiguous spans of messages above one store chunk (4)
    uint32_t span_bytes;                // SR, the spans' size
    uint32_t span_head;                 // the byte offset of the oldest live span's start (the reclaim floor)
    uint32_t span_tail;                 // the byte offset the next span is placed at
    uint32_t span_first, span_last;     // the oldest and the newest live span's descriptor, RN_DESCRIPTOR_NONE when none
    uint32_t span_ticket_next;          // the next span allocation ticket; the flat span order array orders live spans, tickets mirror that order
    uint32_t schedule_ticket_next;      // next enqueue ticket (from 1); 0 is never issued
    rn_sent_packet_t * sent_packets;
    rn_sent_index_t * sent_index;
    uint32_t * sent_refs;
    rn_delivery_t * deliveries;
    rn_sequenced_hold_t * sequenced_messages_held;
    rn_sequenced_payload_t * sequenced_message_payloads;
    uint32_t * sequenced_hold_free_stack;       // free hold indices in the holds part, the top at sequenced_hold_free_count - 1
    uint32_t sequenced_hold_free_count;         // holds on the free stack: taken in O(1), never scanned for
    uint32_t * sequenced_holds_delivered;       // delivered (used == 2) hold indices in the holds part, this update's
    uint32_t sequenced_holds_delivered_count;   // delivered holds the end-of-update pass returns, none when zero
    rn_receipt_t * receipts;
    rn_resend_link_t * resend_fifo;
    rn_unreliable_entry_t * unreliable_ring;    // 2 x queue entries: the unreliable FIFO in push order
    uint32_t unreliable_ring_entries;

    uint32_t descriptors_free_top;      // the descriptor free stack's depth: taken-then-freed handles on the stack, in the descriptors' free_slot array
    uint32_t descriptors_free;          // queue less the descriptors in use
    uint32_t descriptors_fresh;         // descriptors [0, fresh) have been taken since the carve; the rest are untouched (tla/RNLazyPool.tla)

    uint32_t exhausted;                 // sticky until reset: an operation found no room (RNConnectionMemory Exhaust)
    int32_t exhausted_reason;           // ROCKETNET_DISCONNECT_REASON_*: NONE, or MESSAGE_MEMORY_EXHAUSTED

    uint32_t send_room_armed;           // 1 until the send room watch reports a crossing, 1 again below half (rn_region_send_room_watch)
};

// The next enqueue ticket on this connection. 0 is never issued.

static inline uint32_t rn_region_schedule_ticket( rn_region_t * region )
{
    uint32_t ticket = region->schedule_ticket_next;
    if ( ticket == 0 )
        ticket = 1;

    uint32_t next = ticket + 1;
    if ( next == 0 )
        next = 1;

    region->schedule_ticket_next = next;

    return ticket;
}

static inline void rn_descriptor_enqueue_stamp( rn_region_t * region, rn_send_descriptor_t * descriptor )
{
    descriptor->enqueue_ticket = rn_region_schedule_ticket( region );
    descriptor->queue_next = RN_DESCRIPTOR_NONE;
    descriptor->schedule_flags = (uint16_t) RN_SCHEDULE_QUEUED;
}

static inline bool rn_descriptor_schedule_live( const rn_send_descriptor_t * descriptor )
{
    return descriptor->in_use && ( descriptor->schedule_flags & RN_SCHEDULE_QUEUED ) != 0 &&
           descriptor->enqueue_ticket != 0;
}

#if ROCKETNET_DEV
// Present only in the unit-test library; counts descriptors examined by
// rn_span_unlink's edge search, never production state.
struct rn_span_cost_t
{
    uint64_t edge_visits;
};
void rn_span_cost_reset();
rn_span_cost_t rn_span_cost_read();
#endif // #if ROCKETNET_DEV

// at connect: every table back to its empty state, every free list
// emptied (the descriptor pool and the store's classes are lazy,
// tla/RNLazyPool.tla) and the exhausted state cleared, in the region's own
// memory. Allocates nothing. The ring's, the store's and the pools' bytes
// are not cleared: nothing reads them before writing them.
void rn_region_carve( rn_region_t * region );

// the slot's region when its connection ends and before the slot is reused
// (RNConnectionMemory Reuse): the carve, so a reused slot starts exactly as
// a fresh one and nothing of the previous client is reachable.
void rn_region_reset( rn_region_t * region );

// one send's descriptor and store chunks, both or neither. The descriptor
// index, or RN_DESCRIPTOR_NONE: a full pool counts MESSAGE_SEND_QUEUE_FULL,
// a full store MESSAGE_SEND_STORE_FULL, and either exhausts the region
// (rn_region_exhaust). bytes outside 1..max_message_bytes is the caller's
// refusal to count, takes nothing here and is not exhaustion.
uint32_t rn_region_send_alloc( rn_region_t * region, int bytes );

// RNConnectionMemory Exhaust: an operation of this region's connection found
// no room for something it must not drop (a send, a receive ring placement,
// a hold). Sticky until rn_region_reset; the first call counts
// MESSAGE_MEMORY_EXHAUSTED on the owner's counters, later calls nothing.
void rn_region_exhaust( rn_region_t * region );

bool rn_region_exhausted( const rn_region_t * region );

#if ROCKETNET_DEV
int rn_region_exhausted_reason( const rn_region_t * region );     // ROCKETNET_DISCONNECT_REASON_*
#endif // #if ROCKETNET_DEV

// RNConnectionMemory Kick, the decision: pure, allocation-free, called by the
// owner every update. ROCKETNET_DISCONNECT_REASON_NONE, or the reason to end
// this connection with: a server disconnects the slot
// (rocketnet_server_transport_disconnect_client) and resets its region; a
// client disconnects itself (rocketnet_client_transport_disconnect). An
// exhausted connection takes no further room before the kick: both send
// allocators refuse (rn_region_send_alloc and rn_region_send_alloc_span in sdk/rocketnet_allocator.cpp) and a payload is taken no further
// (rn_ordered_receive in sdk/rocketnet_channel.cpp). Room can still come back:
// an ack (rn_endpoint_on_acked in sdk/rocketnet_endpoint.cpp) or an abort frees
// through rn_region_send_free, and nothing takes that room again before the
// reset (RNConnectionMemory Release, allowed while exhausted).
int rn_connection_memory_decide( const rn_region_t * region );

void rn_region_send_free( rn_region_t * region, uint32_t descriptor );

// ----------------------------------------------------------------
//                      the send room watch
// ----------------------------------------------------------------

// The send room is five pools a start message takes from: the send queue's
// descriptors (send_queue_messages), each of the store's three chunk classes
// (send_message_buffer_bytes), and the send spans (receive_message_buffer_bytes). A pool fills
// as start messages take room and empties as the receiver acks them. Running one
// out ends the connection (R5), so a debug build warns before it does.

#define RN_SEND_ROOM_WARN_PERMILLE      750     // a crossing: the fullest pool reaches 75%
#define RN_SEND_ROOM_REARM_PERMILLE     500     // the watch rearms once every pool is below 50%

#define RN_SEND_ROOM_NONE               0
#define RN_SEND_ROOM_CROSSED            1

struct rn_send_room_t
{
    uint64_t store_used_bytes;          // the store's chunks in use, in bytes
    uint64_t store_bytes;               // all of the store's chunks, in bytes
    uint64_t span_used_bytes;           // the send spans from the oldest live span to the tail, padding included
    uint64_t span_bytes;                // SR
    uint32_t used_messages;             // the send queue's descriptors in use
    uint32_t messages;                  // Q
    uint32_t permille;                  // the fullest pool's use, in thousandths
};

// the region's send room now; pure, allocation-free
void rn_region_send_room( const rn_region_t * region, rn_send_room_t * room );

// 1 when a start message of `bytes` bytes would be given room now, so it
// would not exhaust the region; pure. The pool it would take from (the
// store class for up to one block stride, the send spans above it): its free
// bytes and its size, and the free descriptors; each out pointer may be NULL.
int rn_region_send_fits( const rn_region_t * region, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );

// The watch's decision, called around every start message:
// RN_SEND_ROOM_CROSSED once when the fullest pool first reaches
// RN_SEND_ROOM_WARN_PERMILLE, then RN_SEND_ROOM_NONE until every pool is
// below RN_SEND_ROOM_REARM_PERMILLE again. A connection that stays above the
// warning crosses once.
int rn_region_send_room_watch( rn_region_t * region );

// The debug-build warnings, through the log sink at ROCKETNET_LOG_LEVEL_WARN:
// a crossing (rn_region_send_room_watch) and an exhaustion just before the
// kick, each naming the client or the server slot, the room used against its
// limits and the config fields to raise. A release build compiles both to
// nothing; rn_send_room_warnings is 1 when they are compiled in.
extern const int rn_send_room_warnings;
void rn_region_send_room_note( rn_region_t * region, int is_server, int index );
void rn_region_send_room_exhausted( const rn_region_t * region, int is_server, int index );

// The largest message that takes a store chunk is one block stride, 1152 bytes.
// A message above it takes a contiguous span (rn_region_send_alloc_span): the
// application writes it once, in place, and every fragment is copied from it
// into a frame (4, 8).
#define RN_SPAN_MIN_BYTES       ( RN_STORE_LARGE_BYTES + 1 )

// One send's descriptor and a contiguous span of the send spans, both or
// neither, for a message of 1 to max_message_bytes bytes. The span is placed
// as RNRing places a holdable record (tla/RNRing.tla, PlaceSingle): at the
// tail, or at the start of the ring when the tail leaves too little room
// before the end, and admitted only when it stays clear of the oldest live
// span's start. Spans are freed in any order; the head moves to the oldest
// live span. A refusal counts MESSAGE_SEND_STORE_FULL (or QUEUE_FULL) and
// exhausts the region like rn_region_send_alloc (R5). The descriptor index, or
// RN_DESCRIPTOR_NONE.
uint32_t rn_region_send_alloc_span( rn_region_t * region, int bytes );

// ----------------------------------------------------------------
//                      the send room decision
// ----------------------------------------------------------------

// The one decision a send's room is taken by. rn_region_send_alloc and
// rn_region_send_alloc_span act on it, and the public room query
// (rocketnet_client_can_send_message, rocketnet_server_can_send_message)
// reads it, so the query and the send never disagree. Pure, allocation-free,
// O(1). Checked in the allocation's order: the size, then the sticky
// exhaustion, then a free descriptor, then the chunks or the span.
#define RN_SEND_FIT_OK              0   // room: the allocation takes it
#define RN_SEND_FIT_REFUSED         1   // bytes outside the pool's range: the caller's refusal, not exhaustion
#define RN_SEND_FIT_EXHAUSTED       2   // the region is already exhausted (sticky until reset)
#define RN_SEND_FIT_QUEUE_FULL      3   // no free send descriptor: the allocation exhausts the region
#define RN_SEND_FIT_STORE_FULL      4   // no chunks, or no span placement: the allocation exhausts the region

int rn_region_send_fit_store( const rn_region_t * region, int bytes );
int rn_region_send_fit_span( const rn_region_t * region, int bytes, uint32_t * out_start );

// A start message's room: a store chunk up to one block stride, a span above
// it. rn_region_send_take allocates it (rn_region_send_alloc or
// rn_region_send_alloc_span) and rn_region_send_fit decides it, by the same
// size rule.

inline bool rn_region_send_is_span( int bytes )
{
    return bytes >= (int) RN_SPAN_MIN_BYTES;
}

int rn_region_send_fit( const rn_region_t * region, int bytes );
uint32_t rn_region_send_take( rn_region_t * region, int bytes );

// The bytes of fragment `fragment` of a send, and their length: from the span
// at fragment * F, or from the single store chunk. NULL for a descriptor not in
// use or a fragment outside the send.
const uint8_t * rn_region_send_data( const rn_region_t * region, uint32_t descriptor, uint32_t fragment, uint32_t * out_bytes );

// The writable start of a send's bytes: the span's start, or the store chunk.
uint8_t * rn_region_send_write_pointer( rn_region_t * region, uint32_t descriptor );

// frees one fragment's store chunk from an in-use send descriptor's block,
// and returns the descriptor itself to Q when its last fragment is acked (6.4).
void rn_region_send_free_fragment( rn_region_t * region, uint32_t descriptor, uint32_t chain_index );

#if ROCKETNET_DEV
bool rn_region_contains( const rn_region_t * region, const void * p, uint64_t bytes );
#endif // #if ROCKETNET_DEV

// the receiver's only ring logic (4.3): a placement is 16-aligned and inside B
bool rn_region_ring_span_valid( const rn_region_t * region, uint64_t offset, uint64_t bytes );

// ----------------------------------------------------------------
//                              the arena
// ----------------------------------------------------------------

struct rn_arena_t;

// the one allocation: num_regions regions, each laid out, pointed and
// carved. NULL (allocating nothing) for a refused config or count.
rn_arena_t * rn_arena_create( void * context, const rocketnet_message_config_t * config, int num_regions, rocketnet_counters_t * counters );

// the exact bytes of an arena of num_regions regions at config: its header,
// its region structs and its regions. 0 for a refused config or count.
uint64_t rn_arena_required_bytes( const rocketnet_message_config_t * config, int num_regions );

// the same arena in the caller's block, allocating nothing: block must be
// ROCKETNET_ALLOCATION_ALIGNMENT-aligned and block_bytes at least
// rn_arena_required_bytes. A short, misaligned or NULL block is refused
// (NULL), never truncated. Destroy then frees nothing; the block stays the
// caller's, and must outlive the arena.
rn_arena_t * rn_arena_create_in_block( void * block, uint64_t block_bytes, const rocketnet_message_config_t * config, int num_regions, rocketnet_counters_t * counters );

void rn_arena_destroy( rn_arena_t * arena );

#if ROCKETNET_DEV
int rn_arena_num_regions( const rn_arena_t * arena );
#endif // #if ROCKETNET_DEV

rn_region_t * rn_arena_region( rn_arena_t * arena, int index );

#if ROCKETNET_DEV
uint64_t rn_arena_bytes( const rn_arena_t * arena );     // the block's size: rn_arena_required_bytes for its config
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_ALLOCATOR_H
