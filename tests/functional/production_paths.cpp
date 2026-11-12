/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public-API paths the production library (ROCKETNET_DEV=0) runs that no
// other production-linked case reached: the client's two-family race on each
// client transport this host offers, a server ending every slot at once, and
// messages started then aborted on both ends. Real sockets on 127.0.0.1 and
// [::1], the client's I/O thread running. The keys, the packet filter and the
// connect tokens are the test's own production values (tests/token_string.h),
// so the same cases run in functional and in functional_production.

#include "../../examples/shutdown.h"
#include "../token_string.h"
#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_driver.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

// defined in tests/functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );
// defined in tests/functional/rocketnet_functional_tests.cpp
void rocketnet_functional_log_ring_dump();

static void paths_check_handler( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_functional_log_ring_dump();
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define paths_check( condition )                                                                  \
    do                                                                                            \
    {                                                                                             \
        if ( !( condition ) )                                                                     \
        {                                                                                         \
            paths_check_handler( #condition, (const char *) __FUNCTION__, __FILE__, __LINE__ );   \
        }                                                                                         \
    } while ( 0 )

// An outer suite pin must not replace the transports these cases name.

class paths_env_guard_t
{
public:
    explicit paths_env_guard_t( const char * variable )
        : name( variable ), present( false )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        char * current = NULL;
        size_t current_bytes = 0;
        paths_check( _dupenv_s( &current, &current_bytes, name.c_str() ) == 0 );
        if ( current )
        {
            present = true;
            value = current;
            free( current );
        }
        paths_check( _putenv_s( name.c_str(), "" ) == 0 );
#else
        const char * current = getenv( name.c_str() );
        if ( current )
        {
            present = true;
            value = current;
        }
        paths_check( unsetenv( name.c_str() ) == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

    ~paths_env_guard_t()
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        (void) _putenv_s( name.c_str(), present ? value.c_str() : "" );
#else
        if ( present )
        {
            (void) setenv( name.c_str(), value.c_str(), 1 );
        }
        else
        {
            (void) unsetenv( name.c_str() );
        }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

private:
    std::string name;
    bool present;
    std::string value;
};

static const uint64_t paths_protocol_id = 0x5041544853303031ULL;

// any filter but the published test filter, which a production library refuses
static const uint8_t paths_packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x50, 0x41, 0x54, 0x48 };

// every wait in these cases is bounded by this, well inside the suite's limit
static const double paths_wait_seconds = 5.0;

struct paths_world_t
{
    struct rocketnet_server_t * server;
    struct rocketnet_client_t * client[2];
    double client_time[2];
    uint8_t backend_secret_key[64];
    double start;
    bool two_families;
};

// The game clock every end is updated with: 1000 plus the wall seconds since
// the world was made.
static double paths_now( const paths_world_t * world )
{
    return 1000.0 + ( rocketnet_time() - world->start );
}

// A started portable server on 127.0.0.1, and on [::1] at the same port when
// two_families is set.
static void paths_server_start( paths_world_t * world, bool two_families )
{
    memset( world, 0, sizeof( *world ) );
    world->start = rocketnet_time();
    world->two_families = two_families;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x6B ^ i );
    }
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    paths_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public_key, world->backend_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    paths_check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = paths_protocol_id;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    memcpy( server_config.packet_filter, paths_packet_filter, sizeof( server_config.packet_filter ) );
    server_config.max_clients = 2;
    server_config.transport = "portable";

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", two_families ? "[::1]:0" : NULL, &server_config, &status );
    paths_check( world->server != NULL && status == ROCKETNET_STATUS_OK );
    paths_check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );
}

// One frame: the server and every client are updated and drained, then send.
static void paths_step( paths_world_t * world )
{
    const double now = paths_now( world );
    rocketnet_server_update( world->server, now );
    for ( int i = 0; i < 2; i++ )
    {
        if ( world->client[i] )
        {
            rocketnet_client_update( world->client[i], now );
            world->client_time[i] = now;
        }
    }
    for ( int i = 0; i < 2; i++ )
    {
        if ( world->client[i] )
        {
            rocketnet_client_send_packets( world->client[i] );
        }
    }
    rocketnet_server_send_packets( world->server );
    rocketnet_sleep( 0.001 );
}

// Creates client i on the named client transport and connects it with a token
// naming 127.0.0.1, and [::1] too for a two-family server; returns once both
// ends report the connection.
static void paths_client_connect( paths_world_t * world, int i, const char * transport, uint64_t client_id )
{
    char ipv4_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char ipv6_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    snprintf( ipv4_address, sizeof( ipv4_address ), "127.0.0.1:%d", (int) rocketnet_server_port( world->server ) );
    snprintf( ipv6_address, sizeof( ipv6_address ), "[::1]:%d", (int) rocketnet_server_port( world->server ) );
    const char * addresses[] = { ipv4_address, ipv6_address };

    const uint64_t create_timestamp = rocketnet_functional_token_timestamp( 0, paths_now( world ) );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    paths_check( rn_test_server_token_string( world->backend_secret_key, world->server, paths_protocol_id, addresses,
                                              world->two_families ? 2 : 1, client_id, create_timestamp, create_timestamp + 30, 10,
                                              NULL, token ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    paths_check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = transport;
    memcpy( client_config.packet_filter, paths_packet_filter, sizeof( client_config.packet_filter ) );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    world->client[i] = rocketnet_client_create( NULL, &client_config, &status );
    paths_check( world->client[i] != NULL && status == ROCKETNET_STATUS_OK );
    world->client_time[i] = paths_now( world );
    rocketnet_client_update( world->client[i], world->client_time[i] );
    paths_check( rocketnet_client_connect( world->client[i], token ) == ROCKETNET_STATUS_OK );

    const double deadline = rocketnet_time() + paths_wait_seconds;
    while ( rocketnet_time() < deadline &&
            ( rocketnet_client_state( world->client[i] ) != ROCKETNET_CLIENT_STATE_CONNECTED ||
              !rocketnet_server_client_connected( world->server, rocketnet_client_index( world->client[i] ) ) ) )
    {
        paths_step( world );
    }

    paths_check( rocketnet_client_state( world->client[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    paths_check( rocketnet_server_client_connected( world->server, rocketnet_client_index( world->client[i] ) ) );
}

static void paths_client_destroy( paths_world_t * world, int i )
{
    rn_cli_client_stop_and_destroy( world->client[i], world->client_time[i] );
    world->client[i] = NULL;
}

static void paths_world_destroy( paths_world_t * world )
{
    for ( int i = 0; i < 2; i++ )
    {
        paths_client_destroy( world, i );
    }
    rn_cli_server_stop_and_destroy( world->server );
    world->server = NULL;
}

// The byte message k of a stream carries: its index, then a fill from it.
static void paths_fill( uint8_t * data, int bytes, int k, uint8_t salt )
{
    for ( int j = 0; j < bytes; j++ )
    {
        data[j] = (uint8_t) ( salt + k * 7 + j );
    }
}

static bool paths_matches( const uint8_t * data, int bytes, int k, uint8_t salt )
{
    for ( int j = 0; j < bytes; j++ )
    {
        if ( data[j] != (uint8_t) ( salt + k * 7 + j ) )
        {
            return false;
        }
    }
    return true;
}

// Sends `count` reliable ordered messages of `bytes` each from the server to
// client i and from client i to the server, steps until each side has them
// all, and checks every one arrived once, in order, byte exact.
static void paths_exchange( paths_world_t * world, int i, int count, int bytes )
{
    const int slot = rocketnet_client_index( world->client[i] );
    for ( int k = 0; k < count; k++ )
    {
        uint8_t * out = rocketnet_client_start_message( world->client[i], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        paths_check( out != NULL );
        paths_fill( out, bytes, k, 0x11 );
        paths_check( rocketnet_client_finish_message( world->client[i], bytes ) != 0 );

        out = rocketnet_server_start_message( world->server, (uint64_t) slot, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        paths_check( out != NULL );
        paths_fill( out, bytes, k, 0x22 );
        paths_check( rocketnet_server_finish_message( world->server, bytes ) != 0 );
    }

    int at_server = 0;
    int at_client = 0;
    const double deadline = rocketnet_time() + paths_wait_seconds;
    while ( rocketnet_time() < deadline && ( at_server < count || at_client < count ) )
    {
        const double now = paths_now( world );
        rocketnet_server_update( world->server, now );
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                paths_check( batch->client_index[m] == slot );
                paths_check( batch->channel[m] == 1 );
                paths_check( batch->reliability[m] == ROCKETNET_RELIABILITY_RELIABLE_ORDERED );
                paths_check( batch->bytes[m] == bytes );
                paths_check( paths_matches( batch->data[m], bytes, at_server, 0x11 ) );
                at_server++;
            }
        }
        rocketnet_client_update( world->client[i], now );
        world->client_time[i] = now;
        while ( ( batch = rocketnet_client_receive_messages( world->client[i] ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                paths_check( batch->channel[m] == 1 );
                paths_check( batch->bytes[m] == bytes );
                paths_check( paths_matches( batch->data[m], bytes, at_client, 0x22 ) );
                at_client++;
            }
        }
        rocketnet_client_send_packets( world->client[i] );
        rocketnet_server_send_packets( world->server );
        rocketnet_sleep( 0.001 );
    }

    paths_check( at_server == count );
    paths_check( at_client == count );
}

static std::atomic<uint64_t> paths_races_logged{ 0 };

static void paths_race_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    static const char needle[] = "client races both families";
    const size_t needle_bytes = sizeof( needle ) - 1;
    for ( size_t i = 0; i + needle_bytes <= length; i++ )
    {
        if ( memcmp( message + i, needle, needle_bytes ) == 0 )
        {
            paths_races_logged.fetch_add( 1 );
            return;
        }
    }
}

// A token naming both families, on every client transport this host offers
// (loopback has no family): the client dials both, the first answer keeps one
// family and the other is closed, and the connection then carries reliable
// messages both ways, byte exact and in order, through the kept family alone.
// The client's disconnect ends the server's slot as CLIENT_DISCONNECTED.
void rocketnet_test_family_race_each_client_transport_carries_messages()
{
    paths_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    paths_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    paths_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    paths_world_t world;
    paths_server_start( &world, true );

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( paths_race_log, NULL );

    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );
    int raced = 0;
    for ( int t = 0; t < num_available; t++ )
    {
        const char * transport = rocketnet_transport_name( available[t] );
        if ( strcmp( transport, "loopback" ) == 0 )
        {
            continue;
        }

        const uint64_t races_before = paths_races_logged.load();
        paths_client_connect( &world, 0, transport, UINT64_C( 0x7A00 ) + (uint64_t) t );
        paths_check( paths_races_logged.load() == races_before + 1 );
        paths_check( strcmp( rocketnet_client_transport_name( world.client[0] ), transport ) == 0 );

        paths_exchange( &world, 0, 8, 300 );

        const int slot = rocketnet_client_index( world.client[0] );
        rocketnet_client_disconnect( world.client[0] );
        const double deadline = rocketnet_time() + paths_wait_seconds;
        while ( rocketnet_time() < deadline && rocketnet_server_client_connected( world.server, slot ) )
        {
            paths_step( &world );
        }
        paths_check( !rocketnet_server_client_connected( world.server, slot ) );
        paths_check( rocketnet_server_disconnect_reason( world.server, slot ) == ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED );
        paths_client_destroy( &world, 0 );

        printf( "family race over %s: connected, 8 messages each way\n", transport );
        raced++;
    }
    paths_check( raced > 0 );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    paths_world_destroy( &world );
}

// rocketnet_server_disconnect_all_clients with two connected clients: both
// slots end at once as SERVER_DISCONNECTED, the server counts none connected,
// and each client, told by the server's disconnect packets, moves to
// SERVER_DISCONNECTED.
void rocketnet_test_server_disconnect_all_clients_ends_every_slot()
{
    paths_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    paths_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    paths_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    paths_world_t world;
    paths_server_start( &world, false );
    paths_client_connect( &world, 0, "portable", 0x7B01ULL );
    paths_client_connect( &world, 1, "portable", 0x7B02ULL );
    paths_check( rocketnet_server_num_connected_clients( world.server ) == 2 );

    const int slot0 = rocketnet_client_index( world.client[0] );
    const int slot1 = rocketnet_client_index( world.client[1] );
    paths_check( slot0 != slot1 );

    rocketnet_server_disconnect_all_clients( world.server );
    paths_check( !rocketnet_server_client_connected( world.server, slot0 ) );
    paths_check( !rocketnet_server_client_connected( world.server, slot1 ) );
    paths_check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    paths_check( rocketnet_server_disconnect_reason( world.server, slot0 ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
    paths_check( rocketnet_server_disconnect_reason( world.server, slot1 ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

    const double deadline = rocketnet_time() + paths_wait_seconds;
    while ( rocketnet_time() < deadline &&
            ( rocketnet_client_state( world.client[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED ||
              rocketnet_client_state( world.client[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED ) )
    {
        paths_step( &world );
    }
    paths_check( rocketnet_client_state( world.client[0] ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    paths_check( rocketnet_client_state( world.client[1] ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    paths_check( strcmp( rocketnet_client_state_name( rocketnet_client_state( world.client[0] ) ), "SERVER_DISCONNECTED" ) == 0 );

    // a second call with no client connected changes nothing

    rocketnet_server_disconnect_all_clients( world.server );
    paths_check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    paths_check( rocketnet_server_disconnect_reason( world.server, slot0 ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

    paths_world_destroy( &world );
}

// A message started then aborted sends nothing: on the client and on the
// server, an aborted message between two sent ones leaves exactly the two
// sent ones to arrive, in order.
void rocketnet_test_aborted_messages_send_nothing()
{
    paths_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    paths_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    paths_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    paths_world_t world;
    paths_server_start( &world, false );
    paths_client_connect( &world, 0, "portable", 0x7C01ULL );
    const int slot = rocketnet_client_index( world.client[0] );

    for ( int pass = 0; pass < 3; pass++ )
    {
        const bool abort_this = pass == 1;
        uint8_t * out = rocketnet_client_start_message( world.client[0], 2, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 40 );
        paths_check( out != NULL );
        memset( out, 0xA0 + pass, 40 );
        if ( abort_this )
        {
            rocketnet_client_abort_message( world.client[0] );
        }
        else
        {
            paths_check( rocketnet_client_finish_message( world.client[0], 40 ) != 0 );
        }

        out = rocketnet_server_start_message( world.server, (uint64_t) slot, 2, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 40 );
        paths_check( out != NULL );
        memset( out, 0xB0 + pass, 40 );
        if ( abort_this )
        {
            rocketnet_server_abort_message( world.server );
        }
        else
        {
            paths_check( rocketnet_server_finish_message( world.server, 40 ) != 0 );
        }
    }

    uint8_t at_server[4] = { 0 };
    uint8_t at_client[4] = { 0 };
    int num_at_server = 0;
    int num_at_client = 0;
    const double deadline = rocketnet_time() + paths_wait_seconds;
    while ( rocketnet_time() < deadline && ( num_at_server < 2 || num_at_client < 2 ) )
    {
        const double now = paths_now( &world );
        rocketnet_server_update( world.server, now );
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( world.server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                paths_check( num_at_server < 4 && batch->bytes[m] == 40 );
                at_server[num_at_server++] = batch->data[m][0];
            }
        }
        rocketnet_client_update( world.client[0], now );
        world.client_time[0] = now;
        while ( ( batch = rocketnet_client_receive_messages( world.client[0] ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                paths_check( num_at_client < 4 && batch->bytes[m] == 40 );
                at_client[num_at_client++] = batch->data[m][0];
            }
        }
        rocketnet_client_send_packets( world.client[0] );
        rocketnet_server_send_packets( world.server );
        rocketnet_sleep( 0.001 );
    }

    // a few frames more: nothing further arrives

    for ( int frame = 0; frame < 20; frame++ )
    {
        const double now = paths_now( &world );
        rocketnet_server_update( world.server, now );
        paths_check( rocketnet_server_receive_messages( world.server ) == NULL );
        rocketnet_client_update( world.client[0], now );
        world.client_time[0] = now;
        paths_check( rocketnet_client_receive_messages( world.client[0] ) == NULL );
        rocketnet_client_send_packets( world.client[0] );
        rocketnet_server_send_packets( world.server );
        rocketnet_sleep( 0.001 );
    }

    paths_check( num_at_server == 2 && at_server[0] == 0xA0 && at_server[1] == 0xA2 );
    paths_check( num_at_client == 2 && at_client[0] == 0xB0 && at_client[1] == 0xB2 );

    paths_world_destroy( &world );
}
