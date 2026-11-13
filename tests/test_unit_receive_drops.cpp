/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The receive drop reasons a transport counts itself
// (sdk/rocketnet_receive_prefilter.h): the counters each reason names, the
// per-worker tally (one writer, any reader), and the server transport's fold
// of a transport's receive_drop_totals into those counters.
//
// No sockets, no real time: the server transport runs over the loopback hub.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_receive_prefilter.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"
#include "rocketnet_driver.h"
#include "loopback_hub.h"
#include "unit_shutdown.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

static void test_receive_drop_counters_bank_full_is_its_own()
{
    int counters[ROCKETNET_RECEIVE_DROP_MAX_COUNTERS];
    check( rocketnet_receive_drop_counters( ROCKETNET_RECEIVE_DROP_BANK_FULL, counters ) == 1 );
    check( counters[0] == ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL );
    check( rocketnet_receive_drop_counters( ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED, counters ) == 1 );
    check( counters[0] == ROCKETNET_COUNTER_RECEIVE_FAILED );
    check( rocketnet_receive_drop_counters( ROCKETNET_RECEIVE_DROP_STOPPED, counters ) == 1 );
    check( counters[0] == ROCKETNET_COUNTER_RECEIVE_DROPPED_STOPPED );
    check( rocketnet_receive_drop_counters( ROCKETNET_RECEIVE_DROP_RETIRED, counters ) == 1 );
    check( counters[0] == ROCKETNET_COUNTER_RECEIVE_DROPPED_RETIRED );
    check( rocketnet_receive_drop_counters( ROCKETNET_RECEIVE_ACCEPT, counters ) == 0 );
}

#if ROCKETNET_DEV
// the DEV-only sample of refused datagrams (sdk/rocketnet_receive_prefilter.h):
// the report names source, length and the first 16 bytes, and keeps only the
// first ROCKETNET_RECEIVE_DROP_SAMPLES

static void test_receive_drop_samples_name_the_first_refused_datagrams()
{
    rocketnet_address_t from;
    check( rocketnet_address_parse( &from, "127.0.0.1:4242" ) );
    uint8_t data[40];
    for ( int i = 0; i < 40; i++ )
    {
        data[i] = (uint8_t) ( 0xA0 + i );
    }

    rocketnet_receive_drop_sample_record( ROCKETNET_RECEIVE_DROP_WRONG_VALUE, &from, data, 40 );
    const std::string one = rocketnet_receive_drop_samples_text();
    check( one.find( "127.0.0.1:4242" ) != std::string::npos );
    check( one.find( "length 40" ) != std::string::npos );
    check( one.find( "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf" ) != std::string::npos );

    for ( int i = 0; i < 2 * ROCKETNET_RECEIVE_DROP_SAMPLES; i++ )
    {
        rocketnet_receive_drop_sample_record( ROCKETNET_RECEIVE_DROP_WRONG_VALUE, &from, data, 40 );
    }
    size_t lines = 0;
    const std::string all = rocketnet_receive_drop_samples_text();
    for ( size_t at = all.find( "refused datagram" ); at != std::string::npos; at = all.find( "refused datagram", at + 1 ) )
    {
        lines++;
    }
    check( lines == ROCKETNET_RECEIVE_DROP_SAMPLES );
}

REGISTER_UNIT_TEST( test_receive_drop_samples_name_the_first_refused_datagrams );
#endif // #if ROCKETNET_DEV

static void test_receive_drop_tally_counts_and_sums()
{
    static struct rocketnet_receive_drop_tally_t tallies[3];
    for ( int t = 0; t < 3; ++t )
    {
        rocketnet_receive_drop_tally_reset( &tallies[t] );
    }
    rocketnet_receive_drop_tally_count( &tallies[0], ROCKETNET_RECEIVE_DROP_TOO_SHORT );
    rocketnet_receive_drop_tally_count( &tallies[1], ROCKETNET_RECEIVE_DROP_TOO_SHORT );
    rocketnet_receive_drop_tally_count( &tallies[2], ROCKETNET_RECEIVE_DROP_BANK_FULL, 7 );
    rocketnet_receive_drop_tally_count( &tallies[2], ROCKETNET_RECEIVE_DROP_ACK_WRONG_SIZE );

    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS] = {};
    rocketnet_receive_drop_tally_sum( tallies, 3, totals );
    check( totals[ROCKETNET_RECEIVE_ACCEPT] == 0 );
    check( totals[ROCKETNET_RECEIVE_DROP_TOO_SHORT] == 2 );
    check( totals[ROCKETNET_RECEIVE_DROP_BANK_FULL] == 7 );
    check( totals[ROCKETNET_RECEIVE_DROP_ACK_WRONG_SIZE] == 1 );
    uint64_t sum = 0;
    for ( int i = 0; i < ROCKETNET_RECEIVE_NUM_DROPS; ++i )
    {
        sum += totals[i];
    }
    check( sum == 10 );
    check( alignof( struct rocketnet_receive_drop_tally_t ) == 64 && sizeof( struct rocketnet_receive_drop_tally_t ) % 64 == 0 );
}

#if ROCKETNET_DEV
// the server transport's fold: a fake io's receive_drop_totals, read on each
// counters read, lands its growth on the named counters and nothing else

static uint64_t rp_fake_totals[ROCKETNET_RECEIVE_NUM_DROPS];

static void rp_fake_receive_drop_totals( void *, uint64_t * values )
{
    memcpy( values, rp_fake_totals, sizeof( rp_fake_totals ) );
}

static void test_server_transport_folds_receive_drop_totals()
{
    rocketnet_quiet( 1 );

    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.max_clients = 1;
    server_config.protocol_id = 0x666f6c6464726f70ULL;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    check( server != NULL );

    struct rocketnet_server_transport_io_operations_t operations = {};
    const struct rocketnet_server_transport_io_operations_t * swapped_operations = &operations;
    void * swapped_instance = NULL;
    rocketnet_server_transport_swap_io_for_test( server, &swapped_operations, &swapped_instance );
    const struct rocketnet_server_transport_io_operations_t * real_operations = swapped_operations;
    void * const real_instance = swapped_instance;
    operations = *real_operations;
    operations.receive_drop_totals = rp_fake_receive_drop_totals;
    swapped_operations = &operations;
    rocketnet_server_transport_swap_io_for_test( server, &swapped_operations, &swapped_instance );
    swapped_instance = real_instance;

    memset( rp_fake_totals, 0, sizeof( rp_fake_totals ) );
    const struct rocketnet_counters_t before = *rocketnet_server_transport_counters( server );

    rp_fake_totals[ROCKETNET_RECEIVE_DROP_WRONG_VALUE] = 3;
    rp_fake_totals[ROCKETNET_RECEIVE_DROP_PAYLOAD_WRONG_SIZE] = 2;
    rp_fake_totals[ROCKETNET_RECEIVE_DROP_BANK_FULL] = 5;
    const struct rocketnet_counters_t middle = *rocketnet_server_transport_counters( server );
    rocketnet_counters_expect( &before, &middle,
                               { { ROCKETNET_COUNTER_PACKETS_RECEIVED, 5 },
                                 { ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE, 3 },
                                 { ROCKETNET_COUNTER_PAYLOAD_RECEIVED, 2 },
                                 { ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE, 2 },
                                 { ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL, 5 } } );

    // a second read with no growth moves nothing; growth adds only the growth
    const struct rocketnet_counters_t again = *rocketnet_server_transport_counters( server );
    rocketnet_counters_expect( &middle, &again, {} );
    rp_fake_totals[ROCKETNET_RECEIVE_DROP_BANK_FULL] = 6;
    const struct rocketnet_counters_t after = *rocketnet_server_transport_counters( server );
    rocketnet_counters_expect( &again, &after, { { ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL, 1 } } );

    // a datagram a transport received and never handed to main because it
    // stopped is RECEIVE_DROPPED_STOPPED alone
    rp_fake_totals[ROCKETNET_RECEIVE_DROP_STOPPED] = 4;
    const struct rocketnet_counters_t stopped = *rocketnet_server_transport_counters( server );
    rocketnet_counters_expect( &after, &stopped, { { ROCKETNET_COUNTER_RECEIVE_DROPPED_STOPPED, 4 } } );

    // a datagram queued at a client socket the transport retired is
    // RECEIVE_DROPPED_RETIRED alone
    rp_fake_totals[ROCKETNET_RECEIVE_DROP_RETIRED] = 3;
    const struct rocketnet_counters_t retired = *rocketnet_server_transport_counters( server );
    rocketnet_counters_expect( &stopped, &retired, { { ROCKETNET_COUNTER_RECEIVE_DROPPED_RETIRED, 3 } } );

    swapped_operations = real_operations;
    rocketnet_server_transport_swap_io_for_test( server, &swapped_operations, &swapped_instance );
    check( swapped_operations == &operations );
    rn_test_server_transport_stop_and_destroy( server );
}
#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_receive_drop_counters_bank_full_is_its_own );
REGISTER_UNIT_TEST( test_receive_drop_tally_counts_and_sums );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_server_transport_folds_receive_drop_totals );
#endif // #if ROCKETNET_DEV
