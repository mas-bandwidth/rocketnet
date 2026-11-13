/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's session writers and per-slot preflights at their refusals: a
// free slot, a protocol built without ack bookkeeping, a payload size past the
// limit, and a seal that fails because the session counter is at 0 (no
// session). Each refusal returns 0 and leaves the slot's ack debt, its send
// clocks and its counters as they were. One protocol with a hand-made
// connected slot, as tests/test_unit_standard_session.cpp builds it; no
// sockets, no real time, clocks from 1000.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static void r2hs_server_fault_if( bool fault, const char * text )
{
    if ( fault )
    {
        printf( "fault: %s\n", text );
    }
    check( !fault );
}

// an assert in a refused call is a fault of its own: it is counted, and the
// call is let run on so the refusal's result is still checked

static int r2hs_server_asserts = 0;

static void r2hs_server_count_assert( const char * condition, const char * function, const char * file, int line )
{
    printf( "assert %s in %s (%s:%d)\n", condition, function, file, line );
    r2hs_server_asserts++;
}

struct r2hs_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
    int mapping_index;
    rocketnet_assert_function_t saved_assert;
};

// slot 0 connected through a live mapping, slot 1 free

static void r2hs_server_setup( struct r2hs_server_t * test )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = 0x1122334455667788ULL;
    server->send_rate = 10.0;
    memset( server->packet_filter, 0x11, ROCKETNET_FILTER_VALUE_BYTES );
    server->encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    server->client_slots = rocketnet_client_slots_create( NULL, 2 );
    server->acks = rocketnet_server_acks_create( NULL, 2 );
    server->counters = &test->counters;
    check( server->encryption_map && server->client_slots && server->acks );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );

    test->mapping_index = rocketnet_encryption_map_add( server->encryption_map, &test->client_address, 1005.0 );
    check( test->mapping_index >= 0 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, test->mapping_index );
    mapping->control->established = 1;
    mapping->control->client_index = 0;
    mapping->control->timeout_seconds = 10;
    mapping->control->last_receive_time = 1000.0;
    mapping->last_send_time = 1000.0;
    mapping->last_keep_alive_send_time = 1000.0;
    mapping->send_sequence_session = 1;
    memset( mapping->keys.session_server_to_client, 0x42, 32 );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );

    const uint64_t owner_generation = rocketnet_encryption_map_owner_generation( server->encryption_map, test->mapping_index );
    check( rocketnet_client_slots_assign( server->client_slots, 0x1234, test->mapping_index, owner_generation, user_data ) == 0 );
    check( !rocketnet_client_slots_connected( server->client_slots, 1 ) );

    test->saved_assert = rn_assert_function;
    r2hs_server_asserts = 0;
    rocketnet_assert_function( r2hs_server_count_assert );
}

static void r2hs_server_teardown( struct r2hs_server_t * test )
{
    rocketnet_assert_function( test->saved_assert );
    rocketnet_server_acks_destroy( test->server.acks );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_encryption_map_destroy( test->server.encryption_map );
}

static struct rocketnet_encryption_mapping_t * r2hs_server_mapping( struct r2hs_server_t * test )
{
    return rocketnet_encryption_map_entry( test->server.encryption_map, test->mapping_index );
}

static void r2hs_server_no_asserts( const char * what )
{
    if ( r2hs_server_asserts != 0 )
    {
        printf( "fault: %s fired %d assert(s)\n", what, r2hs_server_asserts );
    }
    check( r2hs_server_asserts == 0 );
}

// mutation gap: hunt 2/1775
// mutation gap: hunt 2/1776
//
// A standalone ack whose seal fails (the session counter at 0) is not a
// send: 0 bytes, the ack debt still owed, the send clock not stamped.

static void test_gaps_r2_hs_ack_seal_failure_is_not_a_send()
{
    struct r2hs_server_t test;
    r2hs_server_setup( &test );
    struct rocketnet_encryption_mapping_t * mapping = r2hs_server_mapping( &test );
    mapping->send_sequence_session = 0;
    test.server.acks->endpoints[0].ack_debt = 3;

    uint8_t packet[ROCKETNET_SEND_BUDGET];
    const int bytes = rocketnet_server_protocol_write_ack_if_due( &test.server, 0, 1001.0, packet );
    r2hs_server_fault_if( bytes != 0, "a standalone ack whose seal failed reported bytes written" );
    r2hs_server_fault_if( test.server.acks->endpoints[0].ack_debt != 3, "a standalone ack whose seal failed cleared the ack debt" );
    r2hs_server_fault_if( mapping->last_send_time != 1000.0, "a standalone ack whose seal failed stamped the send clock" );
    r2hs_server_no_asserts( "a standalone ack whose seal failed" );

    r2hs_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_ack_seal_failure_is_not_a_send );

// mutation gap: hunt 2/1799
// mutation gap: hunt 2/1801
// mutation gap: hunt 2/1802
//
// A keep-alive whose seal fails is not a send: 0 bytes, and neither send
// clock is stamped, so the pacing that depends on the keep-alive clock is not
// silenced for an interval.

static void test_gaps_r2_hs_keep_alive_seal_failure_is_not_a_send()
{
    struct r2hs_server_t test;
    r2hs_server_setup( &test );
    struct rocketnet_encryption_mapping_t * mapping = r2hs_server_mapping( &test );
    mapping->send_sequence_session = 0;

    uint8_t packet[ROCKETNET_SEND_BUDGET];
    const int bytes = rocketnet_server_write_keep_alive( &test.server, 0, 1001.0, packet );
    r2hs_server_fault_if( bytes != 0, "a keep-alive whose seal failed reported bytes written" );
    r2hs_server_fault_if( mapping->last_keep_alive_send_time != 1000.0, "a keep-alive whose seal failed stamped the keep-alive clock" );
    r2hs_server_fault_if( mapping->last_send_time != 1000.0, "a keep-alive whose seal failed stamped the send clock" );
    r2hs_server_no_asserts( "a keep-alive whose seal failed" );

    // with the session running the same call sends and stamps

    mapping->send_sequence_session = 1;
    check( rocketnet_server_write_keep_alive( &test.server, 0, 1001.0, packet ) >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    check( mapping->last_keep_alive_send_time == 1001.0 );

    r2hs_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_keep_alive_seal_failure_is_not_a_send );

// mutation gap: hunt 2/1811
// mutation gap: hunt 2/1812
//
// A protocol built without ack bookkeeping owes no acks: write_ack_if_due
// writes nothing, and reads no endpoint.

static void test_gaps_r2_hs_ack_if_due_without_acks()
{
    struct r2hs_server_t test;
    r2hs_server_setup( &test );
    struct rocketnet_server_acks_t * acks = test.server.acks;
    test.server.acks = NULL;

    uint8_t packet[ROCKETNET_SEND_BUDGET];
    const int bytes = rocketnet_server_protocol_write_ack_if_due( &test.server, 0, 1001.0, packet );
    r2hs_server_fault_if( bytes != 0, "write_ack_if_due wrote an ack on a protocol built without ack bookkeeping" );

    test.server.acks = acks;
    r2hs_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_ack_if_due_without_acks );

// mutation gap: hunt 2/1836
// mutation gap: hunt 2/1837
// mutation gap: hunt 2/1849
// mutation gap: hunt 2/1850
// mutation gap: hunt 2/1863
// mutation gap: hunt 2/1864
//
// A free slot has no maintenance due, starts no payload and seals no frame;
// none of them reads the free slot's mapping index (an assert) or counts a
// slot without a mapping.

static void test_gaps_r2_hs_free_slot_refusals()
{
    struct r2hs_server_t test;
    r2hs_server_setup( &test );

    const int due = rocketnet_server_protocol_maintenance_due( &test.server, 1, 2000.0 );
    r2hs_server_fault_if( due != 0, "a free slot reported maintenance due" );
    r2hs_server_no_asserts( "maintenance_due on a free slot" );

    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    uint64_t sequence = 0xABCDu;
    const int started = rocketnet_server_protocol_start_payload( &test.server, 1, &to, &sequence );
    r2hs_server_fault_if( started != 0, "start_payload started a payload for a free slot" );
    r2hs_server_fault_if( sequence != 0xABCDu, "start_payload wrote a sequence for a free slot" );
    r2hs_server_fault_if( test.counters.values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING] != 0,
                          "start_payload on a free slot counted a slot without a mapping" );
    r2hs_server_no_asserts( "start_payload on a free slot" );

    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE] = {};
    const int sealed = rocketnet_server_protocol_seal_payload_frame( &test.server, 1, 1, 1, frame + ROCKETNET_FRAME_WIRE_OFFSET, 4, 2000.0 );
    r2hs_server_fault_if( sealed != 0, "seal_payload_frame sealed a frame for a free slot" );
    r2hs_server_no_asserts( "seal_payload_frame on a free slot" );

    // the connected slot is due once its interval has passed

    check( rocketnet_server_protocol_maintenance_due( &test.server, 0, 2000.0 ) == 1 );

    r2hs_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_free_slot_refusals );

// mutation gap: hunt 2/1871
// mutation gap: hunt 2/1872
// mutation gap: hunt 2/1876
// mutation gap: hunt 2/1877
//
// seal_payload_frame refuses a payload of 0 bytes or one byte past
// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, and a frame whose seal fails (sequence
// 0): 0 bytes, the ack debt still owed, the send clock not stamped, nothing
// recorded sent. A 4-byte payload under a started sequence seals.

static void test_gaps_r2_hs_seal_payload_frame_refusals()
{
    struct r2hs_server_t test;
    r2hs_server_setup( &test );
    struct rocketnet_encryption_mapping_t * mapping = r2hs_server_mapping( &test );
    struct rocketnet_ack_endpoint_t * endpoint = &test.server.acks->endpoints[0];
    endpoint->ack_debt = 2;

    struct rocketnet_address_t to;
    uint64_t sequence = 0;
    check( rocketnet_server_protocol_start_payload( &test.server, 0, &to, &sequence ) == 1 );
    check( sequence == 1 );

    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE] = {};
    uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
    const uint64_t highest_before = endpoint->highest_sequence;

    int bytes = rocketnet_server_protocol_seal_payload_frame( &test.server, 0, sequence, 9, wire, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1, 1001.0 );
    r2hs_server_fault_if( bytes != 0, "seal_payload_frame sealed a payload one byte past ROCKETNET_MAX_PACKET_PAYLOAD_BYTES" );
    bytes = rocketnet_server_protocol_seal_payload_frame( &test.server, 0, sequence, 9, wire, 0, 1001.0 );
    r2hs_server_fault_if( bytes != 0, "seal_payload_frame sealed a 0-byte payload" );
    bytes = rocketnet_server_protocol_seal_payload_frame( &test.server, 0, 0, 9, wire, 4, 1001.0 );
    r2hs_server_fault_if( bytes != 0, "seal_payload_frame reported bytes for a frame whose seal failed (sequence 0)" );

    r2hs_server_fault_if( endpoint->ack_debt != 2, "a refused payload frame cleared the ack debt" );
    r2hs_server_fault_if( mapping->last_send_time != 1000.0, "a refused payload frame stamped the send clock" );
    r2hs_server_fault_if( endpoint->highest_sequence != highest_before, "a refused payload frame was recorded sent" );
    r2hs_server_no_asserts( "a refused payload frame" );

    bytes = rocketnet_server_protocol_seal_payload_frame( &test.server, 0, sequence, 9, wire, 4, 1001.0 );
    check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 );
    check( endpoint->ack_debt == 0 );
    check( mapping->last_send_time == 1001.0 );

    r2hs_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_seal_payload_frame_refusals );

#if ROCKETNET_DEV

// mutation gap: hunt 2/1893
// mutation gap: hunt 2/1894
// mutation gap: hunt 2/1900
// mutation gap: hunt 2/1901
//
// The DEV payload writer writes nothing for a free slot, and a payload whose
// seal fails (the session counter at 0) is not a send: 0 bytes, the ack debt
// still owed, the send clock not stamped.

static void test_gaps_r2_hs_dev_write_payload_refusals()
{
    struct r2hs_server_t test;
    r2hs_server_setup( &test );
    struct rocketnet_encryption_mapping_t * mapping = r2hs_server_mapping( &test );
    struct rocketnet_ack_endpoint_t * endpoint = &test.server.acks->endpoints[0];
    endpoint->ack_debt = 2;

    const uint8_t data[4] = { 1, 2, 3, 4 };
    uint8_t packet[ROCKETNET_SEND_BUDGET];

    int bytes = rocketnet_server_protocol_write_payload( &test.server, 1, data, (int) sizeof( data ), 1001.0, packet );
    r2hs_server_fault_if( bytes != 0, "the DEV payload writer wrote a payload for a free slot" );

    mapping->send_sequence_session = 0;
    bytes = rocketnet_server_protocol_write_payload( &test.server, 0, data, (int) sizeof( data ), 1001.0, packet );
    r2hs_server_fault_if( bytes != 0, "the DEV payload writer reported bytes for a payload whose seal failed" );
    r2hs_server_fault_if( endpoint->ack_debt != 2, "a DEV payload whose seal failed cleared the ack debt" );
    r2hs_server_fault_if( mapping->last_send_time != 1000.0, "a DEV payload whose seal failed stamped the send clock" );
    r2hs_server_no_asserts( "the DEV payload writer's refusals" );

    mapping->send_sequence_session = 1;
    check( rocketnet_server_protocol_write_payload( &test.server, 0, data, (int) sizeof( data ), 1001.0, packet ) > 0 );
    check( endpoint->ack_debt == 0 );

    r2hs_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_dev_write_payload_refusals );

#endif // #if ROCKETNET_DEV
