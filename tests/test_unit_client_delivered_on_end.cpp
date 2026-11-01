/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A payload the client transport counts CLIENT_PAYLOAD_DELIVERED reaches the
// caller, also when the connection ends in the same update or the caller
// disconnects before reading it: the connection reset keeps the batch, and
// the next update clears it (proofs/tla/RNClientDelivery.tla
// CountedImpliesHanded and OfferedOnce). One update processes no more packets
// than the batch holds, so every payload counted delivered is placed, and a
// burst larger than the batch spreads across updates (SlotForNextPacket).
// The cases run on the in-memory
// loopback transport with a step-driven client, time passed as an argument
// from 1000.0, and no socket.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "portable_crt.h"

#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

#if ROCKETNET_DEV
static double delivered_on_end_clock = 1000.0;

static double delivered_on_end_time_callback( void * )
{
    return delivered_on_end_clock;
}

struct delivered_on_end_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    double time;
};

static void delivered_on_end_setup( struct delivered_on_end_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    delivered_on_end_clock = pair->time;

    const uint64_t protocol_id = 0x64656c6976657265ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    pair->server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    check( pair->server != NULL );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( pair->server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x5eed0fdea1e7ef00ULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = delivered_on_end_time_callback;

    pair->client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), pair->time );
    check( pair->client != NULL );
}

static void delivered_on_end_step( struct delivered_on_end_pair_t * pair )
{
    pair->time += 0.01;
    delivered_on_end_clock = pair->time;
    rocketnet_server_transport_update( pair->server, pair->time );
    rocketnet_client_transport_update( pair->client, pair->time );
    rocketnet_server_transport_send_packets( pair->server );
    rocketnet_client_transport_send_packets( pair->client );
}

static void delivered_on_end_connect( struct delivered_on_end_pair_t * pair )
{
    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_is_client_connected( pair->server, 0 ) )
            break;
        delivered_on_end_step( pair );
    }

    check( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_server_transport_is_client_connected( pair->server, 0 ) );

    // settle: whatever the handshake left in flight is delivered and read

    for ( int i = 0; i < 4; i++ )
        delivered_on_end_step( pair );
}

static uint64_t delivered_on_end_delivered( struct delivered_on_end_pair_t * pair )
{
    return rocketnet_client_transport_counters( pair->client )->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED];
}

// the client's next update, without the client's send: the time moves on and
// nothing the client would send is needed for what is checked

static void delivered_on_end_client_update( struct delivered_on_end_pair_t * pair )
{
    pair->time += 0.01;
    delivered_on_end_clock = pair->time;
    rocketnet_client_transport_update( pair->client, pair->time );
}

static const uint8_t delivered_on_end_final[] = { 'f', 'i', 'n', 'a', 'l', ' ', 's', 't', 'a', 't', 'e' };

// The server's final payload and its disconnect arrive in one client update:
// that update counts the payload delivered and ends the connection, and the
// caller still reads the payload from the batch, by index and by the cursor.
// The next update clears the batch and offers nothing again.

static void test_client_payload_delivered_in_ending_update_reaches_caller()
{
    struct delivered_on_end_pair_t pair;
    delivered_on_end_setup( &pair );
    delivered_on_end_connect( &pair );

    rocketnet_server_transport_send_packet( pair.server, 0, delivered_on_end_final, (int) sizeof( delivered_on_end_final ) );
    rocketnet_server_transport_send_packets( pair.server );
    rocketnet_server_transport_disconnect_client( pair.server, 0 );
    rocketnet_server_transport_send_packets( pair.server );

    const uint64_t delivered_before = delivered_on_end_delivered( &pair );

    delivered_on_end_client_update( &pair );

    // the connection ended inside the update that delivered the payload

    check( rocketnet_client_transport_state( pair.client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( rocketnet_client_transport_counters( pair.client )->values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED] == 1 );
    check( delivered_on_end_delivered( &pair ) == delivered_before + 1 );

    // counted delivered is handed to the caller

    check( rocketnet_client_transport_num_payloads( pair.client ) == 1 );

    int payload_bytes = 0;
    const uint8_t * payload = rocketnet_client_transport_payload( pair.client, 0, &payload_bytes );
    check( payload != NULL );
    check( payload_bytes == (int) sizeof( delivered_on_end_final ) );
    check( payload && memcmp( payload, delivered_on_end_final, sizeof( delivered_on_end_final ) ) == 0 );

    uint8_t copy[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    check( rocketnet_client_transport_receive_packet( pair.client, copy, (int) sizeof( copy ) ) == (int) sizeof( delivered_on_end_final ) );
    check( memcmp( copy, delivered_on_end_final, sizeof( delivered_on_end_final ) ) == 0 );
    check( rocketnet_client_transport_receive_packet( pair.client, copy, (int) sizeof( copy ) ) == 0 );

    // the next update clears the batch: nothing is offered twice, nothing more is counted

    delivered_on_end_client_update( &pair );

    check( rocketnet_client_transport_num_payloads( pair.client ) == 0 );
    check( rocketnet_client_transport_receive_packet( pair.client, copy, (int) sizeof( copy ) ) == 0 );
    check( delivered_on_end_delivered( &pair ) == delivered_before + 1 );

    rn_test_client_transport_stop_and_destroy( pair.client );
    rn_test_server_transport_stop_and_destroy( pair.server );
}

REGISTER_UNIT_TEST( test_client_payload_delivered_in_ending_update_reaches_caller );

// The caller's own disconnect between updates runs the same reset: the batch
// the last update delivered stays readable until the next update.

static void test_client_payload_delivered_before_disconnect_reaches_caller()
{
    struct delivered_on_end_pair_t pair;
    delivered_on_end_setup( &pair );
    delivered_on_end_connect( &pair );

    rocketnet_server_transport_send_packet( pair.server, 0, delivered_on_end_final, (int) sizeof( delivered_on_end_final ) );
    rocketnet_server_transport_send_packets( pair.server );

    const uint64_t delivered_before = delivered_on_end_delivered( &pair );

    delivered_on_end_client_update( &pair );

    check( rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( delivered_on_end_delivered( &pair ) == delivered_before + 1 );

    rocketnet_client_transport_disconnect( pair.client );

    check( rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( rocketnet_client_transport_num_payloads( pair.client ) == 1 );

    uint8_t copy[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    check( rocketnet_client_transport_receive_packet( pair.client, copy, (int) sizeof( copy ) ) == (int) sizeof( delivered_on_end_final ) );
    check( memcmp( copy, delivered_on_end_final, sizeof( delivered_on_end_final ) ) == 0 );

    delivered_on_end_client_update( &pair );

    check( rocketnet_client_transport_num_payloads( pair.client ) == 0 );
    check( delivered_on_end_delivered( &pair ) == delivered_before + 1 );

    rn_test_client_transport_stop_and_destroy( pair.client );
    rn_test_server_transport_stop_and_destroy( pair.server );
}

REGISTER_UNIT_TEST( test_client_payload_delivered_before_disconnect_reaches_caller );

// A burst of one payload per packet larger than the batch: the first update
// fills the batch to capacity, every payload it counts delivered is placed,
// and the rest arrive in the next update, in order, none dropped.

#define DELIVERED_ON_END_BURST ( ROCKETNET_NUM_CLIENT_PACKETS + 176 )

static void test_client_payload_burst_past_batch_capacity_spreads_across_updates()
{
    struct delivered_on_end_pair_t pair;
    delivered_on_end_setup( &pair );
    delivered_on_end_connect( &pair );

    for ( int k = 0; k < DELIVERED_ON_END_BURST; k++ )
    {
        const uint8_t payload[4] = { (uint8_t) k, (uint8_t) ( k >> 8 ), 0xB5, 0x7E };
        rocketnet_server_transport_send_packet( pair.server, 0, payload, (int) sizeof( payload ) );
        if ( k % 64 == 63 )
            rocketnet_server_transport_send_packets( pair.server );
    }
    rocketnet_server_transport_send_packets( pair.server );

    const uint64_t delivered_before = delivered_on_end_delivered( &pair );

    int next = 0;
    int full_updates = 0;

    for ( int update = 0; update < 8 && next < DELIVERED_ON_END_BURST; update++ )
    {
        const uint64_t before = delivered_on_end_delivered( &pair );

        delivered_on_end_client_update( &pair );

        const int num = rocketnet_client_transport_num_payloads( pair.client );

        // counted delivered this update is exactly what the batch holds

        check( num <= ROCKETNET_NUM_CLIENT_PACKETS );
        check( delivered_on_end_delivered( &pair ) - before == (uint64_t) num );

        if ( num == ROCKETNET_NUM_CLIENT_PACKETS )
            full_updates++;

        for ( int i = 0; i < num; i++ )
        {
            int bytes = 0;
            const uint8_t * data = rocketnet_client_transport_payload( pair.client, i, &bytes );
            check( data != NULL && bytes == 4 );
            check( data && ( (int) data[0] | ( (int) data[1] << 8 ) ) == next );
            check( data && data[2] == 0xB5 && data[3] == 0x7E );
            next++;
        }
    }

    check( full_updates == 1 );
    check( next == DELIVERED_ON_END_BURST );
    check( delivered_on_end_delivered( &pair ) - delivered_before == DELIVERED_ON_END_BURST );

    rn_test_client_transport_stop_and_destroy( pair.client );
    rn_test_server_transport_stop_and_destroy( pair.server );
}

REGISTER_UNIT_TEST( test_client_payload_burst_past_batch_capacity_spreads_across_updates );
#endif // #if ROCKETNET_DEV
