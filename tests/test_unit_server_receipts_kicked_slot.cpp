/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Server receipts for a slot its connection has ended (tla/RNReceipts.tla
// TakeReceipt, which stays takeable after Disconnect): the outcomes an
// update recorded for a slot stay readable after that same update kicks
// the slot, and after rocketnet_server_disconnect_client ends it between
// updates, until the slot's next update begins. Everything runs in one
// process over the in-memory loopback transport with step-driven
// clients: no socket, no thread, no sleep, and time is an argument
// starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_counters.h"
#include "rocketnet_internal.h"

#include <stdio.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check
#define RECEIPTS_KICKED_PROTOCOL 0x5245434B49434B01ULL

#if ROCKETNET_DEV
struct receipts_kicked_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static double receipts_kicked_now = 1000.0;
static double receipts_kicked_clock( void * ) { return receipts_kicked_now; }

static void receipts_kicked_identity( receipts_kicked_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_t * receipts_kicked_server_create( receipts_kicked_identity_t * identity, bool short_reliable_timeout )
{
    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = RECEIPTS_KICKED_PROTOCOL;
    config.max_clients = 2;
    memcpy( config.backend_public_key, identity->backend_public, sizeof( config.backend_public_key ) );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    if ( short_reliable_timeout )
    {
        // a reliable send the client never acknowledges ends its
        // connection one second after its first send
        config.message_resend_delay_initial_ms = 10;
        config.message_resend_delay_min_ms = 10;
        config.message_resend_delay_max_ms = 100;
        config.reliable_message_timeout_ms = 1000;
    }
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( server != NULL );
    rocketnet_server_public_key( server, identity->server_public );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    return server;
}

static rocketnet_client_t * receipts_kicked_client_create()
{
    rocketnet_client_test_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = true;
    config.time_callback = receipts_kicked_clock;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL );
    check( client != NULL );
    return client;
}

static void receipts_kicked_token( const receipts_kicked_identity_t * identity, uint16_t port, uint64_t client_id, char * token_string )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
                                                     RECEIPTS_KICKED_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

static void receipts_kicked_pump( rocketnet_server_t * server, rocketnet_client_t * client, double * time )
{
    *time += 0.01;
    receipts_kicked_now = *time;
    rocketnet_client_update( client, *time );
    rocketnet_client_send_packets( client );
    if ( server && rocketnet_server_running( server ) )
    {
        rocketnet_server_update( server, *time );
        rocketnet_server_send_packets( server );
    }
}

static void receipts_kicked_drain_client( rocketnet_client_t * client )
{
    while ( rocketnet_client_receive_messages( client ) != NULL )
    {
    }
}

// the client alone: its packet ack reaches the server's transport, and
// no server update applies it yet
static void receipts_kicked_pump_client_only( rocketnet_client_t * client, double * time )
{
    *time += 0.01;
    receipts_kicked_now = *time;
    rocketnet_client_update( client, *time );
    receipts_kicked_drain_client( client );
    rocketnet_client_send_packets( client );
}

static int receipts_kicked_connect( const receipts_kicked_identity_t * identity, rocketnet_server_t * server,
                                    rocketnet_client_t * client, uint64_t client_id, double * time )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    receipts_kicked_token( identity, rocketnet_server_port( server ), client_id, token );
    check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        receipts_kicked_pump( server, client, time );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int index = rocketnet_client_index( client );
    check( index >= 0 );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( server, index ); i++ )
        receipts_kicked_pump( server, client, time );
    check( rocketnet_server_client_connected( server, index ) );
    for ( int i = 0; i < 4; i++ )
        receipts_kicked_pump( server, client, time );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    return index;
}

static uint32_t receipts_kicked_send_receipt_message( rocketnet_server_t * server, int index, uint8_t value )
{
    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 3 );
    check( p != NULL );
    p[0] = value;
    p[1] = 2;
    p[2] = 3;
    const uint32_t serial = rocketnet_server_finish_message( server, 3 );
    check( serial != 0 );
    rocketnet_server_send_packets( server );
    return serial;
}

// One server update both applies the ack of the first receipt message
// and kicks the slot for the second, which its client never
// acknowledges past the reliable timeout. The ACKED outcome the kicking
// update recorded stays readable until the next update begins.
static void test_server_receipts_after_kick_same_update()
{
    rocketnet_quiet( 1 );
    receipts_kicked_identity_t identity;
    receipts_kicked_identity( &identity );
    double time = 1000.0;
    receipts_kicked_now = time;
    rocketnet_server_t * server = receipts_kicked_server_create( &identity, true );
    rocketnet_client_t * client = receipts_kicked_client_create();
    const int index = receipts_kicked_connect( &identity, server, client, 0x7A01ULL, &time );

    // the first receipt message is acknowledged by the client; its ack
    // waits at the server's transport, not yet applied by an update
    const uint32_t acked_serial = receipts_kicked_send_receipt_message( server, index, 1 );
    for ( int i = 0; i < 4; i++ )
        receipts_kicked_pump_client_only( client, &time );

    // the second receipt message is never acknowledged: the client is
    // not pumped again, so its packet ack never reaches the server
    const uint32_t lost_serial = receipts_kicked_send_receipt_message( server, index, 2 );
    check( lost_serial != acked_serial );

    // one update, past the second message's reliable timeout: it applies
    // the first message's ack and kicks the slot for the second
    time += 1.5;
    receipts_kicked_now = time;
    rocketnet_server_update( server, time );
    check( !rocketnet_server_client_connected( server, index ) );
    // the kicking update wrote the first message's ACKED outcome
    check( rocketnet_counter_value( rocketnet_message_server_counters( rocketnet_server_message_server( server ) ), ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ) >= 1 );

    rocketnet_receipt_t out[4];
    memset( out, 0, sizeof( out ) );
    check( rocketnet_server_receipts( server, index, out, 4 ) == 1 );
    check( out[0].serial == acked_serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );

    // the next update begins: the kicked slot's outcomes are gone
    time += 0.01;
    receipts_kicked_now = time;
    rocketnet_server_update( server, time );
    check( rocketnet_server_receipts( server, index, out, 4 ) == 0 );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_server_receipts_after_kick_same_update );

// rocketnet_server_disconnect_client between the update that records an
// outcome and the read: the outcomes recorded before it stay readable
// until the slot's next update begins.
static void test_server_receipts_after_disconnect_client()
{
    rocketnet_quiet( 1 );
    receipts_kicked_identity_t identity;
    receipts_kicked_identity( &identity );
    double time = 1000.0;
    receipts_kicked_now = time;
    rocketnet_server_t * server = receipts_kicked_server_create( &identity, false );
    rocketnet_client_t * client = receipts_kicked_client_create();
    const int index = receipts_kicked_connect( &identity, server, client, 0x7A02ULL, &time );

    const uint32_t serial = receipts_kicked_send_receipt_message( server, index, 3 );
    for ( int i = 0; i < 4; i++ )
        receipts_kicked_pump_client_only( client, &time );
    // one server update records the ACKED outcome for the slot; the
    // application ends the connection before reading it
    time += 0.01;
    receipts_kicked_now = time;
    rocketnet_server_update( server, time );
    rocketnet_server_disconnect_client( server, index );
    check( !rocketnet_server_client_connected( server, index ) );

    rocketnet_receipt_t out[4];
    memset( out, 0, sizeof( out ) );
    check( rocketnet_server_receipts( server, index, out, 4 ) == 1 );
    check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );

    // the next update begins: the ended slot's outcomes are gone
    time += 0.01;
    receipts_kicked_now = time;
    rocketnet_server_update( server, time );
    check( rocketnet_server_receipts( server, index, out, 4 ) == 0 );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_server_receipts_after_disconnect_client );
#endif // #if ROCKETNET_DEV
