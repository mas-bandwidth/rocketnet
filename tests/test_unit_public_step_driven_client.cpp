/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public step-driven clients use only rocketnet.h for their success paths.
// The loopback transport carries packets in memory; callers provide every
// update time from 1000.0, and no test sleeps or opens a socket.
//
// The connect-without-a-thread case moved to the functional suite
// (tests/functional/process_thread_counts.cpp): reading the process's thread count
// from /proc/self/task is OS state. The pair setup is shared from
// tests/public_step_pair.h.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "fake_client_driver.h"
#include "portable_crt.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_config.h"
#include "rocketnet_client_io.h"       // refusal fixture for a transport with no non-blocking step

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

#include "public_step_pair.h"

static void test_public_step_driven_client_delivers_both_ways()
{
    public_step_identity_t identity;
    public_step_identity( &identity );

    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity );
    public_step_connect( &pair );

    const uint8_t client_messages[2][4] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 } };
    const uint8_t server_messages[2][4] = { { 9, 10, 11, 12 }, { 13, 14, 15, 16 } };

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        check( out != NULL );

        memcpy( out, client_messages[i], 4 );

        check( rocketnet_client_finish_message( pair.client, 4 ) != 0 );

        out = rocketnet_server_start_message( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        check( out != NULL );

        memcpy( out, server_messages[i], 4 );

        check( rocketnet_server_finish_message( pair.server, 4 ) != 0 );
    }

    int at_server = 0, at_client = 0;
    for ( int step = 0; step < 400 && ( at_server < 2 || at_client < 2 ); step++ )
    {
        public_step_pump( &pair );

        const uint8_t * data = NULL;
        int bytes = 0;
        uint64_t sender = ~0ULL;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( pair.server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
                sender = (uint64_t) batch->client_index[i];
                check( sender == 0 );
                check( at_server < 2 && bytes == 4 && memcmp( data, client_messages[at_server], 4 ) == 0 );

                at_server++;
            }
        }

        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( pair.client ) ) != NULL )
        {
            for ( int i = 0; i < incoming->num_messages; i++ )
            {
                data = incoming->data[i];
                bytes = incoming->bytes[i];
                check( at_client < 2 && bytes == 4 && memcmp( data, server_messages[at_client], 4 ) == 0 );

                at_client++;
            }
        }
    }

    check( at_server == 2 && at_client == 2 );

    public_step_pair_destroy( &pair );
}

REGISTER_UNIT_TEST( test_public_step_driven_client_delivers_both_ways );

// The deterministic trace: the client's counters in the stats struct's
// order, then the server's delivery-relevant counters, then the verified
// server delivery count and byte total. A run that delivers nothing fails
// inside the trace, so two empty traces can never compare equal and pass.
#define PUBLIC_STEP_NUM_COUNTERS 19

static void public_step_trace( const public_step_identity_t * identity, uint16_t port,
                               uint64_t * counters )
{
    public_step_pair_t pair;
    public_step_pair_create( &pair, identity, port );
    public_step_connect( &pair );

    int delivered = 0;
    uint64_t delivered_bytes = 0;
    for ( int i = 0; i < 8; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
        check( out != NULL );

        out[0] = (uint8_t) i;

        check( rocketnet_client_finish_message( pair.client, 1 ) != 0 );

        public_step_pump( &pair );

        // every delivery is received before the next update (standard/MESSAGES.md m1.6)

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( pair.server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                check( delivered < 8 );
                check( batch->bytes[m] == 1 );
                check( batch->data[m][0] == (uint8_t) delivered );
                check( batch->client_index[m] == 0 );

                delivered++;
                delivered_bytes += (uint64_t) batch->bytes[m];
            }
        }

        while ( rocketnet_client_receive_messages( pair.client ) != NULL )
        {
        }
    }

    check( delivered == 8 );
    check( delivered_bytes == 8 );

    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( pair.client, &stats );
    check( stats.packets_sent > 0 );
    check( stats.packets_received > 0 );
    check( stats.bytes_sent > 0 );
    check( stats.bytes_received > 0 );

    struct rocketnet_server_stats_t server_stats;
    rocketnet_server_stats( pair.server, &server_stats );
    check( server_stats.packets_sent > 0 );
    check( server_stats.packets_received > 0 );
    check( server_stats.bytes_sent > 0 );
    check( server_stats.bytes_received > 0 );
    check( server_stats.payloads_received > 0 );
    check( server_stats.connections_accepted == 1 );

    const uint64_t values[PUBLIC_STEP_NUM_COUNTERS] = { stats.packets_sent, stats.packets_received, stats.bytes_sent,
                                                        stats.bytes_received, stats.payloads_received, stats.messages_resent, stats.messages_duplicated,
                                                        stats.receipts_acked, stats.receipts_lost, stats.messages_dropped_over_budget, stats.messages_dropped_not_connected,
                                                        server_stats.packets_sent, server_stats.packets_received, server_stats.bytes_sent,
                                                        server_stats.bytes_received, server_stats.payloads_received, server_stats.connections_accepted,
                                                        (uint64_t) delivered, delivered_bytes };
    memcpy( counters, values, sizeof( values ) );
    public_step_pair_destroy( &pair );
}

static void test_public_step_driven_client_same_inputs_same_trace()
{
    public_step_identity_t identity;
    public_step_identity( &identity );

    const uint16_t port = 52341;
    uint64_t first[PUBLIC_STEP_NUM_COUNTERS] = {};
    uint64_t second[PUBLIC_STEP_NUM_COUNTERS] = {};
    public_step_trace( &identity, port, first );
    public_step_trace( &identity, port, second );
    check( memcmp( first, second, sizeof( first ) ) == 0 );
}

REGISTER_UNIT_TEST( test_public_step_driven_client_same_inputs_same_trace );

static void test_public_step_driven_client_refused_on_a_threaded_only_transport()
{
    public_step_identity_t identity;
    public_step_identity( &identity );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_step_token( 40000, identity.server_public, &identity, token );

    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.step_driven = true;

    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_ex(
        &fake, token, &config, 1000.0, &rocketnet_fake_client_operations_base, &fake, &status );
    check( transport == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    check( fake.destroy_calls == 1 );
}

REGISTER_UNIT_TEST( test_public_step_driven_client_refused_on_a_threaded_only_transport );

static int public_step_setenv( const char * name, const char * value )
{
    return rocketnet_test_env_setenv( name, value, 1 );
}

static int public_step_unsetenv( const char * name )
{
    return rocketnet_test_env_unsetenv( name );
}

static void test_public_step_driven_client_env_selects_the_mode()
{
    char saved[64] = {};
    const bool present = rocketnet_test_env_getenv( "ROCKETNET_CLIENT_STEP_DRIVEN", saved, sizeof( saved ) ) && saved[0] != '\0';

    rocketnet_client_transport_config_t config;
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &num_fields );
    check( public_step_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", "1" ) == 0 );

    rocketnet_client_transport_default_config( &config );
    check( rocketnet_config_apply_env( "client", &config, fields, num_fields ) );
    check( config.step_driven == 1 );
    check( public_step_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", "0" ) == 0 );

    rocketnet_client_transport_default_config( &config );
    check( rocketnet_config_apply_env( "client", &config, fields, num_fields ) );
    check( config.step_driven == 0 );
    if ( present )
        check( public_step_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", saved ) == 0 );
    else
        check( public_step_unsetenv( "ROCKETNET_CLIENT_STEP_DRIVEN" ) == 0 );
}

REGISTER_UNIT_TEST( test_public_step_driven_client_env_selects_the_mode );

static void test_public_step_driven_client_loss_100_drops_everything()
{
    rocketnet_public_link_simulator_test_development( 1 );

    public_step_identity_t identity;
    public_step_identity( &identity );

    rocketnet_quiet( 1 );

    rocketnet_server_config_t server_config;
    public_step_server_config( &server_config, &identity );
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    public_step_token_string( server, &identity, token );

    rocketnet_client_config_t client_config;
    memset( &client_config, 0, sizeof( client_config ) );
    public_step_client_config( &client_config );

    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );

    rocketnet_client_network_simulator( client, 0, 0, 100, 0 );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    double now = 1000.0;
    for ( int i = 0; i < 50; i++ )
    {
        public_step_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        now += 0.01;
    }

    const uint64_t packets_received = rn_client_stats_of( client ).packets_received;
    check( packets_received == 0 );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    rocketnet_public_link_simulator_test_development( 0 );
}

REGISTER_UNIT_TEST( test_public_step_driven_client_loss_100_drops_everything );

#endif // #if ROCKETNET_DEV
