/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Disconnect reasons and network conditions over a real transport: the
// public client and server on the portable transport over 127.0.0.1, the
// client's network thread running (not step-driven), each reason reached the
// way a game reaches it:
//   - SERVER_DISCONNECTED: the server disconnects a connected client
//   - TIMED_OUT: the client goes quiet under a one-second token timeout
//   - RELIABLE_TIMED_OUT: the server opts in to a reliable message timeout
//     and sends to a client that has gone quiet
//   - MESSAGE_NOT_RECEIVED: a client message is left in the server's receive
//     queue at the next server update (rocketnet_server_update)
// and rocketnet_client_conditions / rocketnet_server_client_conditions read
// while connected, while the client is quiet, and once not connected.
// PROTOCOL_VIOLATION and MESSAGE_MEMORY_EXHAUSTED have no path a game can
// take, by design, and are covered in the unit suite.
//
// The keys, the packet filter and the connect tokens are the test's own
// production values (tests/token_string.h), never the published development
// ones, so the same cases run against the development library (functional) and
// the production library a customer ships (functional_production).

#include "../../examples/shutdown.h"
#include "../token_string.h"
#include "rocketnet.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "../check_count.h"

// defined in tests/functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );
// defined in tests/functional/rocketnet_functional_tests.cpp
void rocketnet_functional_log_ring_dump();
void rocketnet_functional_restore_assert_function();

static void reasons_check_handler( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_functional_log_ring_dump();
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define reasons_check( condition )                                                                   \
    do                                                                                               \
    {                                                                                                \
        ROCKETNET_TEST_COUNT_CHECK();                                                                \
        if ( !( condition ) )                                                                        \
        {                                                                                            \
            reasons_check_handler( #condition, (const char *) __FUNCTION__, __FILE__, __LINE__ );    \
        }                                                                                            \
    } while ( 0 )

// An outer suite pin must not replace the portable transport these cases name.

class reasons_env_guard_t
{
public:
    explicit reasons_env_guard_t( const char * variable )
        : name( variable ), present( false )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        char * current = NULL;
        size_t current_bytes = 0;
        reasons_check( _dupenv_s( &current, &current_bytes, name.c_str() ) == 0 );
        if ( current )
        {
            present = true;
            value = current;
            free( current );
        }
        reasons_check( _putenv_s( name.c_str(), "" ) == 0 );
#else
        const char * current = getenv( name.c_str() );
        if ( current )
        {
            present = true;
            value = current;
        }
        reasons_check( unsetenv( name.c_str() ) == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

    ~reasons_env_guard_t()
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

// any nonzero id the server and its tokens share
static const uint64_t reasons_protocol_id = 0x52454153304E5331ULL;

// any filter but the published test filter, which a production library refuses
static const uint8_t reasons_packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x52, 0x45, 0x41, 0x53 };

// every wait in these cases is bounded by this, well inside the suite's limit
static const double reasons_wait_seconds = 4.0;

struct reasons_pair_t
{
    struct rocketnet_server_t * server;
    struct rocketnet_client_t * client;
    double start;
    double client_time;
    int slot;
};

// The game clock both ends are updated with: 1000 plus the wall seconds since
// the pair was made.
static double reasons_now( const reasons_pair_t * pair )
{
    return 1000.0 + ( rocketnet_time() - pair->start );
}

// One frame. The server is updated and, unless the case leaves a message in
// its queue, drained; the client is updated, drained and sends only while it
// runs, so a client that does not run sends nothing at all.
static void reasons_step( reasons_pair_t * pair, bool client_runs, bool server_drains )
{
    const double now = reasons_now( pair );
    rocketnet_server_update( pair->server, now );
    if ( server_drains )
    {
        while ( rocketnet_server_receive_messages( pair->server ) != NULL )
        {
        }
    }

    if ( client_runs )
    {
        rocketnet_client_update( pair->client, now );
        pair->client_time = now;
        while ( rocketnet_client_receive_messages( pair->client ) != NULL )
        {
        }
        rocketnet_client_send_packets( pair->client );
    }

    rocketnet_server_send_packets( pair->server );
    rocketnet_sleep( 0.001 );
}

// A started portable server and a connected threaded portable client.
// reliable_message_timeout_ms is the server's opt-in (0 leaves it off);
// timeout_seconds is the connect token's silence timeout.
static void reasons_pair_connect( reasons_pair_t * pair, int reliable_message_timeout_ms, int timeout_seconds )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->start = rocketnet_time();

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    reasons_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    reasons_check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = reasons_protocol_id;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    memcpy( server_config.packet_filter, reasons_packet_filter, sizeof( server_config.packet_filter ) );
    server_config.max_clients = 2;
    server_config.transport = "portable";
    if ( reliable_message_timeout_ms > 0 )
    {
        // the timeout must be at least four times the initial resend delay
        server_config.message_resend_delay_initial_ms = reliable_message_timeout_ms / 4;
        server_config.reliable_message_timeout_ms = reliable_message_timeout_ms;
    }

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    pair->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    reasons_check( pair->server != NULL && status == ROCKETNET_STATUS_OK );
    reasons_check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    reasons_check( rocketnet_server_ipv4_address( pair->server, server_address, sizeof( server_address ) ) );
    const char * server_addresses[] = { server_address };
    const uint64_t create_timestamp = rocketnet_functional_token_timestamp( 0, reasons_now( pair ) );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    reasons_check( rn_test_server_token_string( backend_secret_key, pair->server, reasons_protocol_id, server_addresses, 1, 1,
                                                create_timestamp, create_timestamp + 30, timeout_seconds, NULL, token ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    reasons_check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "portable";
    memcpy( client_config.packet_filter, reasons_packet_filter, sizeof( client_config.packet_filter ) );
    reasons_check( !client_config.step_driven );

    pair->client = rocketnet_client_create( NULL, &client_config, &status );
    reasons_check( pair->client != NULL && status == ROCKETNET_STATUS_OK );
    pair->client_time = reasons_now( pair );
    rocketnet_client_update( pair->client, pair->client_time );
    reasons_check( rocketnet_client_connect( pair->client, token ) == ROCKETNET_STATUS_OK );

    const double deadline = rocketnet_time() + reasons_wait_seconds;
    while ( rocketnet_time() < deadline &&
            ( rocketnet_client_state( pair->client ) != ROCKETNET_CLIENT_STATE_CONNECTED ||
              !rocketnet_server_client_connected( pair->server, rocketnet_client_index( pair->client ) ) ) )
    {
        reasons_step( pair, true, true );
    }

    reasons_check( rocketnet_client_state( pair->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    pair->slot = rocketnet_client_index( pair->client );
    reasons_check( rocketnet_server_client_connected( pair->server, pair->slot ) );
    reasons_check( rocketnet_server_disconnect_reason( pair->server, pair->slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
}

// Steps until the server's slot is no longer connected, or the bounded wait
// ends; returns the seconds it took.
static double reasons_wait_slot_ends( reasons_pair_t * pair, bool client_runs, bool server_drains )
{
    const double begin = rocketnet_time();
    while ( rocketnet_time() - begin < reasons_wait_seconds && rocketnet_server_client_connected( pair->server, pair->slot ) )
    {
        reasons_step( pair, client_runs, server_drains );
    }

    return rocketnet_time() - begin;
}

static void reasons_pair_destroy( reasons_pair_t * pair )
{
    rn_cli_client_stop_and_destroy( pair->client, pair->client_time );
    rn_cli_server_stop_and_destroy( pair->server );
}

static void reasons_send_reliable( reasons_pair_t * pair, bool from_server )
{
    uint8_t * out = from_server ? rocketnet_server_start_message( pair->server, (uint64_t) pair->slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 )
                                : rocketnet_client_start_message( pair->client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 );
    reasons_check( out != NULL );
    memset( out, 0x5A, 16 );
    reasons_check( ( from_server ? rocketnet_server_finish_message( pair->server, 16 ) : rocketnet_client_finish_message( pair->client, 16 ) ) != 0 );
}

static bool reasons_conditions_zero( const struct rocketnet_conditions_t * conditions )
{
    return conditions->seconds_since_heard == 0.0 && conditions->seconds_since_progress == 0.0 &&
           conditions->reliable_in_flight == 0 && conditions->send_refused == 0;
}

// The server disconnects a connected client: the slot reads
// SERVER_DISCONNECTED at once and the client, told by the server's
// disconnect packets, moves to SERVER_DISCONNECTED.
void rocketnet_test_disconnect_reason_server_disconnected_portable()
{
    reasons_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    reasons_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    reasons_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    reasons_pair_t pair;
    reasons_pair_connect( &pair, 0, 10 );

    rocketnet_server_disconnect_client( pair.server, pair.slot );
    reasons_check( !rocketnet_server_client_connected( pair.server, pair.slot ) );
    reasons_check( rocketnet_server_disconnect_reason( pair.server, pair.slot ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

    const double begin = rocketnet_time();
    while ( rocketnet_time() - begin < reasons_wait_seconds && rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        reasons_step( &pair, true, true );
    }

    reasons_check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    reasons_check( rocketnet_server_disconnect_reason( pair.server, pair.slot ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

    reasons_pair_destroy( &pair );
}

// A client that stops sending under a one-second token timeout: the server
// keeps updating and ends the slot with TIMED_OUT.
void rocketnet_test_disconnect_reason_timed_out_portable()
{
    reasons_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    reasons_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    reasons_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    reasons_pair_t pair;
    reasons_pair_connect( &pair, 0, 1 );

    const double quiet = reasons_wait_slot_ends( &pair, false, true );
    reasons_check( !rocketnet_server_client_connected( pair.server, pair.slot ) );
    reasons_check( rocketnet_server_disconnect_reason( pair.server, pair.slot ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );
    reasons_check( quiet >= 0.5 );

    reasons_pair_destroy( &pair );
}

// The server opts in to a one-second reliable message timeout and sends a
// reliable message to a client that has gone quiet: the slot ends with
// RELIABLE_TIMED_OUT, before the token's five-second silence timeout.
void rocketnet_test_disconnect_reason_reliable_timed_out_portable()
{
    reasons_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    reasons_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    reasons_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    reasons_pair_t pair;
    reasons_pair_connect( &pair, 1000, 5 );

    reasons_send_reliable( &pair, true );
    const double waited = reasons_wait_slot_ends( &pair, false, true );
    reasons_check( !rocketnet_server_client_connected( pair.server, pair.slot ) );
    reasons_check( rocketnet_server_disconnect_reason( pair.server, pair.slot ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    reasons_check( waited >= 0.5 && waited < 5.0 );

    reasons_pair_destroy( &pair );
}

static int reasons_asserts;

static void reasons_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    reasons_asserts++;
}

// A client message the game leaves in the server's receive queue at the next
// server update is API misuse: the sending client's slot ends with
// MESSAGE_NOT_RECEIVED (rocketnet_server_update), with one assert in a build
// with asserts.
void rocketnet_test_disconnect_reason_message_not_received_portable()
{
    reasons_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    reasons_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    reasons_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    reasons_pair_t pair;
    reasons_pair_connect( &pair, 0, 10 );

    reasons_send_reliable( &pair, false );
    rocketnet_client_send_packets( pair.client );

    rocketnet_assert_function( reasons_assert_hook );
    reasons_asserts = 0;
    reasons_wait_slot_ends( &pair, true, false );
    rocketnet_functional_restore_assert_function();

    reasons_check( !rocketnet_server_client_connected( pair.server, pair.slot ) );
    reasons_check( rocketnet_server_disconnect_reason( pair.server, pair.slot ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED );
    reasons_check( reasons_asserts == ROCKETNET_ASSERTS );

    reasons_pair_destroy( &pair );
}

// Both conditions getters over a real transport with the client's network
// thread running: true and sane while traffic flows; a reliable message is
// in flight until acknowledged; the server's seconds_since_heard grows over
// a measured quiet interval of the client; both return false and zero the
// struct once not connected.
void rocketnet_test_conditions_portable()
{
    reasons_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    reasons_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    reasons_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    reasons_pair_t pair;
    reasons_pair_connect( &pair, 0, 10 );

    for ( int i = 0; i < 50; i++ )
    {
        reasons_step( &pair, true, true );
    }

    // traffic flows both ways and nothing reliable is waiting. Over a real
    // transport the connect's reliable traffic can still be in flight after a
    // fixed number of steps on a loaded host (macOS Release, studio-4, CI run
    // 43306427273: seconds_since_progress was not -1 after 50 steps), so the
    // pair steps on until both ends report nothing in flight, up to the case's
    // deadline, and the checks below then judge what the ends report

    struct rocketnet_conditions_t server_conditions;
    struct rocketnet_conditions_t client_conditions;
    const double settle = rocketnet_time();
    for ( ;; )
    {
        reasons_check( rocketnet_server_client_conditions( pair.server, pair.slot, &server_conditions, (int) sizeof( server_conditions ) ) );
        reasons_check( rocketnet_client_conditions( pair.client, &client_conditions, (int) sizeof( client_conditions ) ) );
        if ( ( server_conditions.seconds_since_progress == -1.0 && client_conditions.seconds_since_progress == -1.0 ) ||
             rocketnet_time() - settle >= reasons_wait_seconds )
        {
            break;
        }
        reasons_step( &pair, true, true );
    }
    reasons_check( server_conditions.seconds_since_heard >= 0.0 && server_conditions.seconds_since_heard < 0.25 );
    reasons_check( client_conditions.seconds_since_heard >= 0.0 && client_conditions.seconds_since_heard < 0.25 );
    reasons_check( server_conditions.seconds_since_progress == -1.0 && client_conditions.seconds_since_progress == -1.0 );
    reasons_check( server_conditions.reliable_in_flight == 0 && client_conditions.reliable_in_flight == 0 );
    reasons_check( server_conditions.send_refused == 0 && client_conditions.send_refused == 0 );

    // a reliable message the client has sent and the server has not yet
    // acknowledged (the server is not updated in between) is in flight

    reasons_send_reliable( &pair, false );
    rocketnet_client_send_packets( pair.client );
    reasons_check( rocketnet_client_conditions( pair.client, &client_conditions, (int) sizeof( client_conditions ) ) );
    reasons_check( client_conditions.reliable_in_flight == 1 );
    reasons_check( client_conditions.seconds_since_progress >= 0.0 );

    double begin = rocketnet_time();
    while ( rocketnet_time() - begin < reasons_wait_seconds && client_conditions.reliable_in_flight != 0 )
    {
        reasons_step( &pair, true, true );
        reasons_check( rocketnet_client_conditions( pair.client, &client_conditions, (int) sizeof( client_conditions ) ) );
    }

    reasons_check( client_conditions.reliable_in_flight == 0 );
    reasons_check( client_conditions.seconds_since_progress == -1.0 );

    // the client goes quiet for a measured interval: the server's reliable
    // message to it stays in flight and seconds_since_heard grows by about
    // that interval

    const double quiet_seconds = 0.5;
    const double quiet_begin = rocketnet_time();
    reasons_send_reliable( &pair, true );
    while ( rocketnet_time() - quiet_begin < quiet_seconds )
    {
        reasons_step( &pair, false, true );
    }

    reasons_check( rocketnet_server_client_conditions( pair.server, pair.slot, &server_conditions, (int) sizeof( server_conditions ) ) );
    const double quiet_measured = rocketnet_time() - quiet_begin;
    reasons_check( server_conditions.seconds_since_heard >= quiet_seconds - 0.1 );
    reasons_check( server_conditions.seconds_since_heard <= quiet_measured + 0.1 );
    reasons_check( server_conditions.reliable_in_flight == 1 );
    reasons_check( server_conditions.seconds_since_progress >= quiet_seconds - 0.1 );

    // the client runs again: it is heard and acknowledges the message

    begin = rocketnet_time();
    while ( rocketnet_time() - begin < reasons_wait_seconds && server_conditions.reliable_in_flight != 0 )
    {
        reasons_step( &pair, true, true );
        reasons_check( rocketnet_server_client_conditions( pair.server, pair.slot, &server_conditions, (int) sizeof( server_conditions ) ) );
    }

    reasons_check( server_conditions.reliable_in_flight == 0 );
    reasons_check( server_conditions.seconds_since_heard >= 0.0 && server_conditions.seconds_since_heard < 0.25 );

    // not connected: both getters return false and zero the struct

    rocketnet_client_disconnect( pair.client );
    memset( &client_conditions, 0xFF, sizeof( client_conditions ) );
    reasons_check( !rocketnet_client_conditions( pair.client, &client_conditions, (int) sizeof( client_conditions ) ) );
    reasons_check( reasons_conditions_zero( &client_conditions ) );

    begin = rocketnet_time();
    while ( rocketnet_time() - begin < reasons_wait_seconds && rocketnet_server_client_connected( pair.server, pair.slot ) )
    {
        reasons_step( &pair, true, true );
    }

    reasons_check( rocketnet_server_disconnect_reason( pair.server, pair.slot ) == ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED );
    memset( &server_conditions, 0xFF, sizeof( server_conditions ) );
    reasons_check( !rocketnet_server_client_conditions( pair.server, pair.slot, &server_conditions, (int) sizeof( server_conditions ) ) );
    reasons_check( reasons_conditions_zero( &server_conditions ) );

    reasons_pair_destroy( &pair );
}
