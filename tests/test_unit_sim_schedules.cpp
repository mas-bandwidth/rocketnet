/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A catalog of the replayable fault schedules that shipped the night's bugs,
// each one a scripted list of steps run through the network simulator's
// ordered-list delivery. A schedule is a list of {deliver, drop, duplicate,
// reorder, advance time} steps composed into a prepared packet list and
// handed to rocketnet_network_simulator_deliver_packets; receive() returns
// the packets in that same order, so the scripted order is the delivered
// order and the invariant is asserted on the module the received bytes drive.
//
// The six schedules:
//   1. long skip then stragglers  -- the replay window, skipped by exactly
//      64, 255, 256, 4095, 4096, 4097 and 65536, then highest-k for k in
//      1..4096 accepted or refused as the model says (the ClearedWords rule).
//   2. final fragment first       -- reassembly with 16 live slots: the final
//      fragment stages first (a reorder step), an oversized count is refused,
//      no slot evicted.
//   3. batch cap                  -- delivered_capacity + 8 payloads complete
//      in one update: the capacity is delivered whole, the excess counts
//      BATCH_FULL, and the next update (an advance-time step) recovers.
//   4. same-tick slot reuse       -- [A disconnect, B response, A's stale
//      payload, A's stale ack] in one update: B never sees A's data.
//   5. sequence wrap              -- transport sequence seeded near 2^32 - 128
//      and the replay window near 2^24 - 128, advanced 512 packets across the
//      wrap.
//   6. unreliable aggregation     -- 65535, 65536 and max_datagram_bytes + 1
//      bytes at the unreliable send's two caps.

#include "test_unit_registry.h"

#include "rocketnet_network_simulator.h"

#include "rocketnet_replay.h"
#include "rocketnet_fragment.h"
#include "rocketnet_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_allocator.h"
#include "rocketnet_address.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// ---------------------------------------------------------------------------
// The schedule runner. The simulator is the wire; this is the hand that
// scripts it. Drop, duplicate and reorder are composed into the prepared list
// (rocketnet_network_simulator_deliver_packets); advance time moves the wire clock so
// the next flush's packets become due at a later delivery time.
// ---------------------------------------------------------------------------

#define SIM_SCHEDULE_MAX_PACKETS 8192

struct sim_schedule_t
{
    struct rocketnet_network_simulator_t * sim;
    struct rocketnet_address_t from;
    struct rocketnet_address_t to;
    double now;

    const uint8_t * packet[SIM_SCHEDULE_MAX_PACKETS];
    int bytes[SIM_SCHEDULE_MAX_PACKETS];
    int count;

    uint8_t rx[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_address_t rx_from;
};

static void sim_write_u64_le( uint8_t * out, uint64_t value )
{
    for ( int i = 0; i < 8; i++ )
    {
        out[i] = (uint8_t) ( value >> ( 8 * i ) );
    }
}

static uint64_t sim_read_u64_le( const uint8_t * in )
{
    uint64_t value = 0;
    for ( int i = 0; i < 8; i++ )
    {
        value |= ( (uint64_t) in[i] ) << ( 8 * i );
    }

    return value;
}

static void sim_schedule_begin( struct sim_schedule_t * s )
{
    memset( s, 0, sizeof( *s ) );
    s->sim = rocketnet_network_simulator_create( NULL, SIM_SCHEDULE_MAX_PACKETS, 1 );
    rocketnet_unit_check( s->sim != NULL );

    rocketnet_network_simulator_config( s->sim, 0.0, 0.0, 0.0f, 0.0f );
    rocketnet_unit_check( rocketnet_address_parse( &s->from, "198.51.100.1:10000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &s->to, "198.51.100.2:10000" ) );
}

static void sim_schedule_end( struct sim_schedule_t * s )
{
    rocketnet_network_simulator_destroy( s->sim );
}

static void sim_schedule_advance_time( struct sim_schedule_t * s, double dt )
{
    s->now += dt;
}

static void sim_schedule_deliver( struct sim_schedule_t * s, const uint8_t * data, int bytes )
{
    rocketnet_unit_check( s->count < SIM_SCHEDULE_MAX_PACKETS );
    rocketnet_unit_check( bytes > 0 && bytes <= ROCKETNET_RECEIVE_CEILING );

    s->packet[s->count] = data;
    s->bytes[s->count] = bytes;
    s->count++;
}

static void sim_schedule_drop( struct sim_schedule_t * s )
{
    rocketnet_unit_check( s->count > 0 );

    s->count--;
}

static void sim_schedule_duplicate( struct sim_schedule_t * s )
{
    rocketnet_unit_check( s->count > 0 );
    rocketnet_unit_check( s->count < SIM_SCHEDULE_MAX_PACKETS );

    s->packet[s->count] = s->packet[s->count - 1];
    s->bytes[s->count] = s->bytes[s->count - 1];
    s->count++;
}

static void sim_schedule_reorder( struct sim_schedule_t * s, int a, int b )
{
    rocketnet_unit_check( a >= 0 && a < s->count && b >= 0 && b < s->count );

    const uint8_t * d = s->packet[a];
    s->packet[a] = s->packet[b];
    s->packet[b] = d;
    int n = s->bytes[a];
    s->bytes[a] = s->bytes[b];
    s->bytes[b] = n;
}

static int sim_schedule_flush( struct sim_schedule_t * s )
{
    int delivered = rocketnet_network_simulator_deliver_packets( s->sim, &s->from, &s->to,
                                                                 s->packet, s->bytes, s->count, s->now );
    s->count = 0;
    return delivered;
}

static int sim_schedule_receive( struct sim_schedule_t * s )
{
    return rocketnet_network_simulator_receive( s->sim, &s->to, s->now, &s->rx_from, s->rx );
}

// ---------------------------------------------------------------------------
// The reassembly counter table: the standard's indices, in the process order
// the fragment engine fires them.
// ---------------------------------------------------------------------------

static const struct rocketnet_reassembly_counters_t sim_fragment_counters = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_FRAGMENT_STALE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL
};

// builds one fragment's type data -- header plus patterned bytes read from the
// position the fragment claims, so a reassembled payload is verifiable
// wherever its fragments came from

static int sim_build_fragment( uint8_t * type_data, uint32_t payload_id,
                               int fragment_index, int fragment_count,
                               int fragment_bytes, int stride, uint8_t pattern )
{
    rocketnet_write_fragment_header( type_data, payload_id, fragment_index, fragment_count );

    for ( int i = 0; i < fragment_bytes; i++ )
    {
        type_data[ROCKETNET_FRAGMENT_HEADER_BYTES + i] = (uint8_t) ( pattern + stride * fragment_index + i );
    }

    return ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes;
}

static void sim_check_reassembled( const uint8_t * payload, int payload_bytes, uint8_t pattern )
{
    for ( int i = 0; i < payload_bytes; i++ )
    {
        rocketnet_unit_check( payload[i] == (uint8_t) ( pattern + i ) );
    }
}

// ---------------------------------------------------------------------------
// 1. long skip then stragglers
// ---------------------------------------------------------------------------

static void test_sim_schedule_long_skip_stragglers()
{
    struct sim_schedule_t sched;
    sim_schedule_begin( &sched );

    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    const uint64_t skips[] = { 64, 255, 256, 4095, 4096, 4097, 65536 };
    uint64_t highest = 0;

    // deliver the seven skips: each packet carries the next high-water mark.
    // each packet needs its own buffer -- the queue stores pointers and the
    // wire copies them only at flush.

    uint8_t skip_packets[7][8];
    for ( int i = 0; i < (int) ( sizeof( skips ) / sizeof( skips[0] ) ); i++ )
    {
        highest += skips[i];
        sim_write_u64_le( skip_packets[i], highest );
        sim_schedule_deliver( &sched, skip_packets[i], 8 );
    }

    rocketnet_unit_check( sim_schedule_flush( &sched ) == 7 );

    for ( int i = 0; i < 7; i++ )
    {
        rocketnet_unit_check( sim_schedule_receive( &sched ) == 8 );

        rocketnet_replay_advance( &replay, sim_read_u64_le( sched.rx ) );
    }

    // replay accepts a sequence exactly once: a duplicated high-water mark is
    // refused twice, and a packet removed from the scripted order (drop)
    // never reaches the module.

    sim_schedule_deliver( &sched, skip_packets[6], 8 );
    sim_schedule_duplicate( &sched );
    rocketnet_unit_check( sim_schedule_flush( &sched ) == 2 );
    rocketnet_unit_check( sim_schedule_receive( &sched ) == 8 );
    rocketnet_unit_check( rocketnet_replay_already_received( &replay, sim_read_u64_le( sched.rx ) ) != 0 );
    rocketnet_unit_check( sim_schedule_receive( &sched ) == 8 );
    rocketnet_unit_check( rocketnet_replay_already_received( &replay, sim_read_u64_le( sched.rx ) ) != 0 );

    sim_schedule_deliver( &sched, skip_packets[6], 8 );
    sim_schedule_drop( &sched );
    rocketnet_unit_check( sim_schedule_flush( &sched ) == 0 );
    rocketnet_unit_check( sim_schedule_receive( &sched ) == 0 );

    // the stragglers arrive after a time step: highest-k for k in 1..4096.
    // the rule (ClearedWords = Words): after the final long skip the whole
    // window is clear except the high-water mark itself, so every sequence
    // highest-k for k in 1..4095 is genuine and accepted, while highest-4096
    // sits below the floor and is refused as too old.

    sim_schedule_advance_time( &sched, 1.0 );

    uint8_t straggler_packets[4096][8];
    for ( int idx = 0; idx < 4096; idx++ )
    {
        sim_write_u64_le( straggler_packets[idx], highest - (uint64_t) ( idx + 1 ) );
        sim_schedule_deliver( &sched, straggler_packets[idx], 8 );
    }

    rocketnet_unit_check( sim_schedule_flush( &sched ) == 4096 );

    for ( uint64_t k = 1; k <= 4095; k++ )
    {
        rocketnet_unit_check( sim_schedule_receive( &sched ) == 8 );
        rocketnet_unit_check( !rocketnet_replay_already_received( &replay, sim_read_u64_le( sched.rx ) ) );
    }

    rocketnet_unit_check( sim_schedule_receive( &sched ) == 8 );
    rocketnet_unit_check( rocketnet_replay_already_received( &replay, sim_read_u64_le( sched.rx ) ) != 0 );

    sim_schedule_end( &sched );
}

// ---------------------------------------------------------------------------
// 2. final fragment first, sixteen live slots
// ---------------------------------------------------------------------------

static void test_sim_schedule_final_fragment_first()
{
    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof( counters_storage ) );
    uint64_t * counters = counters_storage.values;
    const struct rocketnet_reassembly_counters_t * table = &sim_fragment_counters;

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 1, 16, 8192, 16 );
    rocketnet_unit_check( set != NULL );

    rocketnet_reassembly_set_begin_update( set );

    struct sim_schedule_t sched;
    sim_schedule_begin( &sched );

    uint8_t frag_a[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];
    uint8_t frag_b[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];

    // sixteen two-fragment payloads, final fragment first, one per slot: none
    // evicts another and every payload completes. the final fragment is
    // queued first and the reorder step is what stages it ahead of the first.

    for ( int id = 1; id <= 16; id++ )
    {
        int b0 = sim_build_fragment( frag_a, (uint32_t) id, 0, 2, 100, 100, (uint8_t) ( 0x40 + id ) );
        int b1 = sim_build_fragment( frag_b, (uint32_t) id, 1, 2, 40, 100, (uint8_t) ( 0x40 + id ) );
        sim_schedule_deliver( &sched, frag_a, b0 );
        sim_schedule_deliver( &sched, frag_b, b1 );
        sim_schedule_reorder( &sched, sched.count - 2, sched.count - 1 );
        rocketnet_unit_check( sim_schedule_flush( &sched ) == 2 );

        rocketnet_unit_check( sim_schedule_receive( &sched ) == b1 );
        rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b1, counters, table ) == 1 );
        rocketnet_unit_check( set->completed_bytes == 0 );

        rocketnet_unit_check( sim_schedule_receive( &sched ) == b0 );
        rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b0, counters, table ) == 1 );
        rocketnet_unit_check( set->completed_bytes == 140 );

        sim_check_reassembled( set->completed_payload, 140, (uint8_t) ( 0x40 + id ) );
    }

    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 16 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );

    // an oversized final-first payload: the final stages first, then a
    // non-final whose stride makes the reconstruction exceed the ceiling --
    // refused as too large, and nothing keeps the slot

    int b1 = sim_build_fragment( frag_b, 17, 7, 8, 40, ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES, 0x80 );
    sim_schedule_deliver( &sched, frag_b, b1 );
    rocketnet_unit_check( sim_schedule_flush( &sched ) == 1 );
    rocketnet_unit_check( sim_schedule_receive( &sched ) == b1 );
    rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b1, counters, table ) == 1 );

    int b0 = sim_build_fragment( frag_a, 17, 0, 8, ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES, ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES, 0x80 );
    sim_schedule_deliver( &sched, frag_a, b0 );
    rocketnet_unit_check( sim_schedule_flush( &sched ) == 1 );
    rocketnet_unit_check( sim_schedule_receive( &sched ) == b0 );
    rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b0, counters, table ) == 0 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] == 1 );

    sim_schedule_end( &sched );
    rocketnet_reassembly_set_destroy( set );
}

// ---------------------------------------------------------------------------
// 3. the batch cap
// ---------------------------------------------------------------------------

static void test_sim_schedule_batch_cap()
{
    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof( counters_storage ) );
    uint64_t * counters = counters_storage.values;
    const struct rocketnet_reassembly_counters_t * table = &sim_fragment_counters;

    // a delivery batch of four, so twelve completions in one update overflow
    // by eight

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 1, 16, 8192, 4 );
    rocketnet_unit_check( set != NULL );

    rocketnet_reassembly_set_begin_update( set );

    struct sim_schedule_t sched;
    sim_schedule_begin( &sched );

    uint8_t frag_a[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];
    uint8_t frag_b[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];

    // the first four complete inside the capacity and are all delivered

    for ( int id = 1; id <= 4; id++ )
    {
        int b0 = sim_build_fragment( frag_a, (uint32_t) id, 0, 2, 100, 100, 0x20 );
        int b1 = sim_build_fragment( frag_b, (uint32_t) id, 1, 2, 40, 100, 0x20 );
        sim_schedule_deliver( &sched, frag_a, b0 );
        sim_schedule_deliver( &sched, frag_b, b1 );
        rocketnet_unit_check( sim_schedule_flush( &sched ) == 2 );
        rocketnet_unit_check( sim_schedule_receive( &sched ) == b0 );
        rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b0, counters, table ) == 1 );
        rocketnet_unit_check( sim_schedule_receive( &sched ) == b1 );
        rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b1, counters, table ) == 1 );
    }

    rocketnet_unit_check( set->num_delivered == 4 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );

    // eight more in the same update overflow the batch and are counted, not
    // silently lost

    for ( int id = 5; id <= 12; id++ )
    {
        int b0 = sim_build_fragment( frag_a, (uint32_t) id, 0, 2, 100, 100, 0x20 );
        int b1 = sim_build_fragment( frag_b, (uint32_t) id, 1, 2, 40, 100, 0x20 );
        sim_schedule_deliver( &sched, frag_a, b0 );
        sim_schedule_deliver( &sched, frag_b, b1 );
        rocketnet_unit_check( sim_schedule_flush( &sched ) == 2 );
        rocketnet_unit_check( sim_schedule_receive( &sched ) == b0 );
        rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b0, counters, table ) == 1 );
        rocketnet_unit_check( sim_schedule_receive( &sched ) == b1 );
        rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b1, counters, table ) == 1 );
    }

    rocketnet_unit_check( set->num_delivered == 4 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 12 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 8 );

    // the next update opens a fresh batch and delivers again

    sim_schedule_advance_time( &sched, 1.0 );
    rocketnet_reassembly_set_begin_update( set );

    int b0 = sim_build_fragment( frag_a, 13, 0, 2, 100, 100, 0x20 );
    int b1 = sim_build_fragment( frag_b, 13, 1, 2, 40, 100, 0x20 );
    sim_schedule_deliver( &sched, frag_a, b0 );
    sim_schedule_deliver( &sched, frag_b, b1 );
    rocketnet_unit_check( sim_schedule_flush( &sched ) == 2 );
    rocketnet_unit_check( sim_schedule_receive( &sched ) == b0 );
    rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b0, counters, table ) == 1 );
    rocketnet_unit_check( sim_schedule_receive( &sched ) == b1 );
    rocketnet_unit_check( rocketnet_reassembly_process_fragment( set, 0, sched.rx, b1, counters, table ) == 1 );
    rocketnet_unit_check( set->num_delivered == 1 );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 8 );

    sim_schedule_end( &sched );
    rocketnet_reassembly_set_destroy( set );
}

// ---------------------------------------------------------------------------
// 4. same-tick slot reuse
// ---------------------------------------------------------------------------

static void sim_run_same_tick_slot_reuse( bool same_address )
{
    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    uint64_t connection_ids[4] = {};
    uint64_t acked_connection_ids[16] = {};

    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    server.acks = rocketnet_server_acks_create( NULL, 4 );
    server.reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 8192, 4 );
    server.counters = &counters;
    server.connection_ids = connection_ids;
    server.acked_connection_ids = acked_connection_ids;
    server.next_connection_id = 1;
    rocketnet_unit_check( server.encryption_map != NULL && server.client_slots != NULL );
    rocketnet_unit_check( server.acks != NULL && server.reassembly != NULL );

    rocketnet_reassembly_set_begin_update( server.reassembly );

    struct rocketnet_address_t addr_a, addr_b;
    rocketnet_unit_check( rocketnet_address_parse( &addr_a, "198.51.100.7:56789" ) );
    rocketnet_unit_check( rocketnet_address_parse( &addr_b, same_address ? "198.51.100.7:56789" : "198.51.100.8:56789" ) );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );

    uint8_t b_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    for ( int i = 0; i < ROCKETNET_CONNECTION_SALT_BYTES; i++ )
    {
        b_salt[i] = (uint8_t) ( rocketnet_test_salt[i] ^ 0x5Au );
    }

    // client A connects into slot 0 under its own session key and connection generation

    int map_a = rocketnet_encryption_map_add( server.encryption_map, &addr_a, 100.0 );
    rocketnet_unit_check( map_a != -1 );

    struct rocketnet_encryption_mapping_t * m_a = rocketnet_encryption_map_entry( server.encryption_map, map_a );
    memcpy( m_a->keys.session_client_to_server, rocketnet_test_session_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( m_a->connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    m_a->send_sequence_session = 2;

    const uint64_t generation_a = rocketnet_encryption_map_owner_generation( server.encryption_map, map_a );
    int slot_a = rocketnet_client_slots_assign( server.client_slots, 0xA000, map_a, generation_a, user_data );
    rocketnet_unit_check( slot_a == 0 );

    m_a->control->client_index = slot_a;
    m_a->control->established = 1;
    connection_ids[slot_a] = server.next_connection_id++;
    const uint64_t a_connection_id = connection_ids[slot_a];
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( &server.acks->endpoints[slot_a], 1, 64, 0xA111ULL, 1, 0, 99.0 ) );

    // stage fragment 0/2 of A's fragmented payload (payload_id = 1) before A disconnects

    uint8_t a_frag0_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 100];
    memset( a_frag0_plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    const int a_frag0_type_bytes = sim_build_fragment( a_frag0_plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, 1, 0, 2, 100, 100, 0xA0 );
    uint8_t a_frag0_packet[ROCKETNET_RECEIVE_CEILING];
    const int a_frag0_bytes = rocketnet_write_sealed_packet( a_frag0_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                                             rocketnet_test_filter_value, 1, NULL,
                                                             a_frag0_plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + a_frag0_type_bytes,
                                                             ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                             rocketnet_test_session_c2s_key );
    rocketnet_unit_check( a_frag0_bytes != 0 );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload_out[ROCKETNET_RECEIVE_CEILING];
    int payload_out_bytes = 0;
    int payload_client_index = -1;
    rocketnet_unit_check( rocketnet_server_protocol_process_packet( &server, &addr_a, a_frag0_packet, a_frag0_bytes,
                                                                    ROCKETNET_TEST_CREATE_TS, 100.0,
                                                                    response, &response_bytes,
                                                                    payload_out, &payload_out_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_unit_check( server.reassembly->completed_bytes == 0 );

    // A's stale payload, ack (acknowledging sequence 1), keep-alive, and
    // completing fragment 1/2, all sealed under A's session key and salt

    struct rocketnet_replay_protection_t ack_seq1_replay;
    memset( &ack_seq1_replay, 0, sizeof( ack_seq1_replay ) );
    rocketnet_replay_advance( &ack_seq1_replay, 1 );

    uint8_t ack_seq1_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    const int ack_seq1_block_bytes = rocketnet_ack_block_build( ack_seq1_block, sizeof( ack_seq1_block ), &ack_seq1_replay );
    rocketnet_unit_check( ack_seq1_block_bytes >= ROCKETNET_ACK_BLOCK_MIN_BYTES );

    uint8_t a_payload_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 4];
    memset( a_payload_plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );

    const uint8_t a_data[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
    memcpy( a_payload_plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, a_data, 4 );

    uint8_t a_payload_packet[ROCKETNET_RECEIVE_CEILING];
    int a_payload_bytes = rocketnet_write_sealed_packet( a_payload_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                         rocketnet_test_filter_value, 2, NULL,
                                                         a_payload_plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + 4,
                                                         ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                         rocketnet_test_session_c2s_key );
    rocketnet_unit_check( a_payload_bytes != 0 );

    uint8_t a_ack_packet[ROCKETNET_RECEIVE_CEILING];
    int a_ack_bytes = rocketnet_write_sealed_packet( a_ack_packet, ROCKETNET_CONNECTION_ACK_PACKET,
                                                     rocketnet_test_filter_value, 3, NULL,
                                                     ack_seq1_block, ack_seq1_block_bytes,
                                                     ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                     rocketnet_test_session_c2s_key );
    rocketnet_unit_check( a_ack_bytes != 0 );

    uint8_t a_keep_alive_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 8];
    memset( a_keep_alive_plaintext, 0, sizeof( a_keep_alive_plaintext ) );

    uint8_t a_keep_alive_packet[ROCKETNET_RECEIVE_CEILING];
    int a_keep_alive_bytes = rocketnet_write_sealed_packet( a_keep_alive_packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                            rocketnet_test_filter_value, 4, NULL,
                                                            a_keep_alive_plaintext, sizeof( a_keep_alive_plaintext ),
                                                            ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                            rocketnet_test_session_c2s_key );
    rocketnet_unit_check( a_keep_alive_bytes != 0 );

    uint8_t a_frag1_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 40];
    memset( a_frag1_plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    const int a_frag1_type_bytes = sim_build_fragment( a_frag1_plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, 1, 1, 2, 40, 100, 0xA0 );
    uint8_t a_frag1_packet[ROCKETNET_RECEIVE_CEILING];
    int a_frag1_bytes = rocketnet_write_sealed_packet( a_frag1_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                                       rocketnet_test_filter_value, 5, NULL,
                                                       a_frag1_plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + a_frag1_type_bytes,
                                                       ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                       rocketnet_test_session_c2s_key );
    rocketnet_unit_check( a_frag1_bytes != 0 );

    // A disconnects: slot 0 is reset and A's mapping destroyed

    uint8_t disconnect_packets[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int disconnect_count = 0, disconnect_bytes = 0;
    rocketnet_server_protocol_disconnect_client( &server, slot_a, disconnect_packets, &disconnect_count, &disconnect_bytes );
    rocketnet_unit_check( disconnect_count == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( connection_ids[slot_a] == 0 );

    // B's response in the same tick takes the freed slot 0 under B's own key,
    // salt, and connection generation (even when B connects from addr_a).

    int map_b = rocketnet_encryption_map_add( server.encryption_map, &addr_b, 100.0 );
    rocketnet_unit_check( map_b != -1 );

    struct rocketnet_encryption_mapping_t * m_b = rocketnet_encryption_map_entry( server.encryption_map, map_b );
    memcpy( m_b->keys.session_client_to_server, rocketnet_test_session_s2c_key, ROCKETNET_KEY_BYTES );
    memcpy( m_b->connection_salt, b_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    m_b->send_sequence_session = 2;

    const uint64_t generation_b = rocketnet_encryption_map_owner_generation( server.encryption_map, map_b );
    int slot_b = rocketnet_client_slots_assign( server.client_slots, 0xB000, map_b, generation_b, user_data );
    rocketnet_unit_check( slot_b == 0 );

    m_b->control->client_index = slot_b;
    m_b->control->established = 1;
    connection_ids[slot_b] = server.next_connection_id++;
    const uint64_t b_connection_id = connection_ids[slot_b];
    rocketnet_unit_check( b_connection_id != 0 && b_connection_id != a_connection_id );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( &server.acks->endpoints[slot_b], 1, 64, 0xB222ULL, 1, 0, 100.0 ) );

    // B's payload, ACK, and 2-fragment payload, sealed under B's session key and salt

    uint8_t b_payload_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 4];
    memset( b_payload_plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );

    const uint8_t b_data[4] = { 0xBB, 0xBB, 0xBB, 0xBB };
    memcpy( b_payload_plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, b_data, 4 );

    uint8_t b_payload_packet[ROCKETNET_RECEIVE_CEILING];
    int b_payload_bytes = rocketnet_write_sealed_packet( b_payload_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                         rocketnet_test_filter_value, 1, NULL,
                                                         b_payload_plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + 4,
                                                         ROCKETNET_TEST_PROTOCOL_ID, b_salt, NULL,
                                                         rocketnet_test_session_s2c_key );
    rocketnet_unit_check( b_payload_bytes != 0 );

    uint8_t b_ack_packet[ROCKETNET_RECEIVE_CEILING];
    int b_ack_bytes = rocketnet_write_sealed_packet( b_ack_packet, ROCKETNET_CONNECTION_ACK_PACKET,
                                                     rocketnet_test_filter_value, 2, NULL,
                                                     ack_seq1_block, ack_seq1_block_bytes,
                                                     ROCKETNET_TEST_PROTOCOL_ID, b_salt, NULL,
                                                     rocketnet_test_session_s2c_key );
    rocketnet_unit_check( b_ack_bytes != 0 );

    uint8_t b_frag0_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 100];
    memset( b_frag0_plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    const int b_frag0_type_bytes = sim_build_fragment( b_frag0_plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, 1, 0, 2, 100, 100, 0xCC );
    uint8_t b_frag0_packet[ROCKETNET_RECEIVE_CEILING];
    int b_frag0_bytes = rocketnet_write_sealed_packet( b_frag0_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                                       rocketnet_test_filter_value, 3, NULL,
                                                       b_frag0_plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + b_frag0_type_bytes,
                                                       ROCKETNET_TEST_PROTOCOL_ID, b_salt, NULL,
                                                       rocketnet_test_session_s2c_key );
    rocketnet_unit_check( b_frag0_bytes != 0 );

    uint8_t b_frag1_plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + 40];
    memset( b_frag1_plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    const int b_frag1_type_bytes = sim_build_fragment( b_frag1_plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, 1, 1, 2, 40, 100, 0xCC );
    uint8_t b_frag1_packet[ROCKETNET_RECEIVE_CEILING];
    int b_frag1_bytes = rocketnet_write_sealed_packet( b_frag1_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                                       rocketnet_test_filter_value, 4, NULL,
                                                       b_frag1_plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + b_frag1_type_bytes,
                                                       ROCKETNET_TEST_PROTOCOL_ID, b_salt, NULL,
                                                       rocketnet_test_session_s2c_key );
    rocketnet_unit_check( b_frag1_bytes != 0 );

    // One server update driven through the wire: A's stale payload, stale ack,
    // stale keep-alive, and stale completing fragment 1/2 arrive first, then
    // B's payload, ACK, and 2-fragment payload.

    struct sim_schedule_t sched;
    sim_schedule_begin( &sched );

    sched.from = addr_a;
    sim_schedule_deliver( &sched, a_payload_packet, a_payload_bytes );
    sim_schedule_deliver( &sched, a_ack_packet, a_ack_bytes );
    sim_schedule_deliver( &sched, a_keep_alive_packet, a_keep_alive_bytes );
    sim_schedule_deliver( &sched, a_frag1_packet, a_frag1_bytes );
    rocketnet_unit_check( sim_schedule_flush( &sched ) == 4 );

    sched.from = addr_b;
    sim_schedule_deliver( &sched, b_payload_packet, b_payload_bytes );
    sim_schedule_deliver( &sched, b_ack_packet, b_ack_bytes );
    sim_schedule_deliver( &sched, b_frag0_packet, b_frag0_bytes );
    sim_schedule_deliver( &sched, b_frag1_packet, b_frag1_bytes );
    rocketnet_unit_check( sim_schedule_flush( &sched ) == 4 );

    const int stale_bytes[4] = { a_payload_bytes, a_ack_bytes, a_keep_alive_bytes, a_frag1_bytes };

    for ( int i = 0; i < 4; i++ )
    {
        payload_out_bytes = 0;
        payload_client_index = -1;

        rocketnet_unit_check( sim_schedule_receive( &sched ) == stale_bytes[i] );

        int r = rocketnet_server_protocol_process_packet( &server, &sched.rx_from, sched.rx, stale_bytes[i],
                                                          ROCKETNET_TEST_CREATE_TS, 100.0,
                                                          response, &response_bytes,
                                                          payload_out, &payload_out_bytes, &payload_client_index );
        rocketnet_unit_check( r == ROCKETNET_SERVER_PROCESS_IGNORED );
        rocketnet_unit_check( payload_client_index == -1 );
        rocketnet_unit_check( payload_out_bytes == 0 );
    }

    if ( same_address )
    {
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN] == 1 );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_ACK_DID_NOT_OPEN] == 1 );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN] == 1 );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN] == 1 );
    }
    else
    {
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING] == 1 );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_ACK_NO_MAPPING] == 1 );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING] == 1 );
        rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING] == 1 );
    }

    // none of A's stale packets advanced B's replay window, ack state, or reassembly state

    rocketnet_unit_check( m_b->replay_session.highest_opened == 0 );
    rocketnet_unit_check( m_b->accepted_session.highest_opened == 0 );
    rocketnet_unit_check( server.acks->num_acked == 0 );
    rocketnet_unit_check( server.acks->endpoints[slot_b].largest_acked_sequence == 0 );
    rocketnet_unit_check( server.acks->endpoints[slot_b].highest_received_sequence == 0 );
    rocketnet_unit_check( server.reassembly->completed_bytes == 0 );
    rocketnet_unit_check( server.reassembly->num_delivered == 0 );
    rocketnet_unit_check( server.reassembly->newest_completed[slot_b] == 0 );

    // now B's new-generation payload, ACK, and 2-fragment payload all complete cleanly

    payload_out_bytes = 0;
    payload_client_index = -1;

    rocketnet_unit_check( sim_schedule_receive( &sched ) == b_payload_bytes );

    int r = rocketnet_server_protocol_process_packet( &server, &sched.rx_from, sched.rx, b_payload_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, 100.0,
                                                      response, &response_bytes,
                                                      payload_out, &payload_out_bytes, &payload_client_index );
    rocketnet_unit_check( r == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_unit_check( payload_client_index == slot_b );
    rocketnet_unit_check( server.payload_connection_id == b_connection_id );
    rocketnet_unit_check( payload_out_bytes == 4 );
    rocketnet_unit_check( memcmp( payload_out, b_data, 4 ) == 0 );
    rocketnet_unit_check( m_b->replay_session.highest_opened == 1 );

    payload_out_bytes = 0;
    payload_client_index = -1;

    rocketnet_unit_check( sim_schedule_receive( &sched ) == b_ack_bytes );

    r = rocketnet_server_protocol_process_packet( &server, &sched.rx_from, sched.rx, b_ack_bytes,
                                                  ROCKETNET_TEST_CREATE_TS, 100.0,
                                                  response, &response_bytes,
                                                  payload_out, &payload_out_bytes, &payload_client_index );
    rocketnet_unit_check( r == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_unit_check( server.acks->num_acked == 1 );
    rocketnet_unit_check( server.acks->acked_packet_ids[0] == 0xB222ULL );
    rocketnet_unit_check( server.acks->acked_client_index[0] == slot_b );
    rocketnet_unit_check( server.acked_connection_ids[0] == b_connection_id );
    rocketnet_unit_check( server.acks->endpoints[slot_b].largest_acked_sequence == 1 );

    payload_out_bytes = 0;
    payload_client_index = -1;

    rocketnet_unit_check( sim_schedule_receive( &sched ) == b_frag0_bytes );

    r = rocketnet_server_protocol_process_packet( &server, &sched.rx_from, sched.rx, b_frag0_bytes,
                                                  ROCKETNET_TEST_CREATE_TS, 100.0,
                                                  response, &response_bytes,
                                                  payload_out, &payload_out_bytes, &payload_client_index );
    rocketnet_unit_check( r == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_unit_check( server.reassembly->completed_bytes == 0 );

    payload_out_bytes = 0;
    payload_client_index = -1;

    rocketnet_unit_check( sim_schedule_receive( &sched ) == b_frag1_bytes );

    r = rocketnet_server_protocol_process_packet( &server, &sched.rx_from, sched.rx, b_frag1_bytes,
                                                  ROCKETNET_TEST_CREATE_TS, 100.0,
                                                  response, &response_bytes,
                                                  payload_out, &payload_out_bytes, &payload_client_index );
    rocketnet_unit_check( r == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    rocketnet_unit_check( payload_client_index == slot_b );
    rocketnet_unit_check( server.payload_connection_id == b_connection_id );
    rocketnet_unit_check( server.reassembly->completed_bytes == 140 );
    rocketnet_unit_check( server.reassembly->num_delivered == 1 );
    rocketnet_unit_check( server.reassembly->newest_completed[slot_b] == 1 );

    sim_check_reassembled( server.reassembly->completed_payload, 140, 0xCC );

    sim_schedule_end( &sched );
    rocketnet_reassembly_set_destroy( server.reassembly );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_counters_assert_conserved( &counters );
}

static void test_sim_schedule_same_tick_slot_reuse()
{
    sim_run_same_tick_slot_reuse( false );
    sim_run_same_tick_slot_reuse( true );
}

// ---------------------------------------------------------------------------
// 5. sequence pre-seeding across the wrap
// ---------------------------------------------------------------------------

static void test_sim_schedule_sequence_wrap()
{
    struct sim_schedule_t sched;
    sim_schedule_begin( &sched );

    // the transport sequence: 64-bit counter, low 32 bits on the wire.
    // seeded near 2^32 - 128, 512 packets reconstruct across the wrap.

    const uint64_t base = ( 1ULL << 32 ) - 128;
    uint64_t highest = base;
    uint8_t transport_packets[512][8];
    for ( int i = 0; i < 512; i++ )
    {
        sim_write_u64_le( transport_packets[i], base + 1 + (uint64_t) i );
        sim_schedule_deliver( &sched, transport_packets[i], 8 );
    }

    rocketnet_unit_check( sim_schedule_flush( &sched ) == 512 );

    for ( int i = 0; i < 512; i++ )
    {
        rocketnet_unit_check( sim_schedule_receive( &sched ) == 8 );

        uint64_t seq = sim_read_u64_le( sched.rx );
        uint64_t got = rocketnet_reconstruct_sequence( (uint32_t) seq, highest );
        rocketnet_unit_check( got == seq );

        highest = seq;
    }

    // the replay window, seeded near 2^24 - 128: 512 packets across the
    // boundary are each accepted exactly once and the floor moves forward.

    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    const uint64_t win_base = ( 1ULL << 24 ) - 128;
    uint8_t replay_packets[512][8];
    for ( int i = 0; i < 512; i++ )
    {
        sim_write_u64_le( replay_packets[i], win_base + 1 + (uint64_t) i );
        sim_schedule_deliver( &sched, replay_packets[i], 8 );
    }

    rocketnet_unit_check( sim_schedule_flush( &sched ) == 512 );

    for ( int i = 0; i < 512; i++ )
    {
        rocketnet_unit_check( sim_schedule_receive( &sched ) == 8 );

        uint64_t seq = sim_read_u64_le( sched.rx );
        rocketnet_unit_check( !rocketnet_replay_already_received( &replay, seq ) );

        rocketnet_replay_advance( &replay, seq );
        rocketnet_unit_check( rocketnet_replay_already_received( &replay, seq ) != 0 );
    }

    // below the retained window everything is refused as too old

    rocketnet_unit_check( rocketnet_replay_already_received( &replay, 1 ) != 0 );

    sim_schedule_end( &sched );
}

// ---------------------------------------------------------------------------
// Two pins on the simulator's stated behaviour (audit 2026-10-24 findings 7
// and 8). Neither changes code; each holds a comment in
// sdk/rocketnet_network_simulator.cpp to what the code does.
// ---------------------------------------------------------------------------

extern bool rocketnet_network_simulator_created_active( const struct rocketnet_network_simulator_t * simulator );

// created_active latches on the first config with a nonzero setting and is
// never cleared: a later all-zero config leaves it set.
static void test_simulator_created_active_is_sticky()
{
    struct rocketnet_network_simulator_t * sim = rocketnet_network_simulator_create( NULL, 16, 1 );
    rocketnet_unit_check( sim != NULL );

    rocketnet_network_simulator_config( sim, 0.0, 0.0, 0.0f, 0.0f );
    rocketnet_unit_check( !rocketnet_network_simulator_created_active( sim ) );

    rocketnet_network_simulator_config( sim, 0.05, 0.0, 0.0f, 0.0f );
    rocketnet_unit_check( rocketnet_network_simulator_created_active( sim ) );

    rocketnet_network_simulator_config( sim, 0.0, 0.0, 0.0f, 0.0f );
    rocketnet_unit_check( rocketnet_network_simulator_created_active( sim ) );

    rocketnet_network_simulator_destroy( sim );
}

// Two addresses of a type that is neither IPv4 nor IPv6 never compare equal,
// even with identical bytes. The simulator's shard hash folds only the type
// for them, which is conservative: equal still implies the same shard.
static void test_address_equal_degenerate_types_never_equal()
{
    struct rocketnet_address_t a;
    struct rocketnet_address_t b;
    memset( &a, 0, sizeof( a ) );
    memset( &b, 0, sizeof( b ) );
    a.type = ROCKETNET_ADDRESS_NONE;
    b.type = ROCKETNET_ADDRESS_NONE;
    rocketnet_unit_check( memcmp( &a, &b, sizeof( a ) ) == 0 );
    rocketnet_unit_check( !rocketnet_address_equal( &a, &b ) );
    rocketnet_unit_check( !rocketnet_address_equal( &a, &a ) );
}

// The shard index is a two-level flat bitmap, not a timing claim. This case
// crosses physical-slot word boundaries, removes and reuses an interior slot,
// proves the old generation cannot remove its replacement, and observes the
// exact bounded word work. A dense shard with only its final slot due also
// pins ascending enumeration and the scan bound to occupied entries plus
// summary/occupancy words.
static void test_simulator_bitmap_membership_generation_and_bounds()
{
    const int capacity = 130;
    struct rocketnet_network_simulator_t * sim = rocketnet_network_simulator_create( NULL, capacity, 1 );
    rocketnet_unit_check( sim != NULL );

    struct rocketnet_address_t from;
    struct rocketnet_address_t to;
    rocketnet_unit_check( rocketnet_address_parse( &from, "198.51.100.10:10000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &to, "198.51.100.20:10000" ) );

    rocketnet_network_simulator_test_reset_index_stats( sim );

    for ( int i = 0; i < capacity; i++ )
    {
        const uint8_t byte = (uint8_t) i;
        const uint8_t * packet = &byte;
        const int bytes = 1;
        rocketnet_unit_check( rocketnet_network_simulator_deliver_packets( sim, &from, &to, &packet, &bytes, 1, 10.0 ) == 1 );
    }

    struct rocketnet_network_simulator_slot_handle_t old_handle;
    rocketnet_unit_check( rocketnet_network_simulator_test_handle_for_slot( sim, 64, &old_handle ) );
    rocketnet_unit_check( rocketnet_network_simulator_test_remove_handle( sim, &old_handle ) );

    const uint8_t replacement = 0xee;
    const uint8_t * replacement_packet = &replacement;
    const int replacement_bytes = 1;
    rocketnet_unit_check( rocketnet_network_simulator_deliver_packets( sim, &from, &to,
                                                                       &replacement_packet, &replacement_bytes,
                                                                       1, 10.0 ) == 1 );

    struct rocketnet_network_simulator_slot_handle_t replacement_handle;
    rocketnet_unit_check( rocketnet_network_simulator_test_handle_for_slot( sim, 64, &replacement_handle ) );
    rocketnet_unit_check( replacement_handle.generation != old_handle.generation );
    rocketnet_unit_check( !rocketnet_network_simulator_test_remove_handle( sim, &old_handle ) );

    uint8_t received[ROCKETNET_RECEIVE_CEILING];
    struct rocketnet_address_t received_from;
    for ( int i = 0; i < capacity; i++ )
    {
        rocketnet_unit_check( rocketnet_network_simulator_receive( sim, &to, 10.0, &received_from, received ) == 1 );
        rocketnet_unit_check( received[0] == ( i == 64 ? replacement : (uint8_t) i ) );
    }

    rocketnet_unit_check( rocketnet_network_simulator_receive( sim, &to, 10.0, &received_from, received ) == 0 );

    struct rocketnet_network_simulator_index_stats_t stats;
    rocketnet_network_simulator_test_index_stats( sim, &stats );
    rocketnet_unit_check( stats.max_membership_word_updates <= 2 );
    rocketnet_unit_check( stats.membership_word_updates == 268 );
    rocketnet_unit_check( stats.stale_removals == 1 );
    rocketnet_unit_check( stats.max_selection_summary_words == 1 );
    rocketnet_unit_check( stats.max_selection_occupancy_words <= 1 );
    rocketnet_unit_check( stats.max_selection_slots <= 1 );

    rocketnet_network_simulator_destroy( sim );

    sim = rocketnet_network_simulator_create( NULL, capacity, 2 );
    rocketnet_unit_check( sim != NULL );

    for ( int i = 0; i < capacity; i++ )
    {
        const uint8_t byte = (uint8_t) i;
        const uint8_t * packet = &byte;
        const int bytes = 1;
        const double delivery_time = i == capacity - 1 ? 0.0 : 1000.0;
        rocketnet_unit_check( rocketnet_network_simulator_deliver_packets( sim, &from, &to, &packet, &bytes, 1, delivery_time ) == 1 );
    }

    rocketnet_network_simulator_test_reset_index_stats( sim );
    rocketnet_unit_check( rocketnet_network_simulator_receive( sim, &to, 0.0, &received_from, received ) == 1 );
    rocketnet_unit_check( received[0] == (uint8_t) ( capacity - 1 ) );

    rocketnet_network_simulator_test_index_stats( sim, &stats );
    rocketnet_unit_check( stats.max_selection_summary_words == 1 );
    rocketnet_unit_check( stats.max_selection_occupancy_words == 3 );
    rocketnet_unit_check( stats.max_selection_slots == (uint64_t) capacity );
    rocketnet_unit_check( stats.max_membership_word_updates <= 2 );

    rocketnet_network_simulator_destroy( sim );
}

// ---------------------------------------------------------------------------

REGISTER_UNIT_TEST( test_sim_schedule_final_fragment_first );
REGISTER_UNIT_TEST( test_sim_schedule_batch_cap );
REGISTER_UNIT_TEST( test_sim_schedule_same_tick_slot_reuse );
REGISTER_UNIT_TEST( test_sim_schedule_sequence_wrap );
REGISTER_UNIT_TEST( test_sim_schedule_long_skip_stragglers );
REGISTER_UNIT_TEST( test_simulator_created_active_is_sticky );
REGISTER_UNIT_TEST( test_address_equal_degenerate_types_never_equal );
REGISTER_UNIT_TEST( test_simulator_bitmap_membership_generation_and_bounds );
