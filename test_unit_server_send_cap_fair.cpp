/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's per-update payload send capacity is shared fairly between its
// connections (tla/RNServerSendFair.tla). The cases run a message server and
// two or three message clients on the loopback driver, step-driven, with time
// passed as an argument from 1000.0 and advanced 0.1 per tick (10 Hz): no
// socket, no sleep, no wall clock.
//
// server_send_cap_one_is_fair_two_clients and _three_clients set the capacity
// to ONE payload packet per update (rocketnet_server_transport_set_send_capacity_for_test),
// far below the client count, so a server that serves its connections from
// index 0 every update hands the one packet to the first connection forever.
// For 60 s of test time every client and the server send one reliable ordered
// and one unreliable message per tick each way; then 30 s with no new sends
// drain the queues. Each client must connect, receive, never disconnect, and
// receive every reliable message in order.
//
// server_send_capacity_default_covers_max_clients checks that the default
// capacity, with no test override, is at least the configured max clients at
// ROCKETNET_MAX_CLIENTS, and that the fair case also holds at the default.

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define FAIR_MAX_CLIENTS        3
#define FAIR_CHANNEL            0           // unordered deliveries carry no channel (rocketnet_channel_reliable.cpp:98), so both kinds share 0
#define FAIR_KIND_RELIABLE      1u
#define FAIR_KIND_UNRELIABLE    2u
#define FAIR_DT                 0.1
#define FAIR_SEND_TICKS         600         // 60 s at 10 Hz
#define FAIR_DRAIN_TICKS        300         // 30 s with no new sends
#define FAIR_CLIENT_ID_BASE     0x5EED0000ULL

static double fair_clock = 1000.0;

static double fair_time_callback( void * )
{
    return fair_clock;
}

struct fair_client_t
{
    struct rocketnet_message_client_t * client;
    int connected_tick;                 // the tick both ends first saw the connection, or -1
    int disconnect_tick;                // the tick the client first left CONNECTED after connecting, or -1
    int client_disconnect_reason;
    int server_disconnect_reason;       // the server's reason when the client's slot first ended, or NONE
    int server_disconnect_tick;         // the tick the client's slot first ended on the server, or -1
    uint32_t client_sent_reliable;      // client to server
    uint32_t server_got_reliable;
    uint32_t server_got_unreliable;
    uint32_t server_sent_reliable;      // server to client
    uint32_t client_got_reliable;
    uint32_t client_got_unreliable;
    int first_receive_tick;             // the first tick the client received anything from the server, or -1
    bool in_order;                      // every reliable message, each way, arrived as the next in sequence
};

struct fair_run_t
{
    int num_clients;
    struct fair_client_t clients[FAIR_MAX_CLIENTS];
    int slot_client[FAIR_MAX_CLIENTS];  // the client each server slot last held, or -1
    uint64_t server_send_ring_full;
};

static void fair_write( uint8_t bytes[8], uint32_t kind, uint32_t sequence )
{
    memcpy( bytes, &kind, 4 );
    memcpy( bytes + 4, &sequence, 4 );
}

static uint32_t fair_read_kind( const uint8_t * bytes )
{
    uint32_t kind;
    memcpy( &kind, bytes, 4 );
    return kind;
}

static uint32_t fair_read_sequence( const uint8_t * bytes )
{
    uint32_t sequence;
    memcpy( &sequence, bytes + 4, 4 );
    return sequence;
}

static int fair_client_for_slot( struct rocketnet_message_server_t * server, int slot, int num_clients )
{
    const uint64_t id = rocketnet_server_transport_client_id( rocketnet_message_server_transport( server ), slot );
    if ( id < FAIR_CLIENT_ID_BASE || id >= FAIR_CLIENT_ID_BASE + (uint64_t) num_clients )
        return -1;
    return (int) ( id - FAIR_CLIENT_ID_BASE );
}

// capacity 0 is the default; anything else is set through the test seam
static void fair_run( int num_clients, int capacity, struct fair_run_t * run )
{
    check( num_clients >= 1 && num_clients <= FAIR_MAX_CLIENTS );
    memset( run, 0, sizeof( *run ) );
    run->num_clients = num_clients;

    fair_clock = 1000.0;
    double time = fair_clock;

    const uint64_t protocol_id = 0x666169726361700aULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = num_clients;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.driver, sizeof( server_config.driver ), "%s", "loopback" );

    rocketnet_server_transport_set_send_capacity_for_test( capacity );
    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "127.0.0.1:0", &server_config, NULL, NULL, 0, 0 );
    rocketnet_server_transport_set_send_capacity_for_test( 0 );
    check( server != NULL );
    struct rocketnet_server_transport_t * transport = rocketnet_message_server_transport( server );
    if ( capacity > 0 )
        check( rocketnet_server_transport_send_capacity( transport ) == capacity );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( transport ) );
    const char * addresses[] = { server_address };

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.driver, sizeof( client_config.driver ), "%s", "loopback" );
    client_config.step_driven = 1;
    client_config.time_callback = fair_time_callback;

    for ( int k = 0; k < num_clients; k++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                         FAIR_CLIENT_ID_BASE + (uint64_t) k, 0, 1ULL << 40, 60, NULL,
                                                         token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
        struct fair_client_t * c = &run->clients[k];
        c->client = rocketnet_message_client_create( NULL, token, &client_config, NULL, NULL, 0, 0 );
        check( c->client != NULL );
        c->connected_tick = -1;
        c->disconnect_tick = -1;
        c->first_receive_tick = -1;
        c->server_disconnect_tick = -1;
        c->in_order = true;
        run->slot_client[k] = -1;
    }

    for ( int tick = 0; tick < FAIR_SEND_TICKS + FAIR_DRAIN_TICKS; tick++ )
    {
        time += FAIR_DT;
        fair_clock = time;
        const bool sending = tick < FAIR_SEND_TICKS;

        check( rocketnet_message_server_update( server, time ) == ROCKETNET_UPDATE_OK );
        for ( int k = 0; k < num_clients; k++ )
            check( rocketnet_message_client_update( run->clients[k].client, time ) == ROCKETNET_UPDATE_OK );

        // the server's view of each slot: receive, release, then send
        for ( int slot = 0; slot < num_clients; slot++ )
        {
            if ( !rocketnet_message_server_client_connected( server, slot ) )
            {
                const int was = run->slot_client[slot];
                if ( was >= 0 && run->clients[was].server_disconnect_tick < 0 )
                {
                    run->clients[was].server_disconnect_tick = tick;
                    run->clients[was].server_disconnect_reason = rocketnet_message_server_disconnect_reason( server, slot );
                }
                run->slot_client[slot] = -1;
                continue;
            }
            const int k = fair_client_for_slot( server, slot, num_clients );
            check( k >= 0 );
            run->slot_client[slot] = k;
            struct fair_client_t * c = &run->clients[k];
            const uint8_t * data;
            int bytes;
            while ( rocketnet_message_server_receive( server, slot, FAIR_CHANNEL, &data, &bytes ) )
            {
                if ( bytes == 8 && fair_read_kind( data ) == FAIR_KIND_UNRELIABLE )
                    c->server_got_unreliable++;
                else
                {
                    if ( bytes != 8 || fair_read_kind( data ) != FAIR_KIND_RELIABLE || fair_read_sequence( data ) != c->server_got_reliable )
                        c->in_order = false;
                    c->server_got_reliable++;
                }
                rocketnet_message_server_release( server, slot, data );
            }
            if ( sending && c->connected_tick >= 0 )
            {
                uint8_t message[8];
                fair_write( message, FAIR_KIND_RELIABLE, c->server_sent_reliable );
                if ( rocketnet_message_server_send( server, slot, FAIR_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, 8 ) != 0 )
                    c->server_sent_reliable++;
                fair_write( message, FAIR_KIND_UNRELIABLE, (uint32_t) tick );
                rocketnet_message_server_send( server, slot, FAIR_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE, message, 8 );
            }
        }

        // each client: connection bookkeeping, receive, release, then send
        for ( int k = 0; k < num_clients; k++ )
        {
            struct fair_client_t * c = &run->clients[k];
            const bool client_connected = rocketnet_message_client_state( c->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
            bool server_connected = false;
            for ( int slot = 0; slot < num_clients; slot++ )
                if ( rocketnet_message_server_client_connected( server, slot ) && fair_client_for_slot( server, slot, num_clients ) == k )
                    server_connected = true;
            if ( c->connected_tick < 0 && client_connected && server_connected )
                c->connected_tick = tick;
            if ( c->connected_tick >= 0 && c->disconnect_tick < 0 && !client_connected )
            {
                c->disconnect_tick = tick;
                c->client_disconnect_reason = rocketnet_message_client_disconnect_reason( c->client );
            }
            if ( !client_connected )
                continue;

            const uint8_t * data;
            int bytes;
            while ( rocketnet_message_client_receive( c->client, FAIR_CHANNEL, &data, &bytes ) )
            {
                if ( bytes == 8 && fair_read_kind( data ) == FAIR_KIND_UNRELIABLE )
                    c->client_got_unreliable++;
                else
                {
                    if ( bytes != 8 || fair_read_kind( data ) != FAIR_KIND_RELIABLE || fair_read_sequence( data ) != c->client_got_reliable )
                        c->in_order = false;
                    c->client_got_reliable++;
                }
                if ( c->first_receive_tick < 0 )
                    c->first_receive_tick = tick;
                rocketnet_message_client_release( c->client, data );
            }
            if ( sending && c->connected_tick >= 0 )
            {
                uint8_t message[8];
                fair_write( message, FAIR_KIND_RELIABLE, c->client_sent_reliable );
                if ( rocketnet_message_client_send( c->client, FAIR_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, 8 ) != 0 )
                    c->client_sent_reliable++;
                fair_write( message, FAIR_KIND_UNRELIABLE, (uint32_t) tick );
                rocketnet_message_client_send( c->client, FAIR_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE, message, 8 );
            }
        }

        rocketnet_message_server_send_packets( server );
        for ( int k = 0; k < num_clients; k++ )
            rocketnet_message_client_send_packets( run->clients[k].client );
    }

    run->server_send_ring_full = rocketnet_server_transport_counters( transport )->values[ROCKETNET_COUNTER_SEND_RING_FULL];

    // the record: what each client saw
    for ( int k = 0; k < num_clients; k++ )
    {
        const struct fair_client_t * c = &run->clients[k];
        printf( "    send capacity %d, client %d of %d: connected_tick=%d first_receive_tick=%d disconnect_tick=%d client_reason=%d server_disconnect_tick=%d server_reason=%d "
                "server->client reliable %u/%u unreliable %u, client->server reliable %u/%u unreliable %u, in_order=%d\n",
                capacity, k, num_clients, c->connected_tick, c->first_receive_tick, c->disconnect_tick,
                c->client_disconnect_reason, c->server_disconnect_tick, c->server_disconnect_reason,
                c->client_got_reliable, c->server_sent_reliable, c->client_got_unreliable,
                c->server_got_reliable, c->client_sent_reliable, c->server_got_unreliable, c->in_order ? 1 : 0 );
    }
    printf( "    send capacity %d: SEND_RING_FULL=%llu\n", capacity, (unsigned long long) run->server_send_ring_full );

    for ( int k = 0; k < num_clients; k++ )
        rocketnet_message_client_destroy( run->clients[k].client );
    rocketnet_server_transport_stop( transport );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( transport ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        time += FAIR_DT;
        rocketnet_message_server_update( server, time );
    }
    check( rocketnet_server_transport_state( transport ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_message_server_destroy( server );
}

// DONE-WHEN (3): every client connects, every client receives, no client is
// disconnected, and the reliable messages are all delivered in order
static void fair_check( const struct fair_run_t * run )
{
    for ( int k = 0; k < run->num_clients; k++ )
        check( run->clients[k].connected_tick >= 0 );                                   // every client connects
    for ( int k = 0; k < run->num_clients; k++ )
        check( run->clients[k].client_got_reliable > 0 && run->clients[k].client_got_unreliable > 0 );   // every client receives
    for ( int k = 0; k < run->num_clients; k++ )
        check( run->clients[k].disconnect_tick < 0 && run->clients[k].server_disconnect_tick < 0 );   // no client is disconnected
    for ( int k = 0; k < run->num_clients; k++ )
    {
        const struct fair_client_t * c = &run->clients[k];
        check( c->in_order );                                                           // reliable messages arrive in order
        check( c->server_sent_reliable > 0 && c->client_got_reliable == c->server_sent_reliable );   // every reliable message delivered, server to client
        check( c->client_sent_reliable > 0 && c->server_got_reliable == c->client_sent_reliable );   // every reliable message delivered, client to server
    }
}

static void server_send_cap_one_is_fair_two_clients()
{
    struct fair_run_t run;
    fair_run( 2, 1, &run );
    fair_check( &run );
    printf( "server_send_cap_one_is_fair_two_clients\n" );
}

REGISTER_UNIT_TEST( server_send_cap_one_is_fair_two_clients );

static void server_send_cap_one_is_fair_three_clients()
{
    struct fair_run_t run;
    fair_run( 3, 1, &run );
    fair_check( &run );
    printf( "server_send_cap_one_is_fair_three_clients\n" );
}

REGISTER_UNIT_TEST( server_send_cap_one_is_fair_three_clients );

static void server_send_capacity_default_covers_max_clients()
{
    // the default, with no override, at the largest max_clients a config may name
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = ROCKETNET_MAX_CLIENTS;
    config.protocol_id = 0x666169726361700bULL;
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( config.backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( config.server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );
    memcpy( config.server_secret_key, server_secret_key, sizeof( config.server_secret_key ) );
    snprintf( config.driver, sizeof( config.driver ), "%s", "loopback" );
    rocketnet_server_transport_set_send_capacity_for_test( 0 );
    struct rocketnet_server_transport_t * transport = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, NULL );
    check( transport != NULL );
    check( rocketnet_server_transport_send_capacity( transport ) >= ROCKETNET_MAX_CLIENTS );
    rocketnet_server_transport_stop( transport );
    double time = 1000.0;
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( transport ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        time += FAIR_DT;
        rocketnet_server_transport_update( transport, time );
    }
    check( rocketnet_server_transport_state( transport ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_server_transport_destroy( transport );

    // and the fair case at the default capacity
    struct fair_run_t run;
    fair_run( 3, 0, &run );
    fair_check( &run );
    check( run.server_send_ring_full == 0 );
    printf( "server_send_capacity_default_covers_max_clients\n" );
}

REGISTER_UNIT_TEST( server_send_capacity_default_covers_max_clients );
