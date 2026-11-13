/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport's construction sets the counts of its buffer members
// and never their bytes (sdk/rocketnet_client_transport.cpp,
// rocketnet_client_transport_construct): the payload rings, the received
// batch and the receive buffers are left as the allocator returned them. A
// system allocator hands a large block back zeroed, so a count construction
// forgot still reads 0 and nothing notices. These cases install an allocator
// that fills every allocation with a poison byte, so a forgotten count is a
// deterministic wrong value, then check each count through a read seam and
// run the first operation that uses it. The message client the ack-stats
// seam adopts, and that seam's protocol, are checked the same way.
//
// The transport is created step-driven against the fake client transport
// (tests/fake_client_driver.h): no thread, no socket; time is an argument
// starting at 1000.0. Each failing check prints the fault first.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "loopback_hub.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_config.h"
#include "rocketnet_ack.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

extern void rocketnet_client_transport_counts_for_test( const rocketnet_client_transport_t * client_transport, uint64_t * ring_counts,
                                                        int * received_num_packets, int * received_receive_index, int * buffer_counts,
                                                        uint64_t * transport_faults );
extern int rocketnet_client_transport_ring_wire_bytes_for_test( const rocketnet_client_transport_t * client_transport, int ring, int slot );
extern bool rocketnet_client_transport_reset_witness_for_test( struct rocketnet_client_transport_t * client );
extern bool rocketnet_client_transport_connection_state_clean_for_test( struct rocketnet_client_transport_t * client, char * failure, size_t failure_bytes );
extern void rocketnet_client_transport_seed_refusal_state_for_test( rocketnet_client_transport_t *, uint64_t, const uint8_t *, const uint8_t *, const uint8_t * );
extern void rocketnet_client_transport_ack_stats_for_test( const rocketnet_ack_endpoint_t * endpoint, rocketnet_client_transport_stats_t * stats );
extern struct rocketnet_client_t * rocketnet_client_ack_stats_public_client_for_test( const rocketnet_ack_endpoint_t * endpoint );
extern void rocketnet_client_ack_stats_public_client_for_test_free( struct rocketnet_client_t * client );

// ---------------------------------------------------------------------------
// the poisoning allocator

static uint8_t ct_memory_poison_byte = 0xA5;

static void * ct_memory_poison_malloc( void * context, size_t bytes )
{
    (void) context;
    void * p = malloc( bytes );
    if ( p )
    {
        memset( p, ct_memory_poison_byte, bytes );
    }
    return p;
}

static void ct_memory_poison_free( void * context, void * p )
{
    (void) context;
    free( p );
}

static void ct_memory_poison_install( uint8_t poison )
{
    ct_memory_poison_byte = poison;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( NULL, ct_memory_poison_malloc, ct_memory_poison_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );
}

static void ct_memory_poison_remove()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// a step-driven client on the BASE path (the fake's four required calls, no
// wait and wake): payload ring 0 and the legacy receive buffers

static double ct_memory_now = 1000.0;
static double ct_memory_time( void * ) { return ct_memory_now; }

static void ct_memory_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

// the token's server, 127.0.0.1:40000
static void ct_memory_server_address( rocketnet_address_t * address )
{
    memset( address, 0, sizeof( *address ) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = 127;
    address->data.ipv4[3] = 1;
    address->port = 40000;
}

static rocketnet_client_transport_t * ct_memory_base_client( rocketnet_fake_client_transport_io_t * fake )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    ct_memory_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = ct_memory_time;

    ct_memory_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0,
                                                                                              &rocketnet_fake_client_operations_base, fake );
    check( transport != NULL );
    return transport;
}

struct ct_memory_counts_t
{
    uint64_t rings[ROCKETNET_CLIENT_IO_NUM_RINGS];
    int received_num_packets;
    int received_receive_index;
    int buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    uint64_t transport_faults;
};

static ct_memory_counts_t ct_memory_counts( const rocketnet_client_transport_t * transport )
{
    ct_memory_counts_t counts;
    memset( &counts, 0, sizeof( counts ) );
    rocketnet_client_transport_counts_for_test( transport, counts.rings, &counts.received_num_packets, &counts.received_receive_index,
                                                counts.buffers, &counts.transport_faults );
    return counts;
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6355 (ring loop from 1), hunt 2/6356 (no ring count reset)
//
// Construct sets every payload ring's count to 0 under a poisoning
// allocator, and the first payload a connected client starts takes slot 0 of
// ring 0 (the BASE path's only ring) and is delivered: SEND_RING_FULL stays 0.

static void test_gaps_ct_memory_construct_sets_every_payload_ring_count()
{
    ct_memory_poison_install( 0xA5 );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_memory_base_client( fake );

    const ct_memory_counts_t counts = ct_memory_counts( transport );
    bool counts_zero = true;
    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        if ( counts.rings[ring] != 0 )
        {
            printf( "mutation gap hunt 2/6355,6356: construct left payload_ring[%d].num_packets as the allocator's bytes (0x%llx), not 0\n",
                    ring, (unsigned long long) counts.rings[ring] );
            counts_zero = false;
        }
    }
    check( counts_zero );

    rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value,
                                                            rocketnet_test_salt, rocketnet_test_session_s2c_key );
    check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const uint64_t * values = rocketnet_client_transport_counters( transport )->values;
    const int sent_before = fake->num_sent;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( transport, 40, &context );
    if ( !frame || values[ROCKETNET_COUNTER_SEND_RING_FULL] != 0 )
    {
        printf( "mutation gap hunt 2/6355,6356: the first start_packet after create was refused (SEND_RING_FULL %llu): ring 0's count was never set\n",
                (unsigned long long) values[ROCKETNET_COUNTER_SEND_RING_FULL] );
    }
    check( frame != NULL );
    check( values[ROCKETNET_COUNTER_SEND_RING_FULL] == 0 );

    if ( frame )
    {
        memset( frame + context.payload_index, 0x5C, 40 );
        check( rocketnet_client_transport_finish_packet( transport, &context, 40 ) > 40 );
        rocketnet_client_transport_send_packets( transport );
        if ( fake->num_sent != sent_before + 1 )
        {
            printf( "mutation gap hunt 2/6355,6356: one payload committed after create, %d datagrams sent\n", fake->num_sent - sent_before );
        }
        check( fake->num_sent == sent_before + 1 );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    ct_memory_poison_remove();
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_construct_sets_every_payload_ring_count );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6360 (receive_index not reset)
//
// Construct sets the received batch's count and cursor to 0 under a
// poisoning allocator whose bytes read as a negative int, so a
// receive_packet before the first update finds nothing and reads no entry.

static void test_gaps_ct_memory_construct_sets_the_receive_cursor()
{
    ct_memory_poison_install( 0xA5 );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_memory_base_client( fake );

    const ct_memory_counts_t counts = ct_memory_counts( transport );
    if ( counts.received_num_packets != 0 || counts.received_receive_index != 0 )
    {
        printf( "mutation gap hunt 2/6360: construct left the received batch's count %d and cursor %d, not 0 and 0: receive_packet before the first update reads packet_bytes[%d]\n",
                counts.received_num_packets, counts.received_receive_index, counts.received_receive_index );
    }
    check( counts.received_num_packets == 0 );
    check( counts.received_receive_index == 0 );

    // only with the cursor at 0 is the call safe to make: a negative cursor reads far outside the batch

    if ( counts.received_receive_index == 0 && counts.received_num_packets == 0 )
    {
        const uint64_t * values = rocketnet_client_transport_counters( transport )->values;
        const uint64_t too_small = values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL];
        uint8_t buffer[1200];
        check( rocketnet_client_transport_receive_packet( transport, buffer, (int) sizeof( buffer ) ) == 0 );
        check( values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == too_small );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    ct_memory_poison_remove();
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_construct_sets_the_receive_cursor );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6362 (buffer loop from 1), hunt 2/6364 (no buffer count reset)
//
// Construct sets every receive buffer's count to 0 under a poisoning
// allocator. The BASE path never runs rocketnet_client_io_init (which zeroes
// them again on the I/O core path), and its first fill is buffer 0: one
// one-byte datagram from the server is stored at index 0 and the update's
// prefilter refuses it, PREFILTER_TOO_SHORT once.

static int ct_memory_one_datagram_left = 0;

static int ct_memory_one_datagram_receive( void *, rocketnet_address_t * from, uint8_t * data, int )
{
    if ( ct_memory_one_datagram_left <= 0 )
    {
        return 0;
    }
    ct_memory_one_datagram_left--;
    ct_memory_server_address( from );
    data[0] = 0;
    return 1;
}

static void test_gaps_ct_memory_construct_sets_every_receive_buffer_count()
{
    ct_memory_poison_install( 0xA5 );

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    ct_memory_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = ct_memory_time;

    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;
    operations.receive = ct_memory_one_datagram_receive;
    ct_memory_one_datagram_left = 0;

    ct_memory_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &operations, fake );
    check( transport != NULL );

    const ct_memory_counts_t counts = ct_memory_counts( transport );
    bool counts_zero = true;
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
    {
        if ( counts.buffers[i] != 0 )
        {
            printf( "mutation gap hunt 2/6362,6364: construct left receive_buffer[%d].num_packets as the allocator's bytes (%d), not 0: the BASE receive writes its first datagram at that index\n",
                    i, counts.buffers[i] );
            counts_zero = false;
        }
    }
    check( counts_zero );

    // only with every count at 0 is the receive safe to run: a garbage count is the index the datagram is written at

    if ( counts_zero )
    {
        const uint64_t * values = rocketnet_client_transport_counters( transport )->values;
        const uint64_t too_short = values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT];

        ct_memory_one_datagram_left = 1;
        rocketnet_client_transport_step_legacy_for_test( transport, 1000.0 );
        check( ct_memory_one_datagram_left == 0 );

        const ct_memory_counts_t filled = ct_memory_counts( transport );
        int stored = 0;
        for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
        {
            stored += filled.buffers[i];
        }
        if ( stored != 1 )
        {
            printf( "mutation gap hunt 2/6362,6364: one datagram received, the receive buffers count %d\n", stored );
        }
        check( stored == 1 );

        rocketnet_client_transport_update( transport, 1000.0 );
        if ( values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] != too_short + 1 )
        {
            printf( "mutation gap hunt 2/6362,6364: the one-byte datagram stored at index 0 was not processed once (PREFILTER_TOO_SHORT +%llu)\n",
                    (unsigned long long) ( values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] - too_short ) );
        }
        check( values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] == too_short + 1 );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;

    ct_memory_poison_remove();
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_construct_sets_every_receive_buffer_count );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6375 (the witness poisons from ring 1)
//
// The connection-reset witness poisons every ring, ring 0 included, before
// it runs the reset: a reset that forgot ring 0 is then caught by the
// witness. The reset resets counts only, so the poison is still in each
// ring's first slot afterwards; a witness that skipped ring 0 leaves ring
// 0's first slot as it was (a sealed payload of a known size here).

static void test_gaps_ct_memory_reset_witness_poisons_every_ring()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_memory_base_client( fake );

    rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value,
                                                            rocketnet_test_salt, rocketnet_test_session_s2c_key );

    // a sealed payload in ring 0, slot 0, not yet sent: its wire size is in the slot

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( transport, 40, &context );
    check( frame != NULL );
    int wire_bytes = 0;
    if ( frame )
    {
        memset( frame + context.payload_index, 0x5C, 40 );
        wire_bytes = rocketnet_client_transport_finish_packet( transport, &context, 40 );
    }
    check( wire_bytes > 40 );
    check( rocketnet_client_transport_ring_wire_bytes_for_test( transport, 0, 0 ) == wire_bytes );

    check( rocketnet_client_transport_reset_witness_for_test( transport ) );

    int poison = 0;
    memset( &poison, 0xA5, sizeof( poison ) );
    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        const int slot = rocketnet_client_transport_ring_wire_bytes_for_test( transport, ring, 0 );
        if ( slot != poison )
        {
            printf( "mutation gap hunt 2/6375: the reset witness did not poison payload_ring[%d] (slot 0 holds %d): a reset that forgot that ring would pass it\n",
                    ring, slot );
        }
        check( slot == poison );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_reset_witness_poisons_every_ring );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6398 (failure[1] cleared instead of failure[0])
//
// The clean check clears the caller's failure text when the state is clean:
// old text in the buffer does not survive a clean result, and a one-byte
// buffer is written at its only byte (an ASan leg sees a write past it).

static void test_gaps_ct_memory_clean_check_clears_the_failure_text()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_t * transport = ct_memory_base_client( fake );

    char failure[64];
    memset( failure, 'x', sizeof( failure ) );
    failure[sizeof( failure ) - 1] = '\0';

    check( rocketnet_client_transport_connection_state_clean_for_test( transport, failure, sizeof( failure ) ) );
    if ( failure[0] != '\0' )
    {
        printf( "mutation gap hunt 2/6398: a clean result left the caller's old failure text \"%.8s...\"\n", failure );
    }
    check( failure[0] == '\0' );

    // a one-byte buffer: the clear is its only byte, nothing past it

    char * one = (char *) malloc( 1 );
    check( one != NULL );
    if ( one )
    {
        one[0] = 'x';
        check( rocketnet_client_transport_connection_state_clean_for_test( transport, one, 1 ) );
        if ( one[0] != '\0' )
        {
            printf( "mutation gap hunt 2/6398: a clean result with a one-byte failure buffer left its byte '%c'\n", one[0] );
        }
        check( one[0] == '\0' );
        free( one );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_clean_check_clears_the_failure_text );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7905 (ack-stats seam skips protocol_init)
//
// The ack-stats seam's transport block comes from the allocator and its
// protocol (a buffer member construction does not zero) is initialized by
// rocketnet_client_protocol_init: with an endpoint whose rtt is 0 the getter
// falls back to the protocol's rtt estimate, which must be 0, not the
// allocator's bytes. The poison 0x41 reads as a double of about 2.3e6.

static rocketnet_ack_endpoint_t * ct_memory_zero_endpoint()
{
    rocketnet_ack_endpoint_t * endpoint = (rocketnet_ack_endpoint_t *) calloc( 1, sizeof( rocketnet_ack_endpoint_t ) );
    check( endpoint != NULL );
    if ( endpoint )
    {
        rocketnet_ack_endpoint_reset( endpoint );
    }
    return endpoint;
}

static void test_gaps_ct_memory_ack_stats_seam_initializes_the_protocol()
{
    rocketnet_ack_endpoint_t * endpoint = ct_memory_zero_endpoint();
    if ( !endpoint )
    {
        return;
    }
    check( endpoint->rtt == 0.0f );

    ct_memory_poison_install( 0x41 );

    rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    if ( stats.rtt != 0.0f )
    {
        printf( "mutation gap hunt 2/7905: the ack-stats seam reported rtt %g for an endpoint with no rtt: its protocol's rtt estimate is the allocator's bytes\n",
                (double) stats.rtt );
    }
    check( stats.rtt == 0.0f );

    ct_memory_poison_remove();
    free( endpoint );
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_ack_stats_seam_initializes_the_protocol );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7915 (no memset of the adopted message client),
// hunt 2/7914 (memset to 1)
//
// The message client the public ack-stats seam adopts is zeroed but for its
// link: its counters are zero, so every count rocketnet_client_stats sums
// from it is the transport's alone (all zero on a fresh transport), and its
// state is the transport's, DISCONNECTED, with no disconnect reason. The
// allocator poisons the block first, so a missing or wrong clear is seen.

static void test_gaps_ct_memory_ack_stats_public_client_is_zeroed()
{
    rocketnet_ack_endpoint_t * endpoint = ct_memory_zero_endpoint();
    if ( !endpoint )
    {
        return;
    }

    ct_memory_poison_install( 0xA5 );

    struct rocketnet_client_t * client = rocketnet_client_ack_stats_public_client_for_test( endpoint );
    check( client != NULL );
    if ( client )
    {
        struct rocketnet_client_stats_t stats;
        rocketnet_client_stats( client, &stats );

        const uint64_t counts[] = { stats.packets_sent, stats.packets_received, stats.bytes_sent, stats.bytes_received, stats.payloads_received,
                                    stats.messages_resent, stats.messages_duplicated, stats.receipts_acked, stats.receipts_lost, stats.receipts_dropped };
        const char * names[] = { "packets_sent", "packets_received", "bytes_sent", "bytes_received", "payloads_received",
                                 "messages_resent", "messages_duplicated", "receipts_acked", "receipts_lost", "receipts_dropped" };
        bool all_zero = true;
        for ( int i = 0; i < (int) ( sizeof( counts ) / sizeof( counts[0] ) ); i++ )
        {
            if ( counts[i] != 0 )
            {
                printf( "mutation gap hunt 2/7914,7915: the adopted message client is not zeroed: rocketnet_client_stats %s is %llu on a fresh transport\n",
                        names[i], (unsigned long long) counts[i] );
                all_zero = false;
            }
        }
        check( all_zero );

        // the state reads the message object's connection count; a garbage count is a garbage pointer, so only a zeroed client is asked

        if ( all_zero )
        {
            const enum rocketnet_client_state_t state = rocketnet_client_state( client );
            if ( state != ROCKETNET_CLIENT_STATE_DISCONNECTED )
            {
                printf( "mutation gap hunt 2/7914,7915: the adopted message client reports state %d, not DISCONNECTED\n", (int) state );
            }
            check( state == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        }

        rocketnet_client_ack_stats_public_client_for_test_free( client );
    }

    ct_memory_poison_remove();
    free( endpoint );
}

REGISTER_UNIT_TEST( test_gaps_ct_memory_ack_stats_public_client_is_zeroed );

#endif // #if ROCKETNET_DEV
