/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public contract audit of 2026-10-29: each case holds one promise of
// sdk/rocketnet.h that the code once broke. The state change callback reports
// exactly the transitions of rocketnet_client_state; a message left in the
// receive queue at update is API misuse on both ends; a start refused for its
// arguments logs; a refused update time is counted and logged with no live
// connection too; receipts stay readable after disconnect until the next
// update; the integrity-limit end reads a negative state; init with another
// allocator and a dev token with no address are API misuse; and the default
// num_channels fits the build. Everything runs in one process over the in-memory loopback transport
// with step-driven clients: no socket, no thread, no sleep, and time is an
// argument starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "loopback_hub.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message_object.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check
#define CONTRACT_AUDIT_PROTOCOL 0x434F4E5452414354ULL

// --- log and assert capture -------------------------------------------------

#if ROCKETNET_DEV

static int contract_errors = 0;
static char contract_last_error[1024];
static int contract_asserts = 0;

static void contract_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;

    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
        return;

    contract_errors++;

    const size_t bytes = length < sizeof( contract_last_error ) - 1 ? length : sizeof( contract_last_error ) - 1;
    memcpy( contract_last_error, message, bytes );
    contract_last_error[bytes] = '\0';
}

static void contract_assert( const char *, const char *, const char *, int )
{
    contract_asserts++;
}

struct contract_capture_t
{
    rocketnet_assert_function_t saved_assert;
    enum rocketnet_log_level_t saved_level;
};

static void contract_capture_begin( contract_capture_t * capture )
{
    capture->saved_assert = rn_assert_function;
    capture->saved_level = rocketnet_log_level_current();
    contract_errors = 0;
    contract_asserts = 0;
    contract_last_error[0] = '\0';
    rocketnet_quiet( 0 );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_log_function( contract_log, NULL );
    rocketnet_assert_function( contract_assert );
}

static void contract_capture_end( const contract_capture_t * capture )
{
    rocketnet_assert_function( capture->saved_assert );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( capture->saved_level );
    rocketnet_quiet( 1 );
}

// U2: both dev token addresses NULL is a required argument missing (the dev
// token exists only in a development build)

static void test_contract_dev_token_without_address_is_misuse()
{
    contract_capture_t capture;
    contract_capture_begin( &capture );

    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES] = {};
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    const uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_create_dev_connect_token( NULL, NULL, server_public_key, 30, 10, 1, CONTRACT_AUDIT_PROTOCOL, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == before + 1 );
    check( contract_errors == 1 );

    contract_capture_end( &capture );
}

REGISTER_UNIT_TEST( test_contract_dev_token_without_address_is_misuse );

#endif // #if ROCKETNET_DEV

// C5: the default num_channels is one the build accepts

static void test_contract_default_channels_fit_the_build()
{
    struct rocketnet_client_config_t client;
    check( rocketnet_client_default_config( &client ) == ROCKETNET_STATUS_OK );
    check( client.num_channels == ROCKETNET_DEFAULT_NUM_CHANNELS );
    check( client.num_channels == ( ROCKETNET_MAX_CHANNELS < 32 ? ROCKETNET_MAX_CHANNELS : 32 ) );

    struct rocketnet_server_config_t server;
    check( rocketnet_server_default_config( &server ) == ROCKETNET_STATUS_OK );
    check( server.num_channels == ROCKETNET_DEFAULT_NUM_CHANNELS );
}

REGISTER_UNIT_TEST( test_contract_default_channels_fit_the_build );

#if ROCKETNET_DEV

// --- one server and step-driven clients over loopback -----------------------

struct contract_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static double contract_now = 1000.0;
static double contract_clock( void * ) { return contract_now; }

// the callback's record: each transition, and the state a poll read inside it

#define CONTRACT_MAX_TRANSITIONS 64

struct contract_transitions_t
{
    int count;
    int previous[CONTRACT_MAX_TRANSITIONS];
    int current[CONTRACT_MAX_TRANSITIONS];
    rocketnet_client_t * client;
    int polled_mismatches;
};

static contract_transitions_t contract_transitions;

static void contract_state_change( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state )
{
    (void) context;

    contract_transitions_t * t = &contract_transitions;
    if ( t->count < CONTRACT_MAX_TRANSITIONS )
    {
        t->previous[t->count] = previous_state;
        t->current[t->count] = current_state;
        t->count++;
    }

    if ( t->client && rocketnet_client_state( t->client ) != current_state )
        t->polled_mismatches++;
}

// every reported change chains from the one before it, and the last one is
// the state a poll reads now: the callback saw exactly the poll's transitions

static void contract_check_chain( rocketnet_client_t * client )
{
    const contract_transitions_t * t = &contract_transitions;
    int state = ROCKETNET_CLIENT_STATE_DISCONNECTED;
    for ( int i = 0; i < t->count; i++ )
    {
        check( t->previous[i] == state );
        check( t->current[i] != t->previous[i] );
        state = t->current[i];
    }

    check( state == rocketnet_client_state( client ) );
    check( t->polled_mismatches == 0 );
}

static bool contract_saw( int previous, int current )
{
    for ( int i = 0; i < contract_transitions.count; i++ )
        if ( contract_transitions.previous[i] == previous && contract_transitions.current[i] == current )
            return true;
    return false;
}

static void contract_identity( contract_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );

    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_t * contract_server_create( contract_identity_t * identity )
{
    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = CONTRACT_AUDIT_PROTOCOL;
    config.max_clients = 2;
    memcpy( config.backend_public_key, identity->backend_public, sizeof( config.backend_public_key ) );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( server != NULL );

    rocketnet_server_public_key( server, identity->server_public );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    return server;
}

static rocketnet_client_t * contract_client_create()
{
    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = true;
    config.time_callback = contract_clock;
    config.state_change_callback = contract_state_change;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL );
    check( client != NULL );

    memset( &contract_transitions, 0, sizeof( contract_transitions ) );
    contract_transitions.client = client;
    return client;
}

static void contract_token( const contract_identity_t * identity, uint16_t port, uint64_t client_id, char * token_string )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
                                                     CONTRACT_AUDIT_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

// one step: the client, then the server; neither drains its receive queue
static void contract_pump( rocketnet_server_t * server, rocketnet_client_t * client, double * time )
{
    *time += 0.01;
    contract_now = *time;
    rocketnet_client_update( client, *time );
    rocketnet_client_send_packets( client );

    if ( server && rocketnet_server_running( server ) )
    {
        rocketnet_server_update( server, *time );
        rocketnet_server_send_packets( server );
    }
}

static int contract_connect( const contract_identity_t * identity, rocketnet_server_t * server, rocketnet_client_t * client, uint64_t client_id, double * time )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    contract_token( identity, rocketnet_server_port( server ), client_id, token );

    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        contract_pump( server, client, time );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int index = rocketnet_client_index( client );
    check( index >= 0 );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( server, index ); i++ )
        contract_pump( server, client, time );
    check( rocketnet_server_client_connected( server, index ) );

    for ( int i = 0; i < 4; i++ )
        contract_pump( server, client, time );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    return index;
}

// C1: connect reports DISCONNECTED -> REQUESTING_COOKIE at once; the
// handshake reports each state a poll reads; a server kick reports
// CONNECTED -> SERVER_DISCONNECTED, the state the poll reads, and the
// disconnect after it SERVER_DISCONNECTED -> DISCONNECTED. A token refused at
// connect reports its own negative state.

static void test_contract_state_callback_reports_public_state()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    double time = 1000.0;
    contract_now = time;

    rocketnet_server_t * server = contract_server_create( &identity );
    rocketnet_client_t * client = contract_client_create();

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    contract_token( &identity, rocketnet_server_port( server ), 0xC1, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( contract_transitions.count == 1 );
    check( contract_transitions.previous[0] == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( contract_transitions.current[0] == ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE );

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        contract_pump( server, client, &time );
        contract_check_chain( client );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int index = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( server, index ); i++ )
        contract_pump( server, client, &time );

    rocketnet_server_disconnect_client( server, index );
    for ( int i = 0; i < 8 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        contract_pump( server, client, &time );

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    check( contract_saw( ROCKETNET_CLIENT_STATE_CONNECTED, ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED ) );
    check( !contract_saw( ROCKETNET_CLIENT_STATE_CONNECTED, ROCKETNET_CLIENT_STATE_DISCONNECTED ) );
    contract_check_chain( client );

    // the state is kept, and nothing more is reported, across further updates

    const int reported = contract_transitions.count;
    for ( int i = 0; i < 4; i++ )
        contract_pump( server, client, &time );
    check( contract_transitions.count == reported );

    rocketnet_client_disconnect( client );
    check( contract_transitions.count == reported + 1 );
    check( contract_saw( ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED, ROCKETNET_CLIENT_STATE_DISCONNECTED ) );
    contract_check_chain( client );
    rn_test_client_wait_disconnected( client );

    // a token that is not a token: the refusal at connect is reported

    uint8_t zeros[ROCKETNET_CONNECT_TOKEN_BYTES] = {};
    char bad[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( zeros, sizeof( zeros ), bad, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    check( rocketnet_client_connect( client, bad ) == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );
    check( contract_saw( ROCKETNET_CLIENT_STATE_DISCONNECTED, ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN ) );
    contract_check_chain( client );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_state_callback_reports_public_state );

// C2: a message left in the receive queue at update is API misuse, once per
// connection, on the client and on the server; C1: the client's callback
// reports CONNECTED -> MESSAGE_NOT_RECEIVED

static void test_contract_message_not_received_is_misuse()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    double time = 1000.0;
    contract_now = time;

    rocketnet_server_t * server = contract_server_create( &identity );
    rocketnet_client_t * client = contract_client_create();
    const int index = contract_connect( &identity, server, client, 0xC2, &time );

    contract_capture_t capture;
    contract_capture_begin( &capture );

    // the server's message reaches the client, which never receives it

    const uint64_t before = rocketnet_api_misuse_count();
    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( p != NULL );
    memset( p, 7, 4 );
    check( rocketnet_server_finish_message( server, 4 ) != 0 );
    rocketnet_server_send_packets( server );

    for ( int i = 0; i < 32 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        time += 0.01;
        contract_now = time;
        rocketnet_client_update( client, time );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED );
    check( rocketnet_api_misuse_count() == before + 1 );
    check( contract_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    check( contract_saw( ROCKETNET_CLIENT_STATE_CONNECTED, ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED ) );
    contract_check_chain( client );

    contract_capture_end( &capture );

    rn_test_client_stop_and_destroy( client );

    // the client's message reaches the server, which never receives it

    rocketnet_client_t * sender = contract_client_create();
    const int slot = contract_connect( &identity, server, sender, 0xC3, &time );

    contract_capture_begin( &capture );

    const uint64_t server_before = rocketnet_api_misuse_count();
    p = rocketnet_client_start_message( sender, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( p != NULL );
    memset( p, 9, 4 );
    check( rocketnet_client_finish_message( sender, 4 ) != 0 );

    for ( int i = 0; i < 32 && rocketnet_server_client_connected( server, slot ); i++ )
        contract_pump( server, sender, &time );

    check( !rocketnet_server_client_connected( server, slot ) );
    check( rocketnet_api_misuse_count() == server_before + 1 );
    check( contract_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    contract_capture_end( &capture );

    rn_test_client_stop_and_destroy( sender );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_message_not_received_is_misuse );

// C3: a start refused for its arguments returns NULL and logs, one line each

static void test_contract_invalid_start_logs()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    double time = 1000.0;
    contract_now = time;

    rocketnet_server_t * server = contract_server_create( &identity );
    rocketnet_client_t * client = contract_client_create();
    const int index = contract_connect( &identity, server, client, 0xC4, &time );

    contract_capture_t capture;
    contract_capture_begin( &capture );

    const uint64_t before = rocketnet_api_misuse_count();

    check( rocketnet_server_start_message( server, (uint64_t) index, ROCKETNET_MAX_CHANNELS + 8, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( contract_errors == 1 );
    check( rocketnet_client_start_message( client, -1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( contract_errors == 2 );
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE, 0 ) == NULL );
    check( contract_errors == 3 );
    int invalid_reliability = 99;
    check( rocketnet_client_start_message( client, 0, (enum rocketnet_reliability_t) invalid_reliability, 4 ) == NULL );
    check( contract_errors == 4 );
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE, ROCKETNET_MAX_MESSAGE_BYTES + 1 ) == NULL );
    check( contract_errors == 5 );
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_MAX_MESSAGE_BYTES + 1 ) == NULL );
    check( contract_errors == 6 );

    // not API misuse, and the connection carries on

    check( rocketnet_api_misuse_count() == before );
    check( contract_asserts == 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server, index ) );

    contract_capture_end( &capture );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_invalid_start_logs );

// C4: with no connection, and after disconnect, a non-finite or backwards
// update time is counted as API misuse and logged

static void test_contract_update_time_without_connection()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    double time = 1000.0;
    contract_now = time;

    rocketnet_server_t * server = contract_server_create( &identity );
    rocketnet_client_t * client = contract_client_create();

    contract_capture_t capture;
    contract_capture_begin( &capture );

    // never connected

    uint64_t before = rocketnet_api_misuse_count();
    rocketnet_client_update( client, (double) NAN );
    check( rocketnet_api_misuse_count() == before + 1 );
    check( contract_errors == 1 );

    rocketnet_client_update( client, time );
    rocketnet_client_update( client, time - 1.0 );
    check( rocketnet_api_misuse_count() == before + 2 );
    check( contract_errors == 2 );

    // a good time after the backwards step is not misuse

    rocketnet_client_update( client, time + 0.01 );
    check( rocketnet_api_misuse_count() == before + 2 );
    check( contract_asserts == ( ROCKETNET_ASSERTS ? 2 : 0 ) );

    contract_capture_end( &capture );

    // after disconnect: the ended connection's clock carries on

    time += 1.0;
    contract_connect( &identity, server, client, 0xC5, &time );
    rocketnet_client_disconnect( client );

    contract_capture_begin( &capture );

    before = rocketnet_api_misuse_count();
    rocketnet_client_update( client, (double) INFINITY );
    check( rocketnet_api_misuse_count() == before + 1 );
    rocketnet_client_update( client, time - 5.0 );
    check( rocketnet_api_misuse_count() == before + 2 );
    check( contract_errors == 2 );

    contract_capture_end( &capture );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_update_time_without_connection );

// U3: the outcomes recorded for a connection remain readable after an
// explicit disconnect ends it, until the next update

static void test_contract_receipts_after_disconnect()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    double time = 1000.0;
    contract_now = time;

    rocketnet_server_t * server = contract_server_create( &identity );
    rocketnet_client_t * client = contract_client_create();
    contract_connect( &identity, server, client, 0xC6, &time );

    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 4 );
    check( p != NULL );
    memset( p, 1, 4 );
    check( rocketnet_client_finish_message( client, 4 ) != 0 );

    int receipts = 0;
    for ( int i = 0; i < 64 && receipts == 0; i++ )
    {
        contract_pump( server, client, &time );
        while ( rocketnet_server_receive_messages( server ) != NULL )
        {
        }

        receipts = rocketnet_client_receipts( client, NULL, 0 );
    }

    check( receipts == 1 );

    rocketnet_client_disconnect( client );

    struct rocketnet_receipt_t out[4];
    check( rocketnet_client_receipts( client, out, 4 ) == 1 );
    check( out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );

    rocketnet_client_update( client, time + 0.01 );
    check( rocketnet_client_receipts( client, out, 4 ) == 0 );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_receipts_after_disconnect );

// U5: a NULL out with room to fill is a NULL required argument, counted in
// every state: a client never connected and a stopped server count it as a
// connected client and a running server do

static void test_contract_receipts_null_out_is_misuse_in_every_state()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    rocketnet_server_t * server = contract_server_create( &identity );
    rocketnet_client_t * client = contract_client_create();

    contract_capture_t capture;
    contract_capture_begin( &capture );

    // NULL with zero capacity is the count query, never misuse

    uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );
    check( rocketnet_server_receipts( server, 0, NULL, 0 ) == 0 );
    check( rocketnet_api_misuse_count() == before );

    // the client has no connection; the server is running

    before = rocketnet_api_misuse_count();
    check( rocketnet_client_receipts( client, NULL, 4 ) == 0 );
    check( rocketnet_api_misuse_count() == before + 1 );
    check( rocketnet_server_receipts( server, 0, NULL, 4 ) == 0 );
    check( rocketnet_api_misuse_count() == before + 2 );

    // stopped

    rocketnet_server_stop( server );
    check( !rocketnet_server_running( server ) );

    before = rocketnet_api_misuse_count();
    check( rocketnet_server_receipts( server, 0, NULL, 4 ) == 0 );
    check( rocketnet_api_misuse_count() == before + 1 );
    check( contract_asserts == ( ROCKETNET_ASSERTS ? 3 : 0 ) );

    contract_capture_end( &capture );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_receipts_null_out_is_misuse_in_every_state );

// U6: a client index outside 0 .. max_clients - 1 is misuse in
// rocketnet_server_set_send_kbps whatever the kbps, a negative one included,
// running or stopped; a negative kbps for a valid index is a refused value,
// not misuse

static void test_contract_set_send_kbps_index_counted_first()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    rocketnet_server_t * server = contract_server_create( &identity );

    contract_capture_t capture;
    contract_capture_begin( &capture );

    for ( int stopped = 0; stopped < 2; stopped++ )
    {
        if ( stopped )
            rocketnet_server_stop( server );

        uint64_t before = rocketnet_api_misuse_count();
        check( rocketnet_server_set_send_kbps( server, 99, 100 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_api_misuse_count() == before + 1 );
        check( rocketnet_server_set_send_kbps( server, 99, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_api_misuse_count() == before + 2 );
        check( rocketnet_server_set_send_kbps( server, -1, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_api_misuse_count() == before + 3 );

        before = rocketnet_api_misuse_count();
        check( rocketnet_server_set_send_kbps( server, 0, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_server_set_send_kbps( server, 0, 100 ) == ROCKETNET_STATUS_OK );
        check( rocketnet_api_misuse_count() == before );
    }

    contract_capture_end( &capture );

    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_set_send_kbps_index_counted_first );

// U4: the AEAD integrity limit ends the attempt in a negative state. The
// literal 2^36 forgeries are out of a unit case's reach, so the end is staged
// as rocketnet_client_integrity_failure leaves it -- INTEGRITY_LIMIT_REACHED
// counted and the attempt ended in plain DISCONNECTED -- and the next update
// reads it (rocketnet_test_integrity_limit drives the count itself).

static void test_contract_integrity_limit_reads_negative()
{
    rocketnet_quiet( 1 );

    contract_identity_t identity;
    contract_identity( &identity );

    double time = 1000.0;
    contract_now = time;

    rocketnet_server_t * server = contract_server_create( &identity );
    rocketnet_client_t * client = contract_client_create();
    contract_connect( &identity, server, client, 0xC7, &time );

    struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( rocketnet_client_message_client( client ) );
    check( transport != NULL );

    struct rocketnet_counters_t * counters = const_cast<struct rocketnet_counters_t *>( rocketnet_client_transport_counters( transport ) );
    counters->values[ROCKETNET_COUNTER_INTEGRITY_LIMIT_REACHED]++;
    rocketnet_client_transport_disconnect( transport );

    rocketnet_client_update( client, time + 0.01 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION );
    check( contract_saw( ROCKETNET_CLIENT_STATE_CONNECTED, ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION ) );
    contract_check_chain( client );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_contract_integrity_limit_reads_negative );

#endif // #if ROCKETNET_DEV

// --- WITNESSES.tsv protocol and rationale rule witnesses --------------------

#include "../sdk/rocketnet_ack.h"
#include "../sdk/rocketnet_client_protocol.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_keys.h"
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_read_write.h"
#include "../sdk/rocketnet_replay.h"
#include "../sdk/rocketnet_server_protocol.h"
#include "../sdk/rocketnet_test_vectors.h"
#include "../sdk/rocketnet_token_tables.h"

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

static bool witness_bytes_are_zero( const uint8_t * bytes, int count )
{
    for ( int i = 0; i < count; i++ )
    {
        if ( bytes[i] != 0 )
            return false;
    }
    return true;
}

static bool witness_contains_slice( const void * haystack_ptr, size_t haystack_bytes, const uint8_t * needle, size_t needle_bytes )
{
    if ( needle_bytes == 0 || haystack_bytes < needle_bytes )
        return false;
    const uint8_t * haystack = (const uint8_t *) haystack_ptr;
    for ( size_t i = 0; i + needle_bytes <= haystack_bytes; i++ )
    {
        if ( memcmp( haystack + i, needle, needle_bytes ) == 0 )
            return true;
    }
    return false;
}

struct witness_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_address_t second_address;
    uint64_t connection_ids[4];
    uint64_t acked_connection_ids[4 * ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    int end_reasons[4];
};

static void witness_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                        struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof( *private_fields ) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

static void witness_mint_token( uint8_t * token_data, uint64_t client_id, uint64_t create, uint64_t expire, int32_t timeout )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    witness_fill_vector_fields( &signed_fields, &private_fields );
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = timeout;
    private_fields.client_id = client_id;
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void witness_server_setup( struct witness_server_t * test )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    server->send_rate = 10.0;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = &test->counters;
    server->next_connection_id = 1;
    server->connection_ids = test->connection_ids;
    server->acked_connection_ids = test->acked_connection_ids;
    server->end_reasons = test->end_reasons;
    check( server->encryption_map && server->used_token_history &&
           server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, 100.0 );

    check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
    check( rocketnet_address_parse( &test->second_address, "198.51.100.8:56789" ) );
}

static void witness_server_teardown( struct witness_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
}

static void witness_drive_to_sending_response( struct witness_server_t * test,
                                               struct rocketnet_client_protocol_t * client,
                                               double * io_time )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    for ( int step = 0; step < 40 && client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE; step++ )
    {
        rocketnet_client_protocol_update( client, *io_time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
        {
            rocketnet_server_protocol_process_packet( &test->server, &test->client_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, *io_time, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( client, response, response_bytes, *io_time, payload, sizeof( payload ), &payload_bytes );
        }
        *io_time += 0.05;
    }

    check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
}

static void witness_drive_to_connected( struct witness_server_t * test,
                                        struct rocketnet_client_protocol_t * client,
                                        double * io_time )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    for ( int step = 0; step < 60 && client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; step++ )
    {
        rocketnet_client_protocol_update( client, *io_time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
        {
            rocketnet_server_protocol_process_packet( &test->server, &test->client_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, *io_time, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( client, response, response_bytes, *io_time, payload, sizeof( payload ), &payload_bytes );
        }
        *io_time += 0.05;
    }

    check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
}

// 1. t2.2: An endpoint MUST erase an ephemeral secret key, and every derived
// key, when the attempt or connection that owns it ends.
static void test_witness_keys_erased_when_connection_ends()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7001ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );

    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    check( !witness_bytes_are_zero( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );

    witness_drive_to_connected( &srv, &client, &time );

    // While connected, all four derived keys are non-zero on both endpoints.
    check( !witness_bytes_are_zero( client.keys.handshake_client_to_server, ROCKETNET_KEY_BYTES ) );
    check( !witness_bytes_are_zero( client.keys.handshake_server_to_client, ROCKETNET_KEY_BYTES ) );
    check( !witness_bytes_are_zero( client.keys.session_client_to_server, ROCKETNET_KEY_BYTES ) );
    check( !witness_bytes_are_zero( client.keys.session_server_to_client, ROCKETNET_KEY_BYTES ) );

    int mapping_index = rocketnet_encryption_map_find( srv.server.encryption_map, &srv.client_address );
    check( mapping_index >= 0 );
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( srv.server.encryption_map, mapping_index );
    check( !witness_bytes_are_zero( (const uint8_t *) &mapping->keys, sizeof( mapping->keys ) ) );

    // End the connection on both endpoints: every derived key and ephemeral secret is erased.
    uint8_t client_disc[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int client_disc_count = 0;
    int client_disc_bytes = 0;
    rocketnet_client_protocol_disconnect( &client, client_disc, &client_disc_count, &client_disc_bytes );
    check( witness_bytes_are_zero( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
    check( witness_bytes_are_zero( (const uint8_t *) &client.keys, sizeof( client.keys ) ) );

    uint8_t server_disc[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int server_disc_count = 0;
    int server_disc_bytes = 0;
    rocketnet_server_protocol_disconnect_client( &srv.server, 0, server_disc, &server_disc_count, &server_disc_bytes );
    check( witness_bytes_are_zero( (const uint8_t *) &mapping->keys, sizeof( mapping->keys ) ) );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_keys_erased_when_connection_ends );

// 2. t7: Both endpoints MUST erase the shared secret as soon as the four keys
// are derived.
// Behavioral regression slice: verifies key derivation agrees across client and
// server and that neither rocketnet_connection_keys_t, rocketnet_client_protocol_t,
// nor rocketnet_encryption_mapping_t retains the raw X25519 shared secret.
// This output-state check does not observe stack-frame zeroing
// (sodium_memzero(shared_secret, ...)) inside rocketnet_derive_connection_keys. The instrumented source test
// test_key_schedule_erases_live_stack_buffers observes those wipes directly.
static void test_witness_shared_secret_not_retained_in_output_state()
{
    uint8_t raw_shared[32];
    check( crypto_scalarmult( raw_shared, rocketnet_test_client_eph_secret, rocketnet_test_server_eph_public ) == 0 );
    check( !witness_bytes_are_zero( raw_shared, 32 ) );

    struct rocketnet_connection_keys_t client_keys;
    memset( &client_keys, 0, sizeof( client_keys ) );
    check( rocketnet_derive_connection_keys( &client_keys,
                                             rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                             rocketnet_test_salt, rocketnet_test_session_nonce,
                                             rocketnet_test_client_eph_secret, rocketnet_test_server_eph_public,
                                             rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) == 1 );

    struct rocketnet_connection_keys_t server_keys;
    memset( &server_keys, 0, sizeof( server_keys ) );
    check( rocketnet_derive_connection_keys( &server_keys,
                                             rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                             rocketnet_test_salt, rocketnet_test_session_nonce,
                                             rocketnet_test_server_eph_secret, rocketnet_test_client_eph_public,
                                             rocketnet_test_client_eph_public, rocketnet_test_server_eph_public ) == 1 );

    check( memcmp( &client_keys, &server_keys, sizeof( client_keys ) ) == 0 );
    check( !witness_contains_slice( &client_keys, sizeof( client_keys ), raw_shared, 32 ) );
    check( !witness_contains_slice( &server_keys, sizeof( server_keys ), raw_shared, 32 ) );

    // Also verify across a full protocol exchange that neither client nor server
    // retains the X25519 shared secret anywhere in its state.
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    // The byte scan below includes padding and inactive storage that protocol
    // init need not write. Define those bytes before observing the whole object.
    memset( &client, 0, sizeof( client ) );
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7002ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t client_eph_secret_copy[32];
    memcpy( client_eph_secret_copy, client.client_ephemeral_secret_key, 32 );

    witness_drive_to_sending_response( &srv, &client, &time );

    int mapping_index = rocketnet_encryption_map_find( srv.server.encryption_map, &srv.client_address );
    check( mapping_index >= 0 );
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( srv.server.encryption_map, mapping_index );

    uint8_t attempt_shared[32];
    check( crypto_scalarmult( attempt_shared, client_eph_secret_copy, mapping->server_ephemeral_public_key ) == 0 );
    check( !witness_contains_slice( &client, sizeof( client ), attempt_shared, 32 ) );
    check( !witness_contains_slice( mapping, sizeof( *mapping ), attempt_shared, 32 ) );

    sodium_memzero( raw_shared, sizeof( raw_shared ) );
    sodium_memzero( client_eph_secret_copy, sizeof( client_eph_secret_copy ) );
    sodium_memzero( attempt_shared, sizeof( attempt_shared ) );
    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_shared_secret_not_retained_in_output_state );

// 3. t7: MUST erase an ephemeral secret key when no handshake that needs it
// remains.
static void test_witness_ephemeral_secret_erased_when_handshake_done()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7003ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );

    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    check( !witness_bytes_are_zero( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );

    // Once the challenge is processed and the client enters SENDING_CONNECTION_RESPONSE (3),
    // no handshake message needing the client ephemeral secret key remains, so it is already zeroed.
    witness_drive_to_sending_response( &srv, &client, &time );
    check( witness_bytes_are_zero( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
    check( !witness_bytes_are_zero( client.keys.session_client_to_server, ROCKETNET_KEY_BYTES ) );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_ephemeral_secret_erased_when_handshake_done );

// 4. t7: the server MUST generate a fresh keypair for every mapping.
static void test_witness_server_ephemeral_keypair_per_mapping()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t c1_counters;
    struct rocketnet_counters_t c2_counters;
    memset( &c1_counters, 0, sizeof( c1_counters ) );
    memset( &c2_counters, 0, sizeof( c2_counters ) );

    struct rocketnet_client_protocol_t c1;
    struct rocketnet_client_protocol_t c2;
    rocketnet_client_protocol_init( &c1, &c1_counters );
    rocketnet_client_protocol_init( &c2, &c2_counters );

    uint8_t t1[ROCKETNET_CONNECT_TOKEN_BYTES];
    uint8_t t2[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( t1, 0x7004AULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    witness_mint_token( t2, 0x7004BULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );

    double time1 = 100.0;
    double time2 = 100.0;
    check( rocketnet_client_protocol_connect( &c1, t1, time1, ROCKETNET_TEST_CREATE_TS ) );
    check( rocketnet_client_protocol_connect( &c2, t2, time2, ROCKETNET_TEST_CREATE_TS ) );

    witness_drive_to_sending_response( &srv, &c1, &time1 );

    // Drive c2 from second_address to SENDING_CONNECTION_RESPONSE.
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;
    for ( int step = 0; step < 40 && c2.state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE; step++ )
    {
        rocketnet_client_protocol_update( &c2, time2, packet, &packet_bytes );
        if ( packet_bytes > 0 )
        {
            rocketnet_server_protocol_process_packet( &srv.server, &srv.second_address, packet, packet_bytes,
                                                      ROCKETNET_TEST_CREATE_TS, time2, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( &c2, response, response_bytes, time2, payload, sizeof( payload ), &payload_bytes );
        }
        time2 += 0.05;
    }
    check( c2.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    int idx1 = rocketnet_encryption_map_find( srv.server.encryption_map, &srv.client_address );
    int idx2 = rocketnet_encryption_map_find( srv.server.encryption_map, &srv.second_address );
    check( idx1 >= 0 && idx2 >= 0 && idx1 != idx2 );

    struct rocketnet_encryption_mapping_t * m1 = rocketnet_encryption_map_entry( srv.server.encryption_map, idx1 );
    struct rocketnet_encryption_mapping_t * m2 = rocketnet_encryption_map_entry( srv.server.encryption_map, idx2 );

    check( !witness_bytes_are_zero( m1->server_ephemeral_public_key, ROCKETNET_KEY_BYTES ) );
    check( !witness_bytes_are_zero( m2->server_ephemeral_public_key, ROCKETNET_KEY_BYTES ) );
    check( memcmp( m1->server_ephemeral_public_key, m2->server_ephemeral_public_key, ROCKETNET_KEY_BYTES ) != 0 );
    check( memcmp( m1->keys.session_server_to_client, m2->keys.session_server_to_client, ROCKETNET_KEY_BYTES ) != 0 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_server_ephemeral_keypair_per_mapping );

// 5. t11.1: A client that accepts a cookie MUST store its last 24 bytes and
// MUST send nothing in response.
static void test_witness_cookie_stores_last_24_bytes_and_sends_nothing()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7005ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    check( rocketnet_client_protocol_connect( &client, token, 100.0, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t out_packet[ROCKETNET_RECEIVE_CEILING];
    int out_bytes = 0;
    rocketnet_client_protocol_update( &client, 100.0, out_packet, &out_bytes );
    check( out_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    uint8_t cookie_packet[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    uint8_t * p = cookie_packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_COOKIE_PACKET, client.packet_filter );
    rocketnet_write_bytes( &p, client.connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    uint8_t expected_24[ROCKETNET_COOKIE_FIELD_BYTES];
    for ( int i = 0; i < ROCKETNET_COOKIE_FIELD_BYTES; i++ )
        expected_24[i] = (uint8_t) ( 0xA0 + i );
    rocketnet_write_bytes( &p, expected_24, ROCKETNET_COOKIE_FIELD_BYTES );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = -1;
    check( rocketnet_client_protocol_process_packet( &client, cookie_packet, sizeof( cookie_packet ), 100.01,
                                                     payload, sizeof( payload ), &payload_bytes ) == 1 );
    check( payload_bytes == 0 );
    check( client.has_cookie == 1 );
    check( memcmp( client.return_cookie, cookie_packet + ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES - 24, 24 ) == 0 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

    // Accepting the cookie does not trigger an immediate send: an update before
    // the 0.1s send interval elapses produces 0 bytes.
    rocketnet_client_protocol_update( &client, 100.01, out_packet, &out_bytes );
    check( out_bytes == 0 );
}

REGISTER_UNIT_TEST( test_witness_cookie_stores_last_24_bytes_and_sends_nothing );

// 6. t11.2: a denial in sending connection response (3) MUST open under the
// handshake server-to-client key the client already holds.
static void test_witness_denial_in_response_opens_under_held_handshake_key()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7006ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    witness_drive_to_sending_response( &srv, &client, &time );

    // A denial sealed under keys derived from a different carried nonce must NOT open in state 3.
    const uint8_t alt_nonce[ROCKETNET_SESSION_NONCE_BYTES] = { 0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4 };
    struct rocketnet_connection_keys_t alt_keys;
    rocketnet_derive_handshake_keys( &alt_keys, rocketnet_test_token_c2s_key, rocketnet_test_token_s2c_key,
                                     client.connection_salt, alt_nonce );

    uint8_t denied_wrong_key[ROCKETNET_CONNECTION_DENIED_WIRE_BYTES];
    check( rocketnet_write_sealed_packet( denied_wrong_key, ROCKETNET_CONNECTION_DENIED_PACKET,
                                          client.packet_filter, 2, alt_nonce, NULL, 0,
                                          ROCKETNET_TEST_PROTOCOL_ID, client.connection_salt, NULL,
                                          alt_keys.handshake_server_to_client ) == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_packet( &client, denied_wrong_key, sizeof( denied_wrong_key ), time,
                                                     payload, sizeof( payload ), &payload_bytes ) == 0 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    check( client_counters.values[ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN] == 1 );

    // A denial sealed under the handshake_server_to_client key the client already holds
    // (even carrying alt_nonce in the header) MUST open and transition to CONNECTION_DENIED.
    uint8_t held_s2c_key[ROCKETNET_KEY_BYTES];
    memcpy( held_s2c_key, client.keys.handshake_server_to_client, ROCKETNET_KEY_BYTES );

    uint8_t denied_held_key[ROCKETNET_CONNECTION_DENIED_WIRE_BYTES];
    check( rocketnet_write_sealed_packet( denied_held_key, ROCKETNET_CONNECTION_DENIED_PACKET,
                                          client.packet_filter, 2, alt_nonce, NULL, 0,
                                          ROCKETNET_TEST_PROTOCOL_ID, client.connection_salt, NULL,
                                          held_s2c_key ) == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );

    check( rocketnet_client_protocol_process_packet( &client, denied_held_key, sizeof( denied_held_key ), time,
                                                     payload, sizeof( payload ), &payload_bytes ) == 1 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_denial_in_response_opens_under_held_handshake_key );

// The supported unkeyed mode must compare every filter bit. The constant-time
// requirement in t5.1 applies to keyed epochs, which this SDK rejects.
// Behavioral regression slice: verifies rocketnet_prefilter_check compares every
// bit of the 32-bit filter value and rejects any single-bit flip.
// This is not a witness for an unimplemented keyed-mode MAC comparison.
static void test_witness_prefilter_compares_all_filter_bits()
{
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x52, 0x4B, 0x54, 0x4E };
    uint8_t packet[ROCKETNET_PREFILTER_BYTES + 16];
    memset( packet, 0, sizeof( packet ) );
    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, filter );

    check( rocketnet_prefilter_check( packet, sizeof( packet ), filter ) == ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    check( sodium_memcmp( packet + 2, filter, ROCKETNET_FILTER_VALUE_BYTES ) == 0 );

    for ( int byte_idx = 0; byte_idx < ROCKETNET_FILTER_VALUE_BYTES; byte_idx++ )
    {
        for ( int bit = 0; bit < 8; bit++ )
        {
            packet[2 + byte_idx] ^= (uint8_t) ( 1u << bit );
            check( sodium_memcmp( packet + 2, filter, ROCKETNET_FILTER_VALUE_BYTES ) != 0 );
            check( rocketnet_prefilter_check( packet, sizeof( packet ), filter ) == ROCKETNET_PREFILTER_WRONG_VALUE );
            packet[2 + byte_idx] ^= (uint8_t) ( 1u << bit );
        }
    }
}

REGISTER_UNIT_TEST( test_witness_prefilter_compares_all_filter_bits );

// Keyed epochs are outside the current SDK. Keep their requirements visible
// as deferred, and reject every nonzero epoch rather than accepting it under
// the unkeyed comparison. This test does not implement or prove a keyed MAC.
static void test_witness_keyed_filter_epochs_are_unsupported()
{
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x52, 0x4B, 0x54, 0x4E };
    uint8_t packet[ROCKETNET_PREFILTER_BYTES + 16] = {};
    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, filter );
    for ( int epoch = 1; epoch <= 255; epoch++ )
    {
        packet[1] = (uint8_t) epoch;
        check( rocketnet_prefilter_check( packet, sizeof( packet ), filter ) == ROCKETNET_PREFILTER_WRONG_EPOCH );
    }
}

REGISTER_UNIT_TEST( test_witness_keyed_filter_epochs_are_unsupported );

// 9. t14: an implementation must not throttle application sends to preserve an
// assumed time span of history.
static void test_witness_application_sends_never_throttled_by_history()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7009ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    witness_drive_to_connected( &srv, &client, &time );

    // Send more than twice the sent-packet history depth at a single instant (dt = 0)
    // with zero acks arriving: no send is ever throttled or refused.
    const int total_sends = 2 * ROCKETNET_SENT_PACKET_BUFFER_SIZE + 64;
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE]{};
    for ( int i = 0; i < total_sends; i++ )
    {
        uint64_t seq = 0;
        check( rocketnet_client_protocol_start_payload( &client, &seq ) == 1 );
        check( rocketnet_client_protocol_seal_payload_frame( &client, seq, (uint64_t) ( i + 1 ), frame, 4, time ) > 0 );
    }

    check( client_counters.values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] >= (uint64_t) ROCKETNET_SENT_PACKET_BUFFER_SIZE );

    // Receiving a server packet afterwards still processes normally.
    uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
    int ka_bytes = rocketnet_server_write_keep_alive( &srv.server, 0, time, keep_alive );
    check( ka_bytes > 0 );
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_packet( &client, keep_alive, ka_bytes, time, payload, sizeof( payload ), &payload_bytes ) == 1 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_application_sends_never_throttled_by_history );

// 10. t14: Implementations must document their retained depth.
static void test_witness_replay_retained_depth_exact()
{
    check( ROCKETNET_REPLAY_WINDOW_BITS == ROCKETNET_HISTORY_PACKETS );
    check( ROCKETNET_SENT_PACKET_BUFFER_SIZE == ROCKETNET_HISTORY_PACKETS );
    check( ROCKETNET_REPLAY_WINDOW_BITS >= 4096 && ROCKETNET_REPLAY_WINDOW_BITS <= 32768 );

    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    const uint64_t high = 2 * (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS + 1;
    rocketnet_replay_advance( &replay, high );

    const uint64_t oldest_in_window = high - ROCKETNET_REPLAY_WINDOW_BITS + 1;
    const uint64_t first_below_window = high - ROCKETNET_REPLAY_WINDOW_BITS;

    check( !rocketnet_replay_already_received( &replay, oldest_in_window ) );
    rocketnet_replay_advance( &replay, oldest_in_window );
    check( rocketnet_replay_already_received( &replay, oldest_in_window ) );
    check( rocketnet_replay_already_received( &replay, first_below_window ) );
}

REGISTER_UNIT_TEST( test_witness_replay_retained_depth_exact );

// 11. t12: Every lookup a server performs per packet or per connection must be
// constant time in the number of client slots.
// Behavioral regression slice: populates 64 distinct addresses and client IDs
// into small (64-slot) and large (512-slot) tables, verifying exact lookup
// resolution and non-member rejection in both DEV=ON and DEV=OFF builds, plus
// per-lookup hash and probe bounds via DEV seams under ROCKETNET_DEV.
// The ledger records the complexity argument as source review. These finite
// cases check keyed lookup and capacity bounds, not worst-case constant cost.
static void test_witness_server_keyed_lookup_probe_bounds()
{
    enum
    {
        NUM_POPULATED = 64
    };
    struct rocketnet_encryption_map_t * small_map = rocketnet_encryption_map_create( NULL, 128 );
    struct rocketnet_encryption_map_t * large_map = rocketnet_encryption_map_create( NULL, 1024 );
    struct rocketnet_client_slots_t * small_slots = rocketnet_client_slots_create( NULL, 64 );
    struct rocketnet_client_slots_t * large_slots = rocketnet_client_slots_create( NULL, 512 );
    check( small_map && large_map && small_slots && large_slots );

    struct rocketnet_address_t addrs[NUM_POPULATED];
    uint64_t client_ids[NUM_POPULATED];
    int s_map_idx[NUM_POPULATED];
    int l_map_idx[NUM_POPULATED];
    int s_slot[NUM_POPULATED];
    int l_slot[NUM_POPULATED];

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );

    for ( int i = 0; i < NUM_POPULATED; i++ )
    {
        char addr_text[64];
        snprintf( addr_text, sizeof( addr_text ), "198.51.100.%d:%d", 1 + i, 40000 + i );
        check( rocketnet_address_parse( &addrs[i], addr_text ) );

        client_ids[i] = UINT64_C( 0xCAFE0000 ) + (uint64_t) i;
        s_map_idx[i] = rocketnet_encryption_map_add( small_map, &addrs[i], 100.0 );
        l_map_idx[i] = rocketnet_encryption_map_add( large_map, &addrs[i], 100.0 );
        check( s_map_idx[i] >= 0 && l_map_idx[i] >= 0 );

        s_slot[i] = rocketnet_client_slots_assign( small_slots, client_ids[i], s_map_idx[i], (uint64_t) ( i + 1 ), user_data );
        l_slot[i] = rocketnet_client_slots_assign( large_slots, client_ids[i], l_map_idx[i], (uint64_t) ( i + 1 ), user_data );
        check( s_slot[i] >= 0 && l_slot[i] >= 0 );
    }

    for ( int i = 0; i < NUM_POPULATED; i++ )
    {
        check( rocketnet_encryption_map_find( small_map, &addrs[i] ) == s_map_idx[i] );
        check( rocketnet_encryption_map_find( large_map, &addrs[i] ) == l_map_idx[i] );
        check( rocketnet_client_slots_find( small_slots, client_ids[i] ) == s_slot[i] );
        check( rocketnet_client_slots_find( large_slots, client_ids[i] ) == l_slot[i] );

#if ROCKETNET_DEV
        const uint64_t s_hashes_before = rocketnet_encryption_map_test_hashes( small_map );
        const uint64_t s_probes_before = rocketnet_encryption_map_test_probes( small_map );
        check( rocketnet_encryption_map_find( small_map, &addrs[i] ) == s_map_idx[i] );
        const uint64_t s_hashes = rocketnet_encryption_map_test_hashes( small_map ) - s_hashes_before;
        const uint64_t s_probes = rocketnet_encryption_map_test_probes( small_map ) - s_probes_before;

        const uint64_t l_hashes_before = rocketnet_encryption_map_test_hashes( large_map );
        const uint64_t l_probes_before = rocketnet_encryption_map_test_probes( large_map );
        check( rocketnet_encryption_map_find( large_map, &addrs[i] ) == l_map_idx[i] );
        const uint64_t l_hashes = rocketnet_encryption_map_test_hashes( large_map ) - l_hashes_before;
        const uint64_t l_probes = rocketnet_encryption_map_test_probes( large_map ) - l_probes_before;

        check( s_hashes == 1 && l_hashes == 1 );
        check( s_probes >= 1 && s_probes <= rocketnet_encryption_map_test_probe_bound( small_map ) );
        check( l_probes >= 1 && l_probes <= rocketnet_encryption_map_test_probe_bound( large_map ) );

        check( rocketnet_client_slots_find( small_slots, client_ids[i] ) == s_slot[i] );
        check( rocketnet_client_slots_test_last_probes( small_slots ) >= 1 );
        check( rocketnet_client_slots_test_last_probes( small_slots ) <= (uint64_t) NUM_POPULATED );

        check( rocketnet_client_slots_find( large_slots, client_ids[i] ) == l_slot[i] );
        check( rocketnet_client_slots_test_last_probes( large_slots ) >= 1 );
        check( rocketnet_client_slots_test_last_probes( large_slots ) <= (uint64_t) NUM_POPULATED );
#endif
    }

    struct rocketnet_address_t missing_addr;
    check( rocketnet_address_parse( &missing_addr, "198.51.100.250:49999" ) );
    check( rocketnet_encryption_map_find( small_map, &missing_addr ) == -1 );
    check( rocketnet_encryption_map_find( large_map, &missing_addr ) == -1 );
    check( rocketnet_client_slots_find( small_slots, UINT64_C( 0xDEADBEEF ) ) == -1 );
    check( rocketnet_client_slots_find( large_slots, UINT64_C( 0xDEADBEEF ) ) == -1 );

    rocketnet_encryption_map_destroy( small_map );
    rocketnet_encryption_map_destroy( large_map );
    rocketnet_client_slots_destroy( small_slots );
    rocketnet_client_slots_destroy( large_slots );
}

REGISTER_UNIT_TEST( test_witness_server_keyed_lookup_probe_bounds );

// 12. t12: The address and client-id lookups are hash tables, and the hash must
// be keyed with a secret generated at server start.
static void test_witness_hash_tables_keyed_at_server_start()
{
    struct rocketnet_encryption_map_t * map_a = rocketnet_encryption_map_create( NULL, 64 );
    struct rocketnet_encryption_map_t * map_b = rocketnet_encryption_map_create( NULL, 64 );
    struct rocketnet_client_slots_t * slots_a = rocketnet_client_slots_create( NULL, 32 );
    struct rocketnet_client_slots_t * slots_b = rocketnet_client_slots_create( NULL, 32 );
    check( map_a && map_b && slots_a && slots_b );

    // Both tables insert, find, and remove entries via their keyed hash tables.
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );
    for ( int i = 0; i < 16; i++ )
    {
        struct rocketnet_address_t addr;
        memset( &addr, 0, sizeof( addr ) );
        addr.type = ROCKETNET_ADDRESS_IPV4;
        addr.data.ipv4[0] = 10;
        addr.data.ipv4[3] = (uint8_t) ( i + 1 );
        addr.port = (uint16_t) ( 30000 + i );

        int ma = rocketnet_encryption_map_add( map_a, &addr, 100.0 );
        int mb = rocketnet_encryption_map_add( map_b, &addr, 100.0 );
        check( ma == i && mb == i );
        check( rocketnet_encryption_map_find( map_a, &addr ) == ma );
        check( rocketnet_encryption_map_find( map_b, &addr ) == mb );

        uint64_t cid = UINT64_C( 0x100000 ) + (uint64_t) i;
        int sa = rocketnet_client_slots_assign( slots_a, cid, ma, (uint64_t) ( i + 1 ), user_data );
        int sb = rocketnet_client_slots_assign( slots_b, cid, mb, (uint64_t) ( i + 1 ), user_data );
        check( sa == i && sb == i );
        check( rocketnet_client_slots_find( slots_a, cid ) == sa );
        check( rocketnet_client_slots_find( slots_b, cid ) == sb );
    }

#if ROCKETNET_DEV
    // Across 16 inputs in a 64-bucket table, two independent random SipHash keys
    // differ on at least one home bucket with overwhelming probability (1 - 64^-16).
    int map_bucket_diffs = 0;
    int slot_bucket_diffs = 0;
    for ( int i = 0; i < 16; i++ )
    {
        struct rocketnet_address_t addr;
        memset( &addr, 0, sizeof( addr ) );
        addr.type = ROCKETNET_ADDRESS_IPV4;
        addr.data.ipv4[0] = 10;
        addr.data.ipv4[3] = (uint8_t) ( i + 1 );
        addr.port = (uint16_t) ( 30000 + i );
        if ( rocketnet_encryption_map_test_home( map_a, &addr ) != rocketnet_encryption_map_test_home( map_b, &addr ) )
            map_bucket_diffs++;

        uint64_t cid = UINT64_C( 0x100000 ) + (uint64_t) i;
        if ( rocketnet_client_slots_test_hash_index( slots_a, cid ) != rocketnet_client_slots_test_hash_index( slots_b, cid ) )
            slot_bucket_diffs++;
    }
    check( map_bucket_diffs > 0 );
    check( slot_bucket_diffs > 0 );
#endif

    rocketnet_encryption_map_destroy( map_a );
    rocketnet_encryption_map_destroy( map_b );
    rocketnet_client_slots_destroy( slots_a );
    rocketnet_client_slots_destroy( slots_b );
}

REGISTER_UNIT_TEST( test_witness_hash_tables_keyed_at_server_start );

// 14. tc: Pushing work down reduces the volume that reaches userspace rather
// than removing the check from it.
static void test_witness_userspace_retains_all_lower_layer_checks()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client = -1;

    // (a) Too short (< 6 bytes)
    uint8_t short_pkt[3] = { 0, 0, 0 };
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, short_pkt, 3,
                                                     ROCKETNET_TEST_CREATE_TS, 100.0, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( srv.counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_SHORT] == 1 );

    // (b) Too large (> ROCKETNET_RECEIVE_CEILING)
    uint8_t large_pkt[ROCKETNET_RECEIVE_CEILING + 1];
    memset( large_pkt, 0, sizeof( large_pkt ) );
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, large_pkt, sizeof( large_pkt ),
                                                     ROCKETNET_TEST_CREATE_TS, 100.0, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( srv.counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == 1 );

    // (c) Wrong filter epoch
    uint8_t bad_epoch[32];
    memset( bad_epoch, 0, sizeof( bad_epoch ) );
    uint8_t * p = bad_epoch;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, srv.server.packet_filter );
    bad_epoch[1] = 1;
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, bad_epoch, sizeof( bad_epoch ),
                                                     ROCKETNET_TEST_CREATE_TS, 100.0, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( srv.counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH] == 1 );

    // (d) Wrong filter value
    uint8_t bad_filter[32];
    memset( bad_filter, 0, sizeof( bad_filter ) );
    p = bad_filter;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, srv.server.packet_filter );
    bad_filter[2] ^= 0xFF;
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, bad_filter, sizeof( bad_filter ),
                                                     ROCKETNET_TEST_CREATE_TS, 100.0, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( srv.counters.values[ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE] == 1 );

    // (e) Unknown / reserved packet type
    uint8_t bad_type[32];
    memset( bad_type, 0, sizeof( bad_type ) );
    p = bad_type;
    rocketnet_write_prefilter_header( &p, 99, srv.server.packet_filter );
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, bad_type, sizeof( bad_type ),
                                                     ROCKETNET_TEST_CREATE_TS, 100.0, response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( srv.counters.values[ROCKETNET_COUNTER_RESERVED_TYPE] == 1 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_userspace_retains_all_lower_layer_checks );

// 15. t15.1: It is stated as a must because rules elsewhere depend on it
// (rocketnet_ack_block_build must not name a sequence its sender has not
// opened, because keep-alive generation reads an ack as the client's claim to
// have received a keep-alive).
static void test_witness_ack_block_truthfulness_preserves_keep_alive_latch()
{
    struct rocketnet_ack_endpoint_t server_endpoint;
    rocketnet_ack_endpoint_reset( &server_endpoint );

    // Server sends keep-alive at sequence 1 (which is lost) and payload at sequence 2 (which arrives).
    rocketnet_ack_endpoint_record_sent( &server_endpoint, 1, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES, 0, 0, 100.0 );
    rocketnet_ack_endpoint_mark_keep_alive( &server_endpoint, 1 );
    rocketnet_ack_endpoint_record_sent( &server_endpoint, 2, 64, 1, 1, 100.05 );

    // Client opens ONLY sequence 2, never sequence 1.
    struct rocketnet_replay_protection_t client_opened;
    memset( &client_opened, 0, sizeof( client_opened ) );
    rocketnet_replay_advance( &client_opened, 2 );

    uint8_t block_bytes_buf[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int written = rocketnet_ack_block_build( block_bytes_buf, sizeof( block_bytes_buf ), &client_opened );
    check( written == ROCKETNET_ACK_BLOCK_MIN_BYTES );

    struct rocketnet_ack_block_t parsed;
    check( rocketnet_ack_block_read( &parsed, block_bytes_buf, written, 2 ) == written );
    check( parsed.largest == 2 );
    check( parsed.first_range_length == 1 );
    check( parsed.num_ranges == 0 );

    uint64_t acked_ids[16];
    int num_acked = 0;
    uint64_t unknown_seq = 0;
    uint64_t batch_full = 0;
    rocketnet_ack_endpoint_process_block( &server_endpoint, &parsed, 100.1,
                                          acked_ids, 16, &num_acked, &unknown_seq, &batch_full );

    check( num_acked == 1 && acked_ids[0] == 1 );
    check( server_endpoint.keep_alive_acked == 0 );
}

REGISTER_UNIT_TEST( test_witness_ack_block_truthfulness_preserves_keep_alive_latch );

// 16. t17: A sender MUST keep the send rate constant for the duration of one
// attempt.
static void test_witness_client_send_rate_constant_across_attempt()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );
    const double initial_rate = client.send_rate;
    const double interval = 1.0 / initial_rate + 1e-6;
    check( initial_rate == 10.0 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7016ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );

    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    check( client.send_rate == initial_rate );

    uint8_t pkt[ROCKETNET_RECEIVE_CEILING];
    int pkt_bytes = 0;
    uint8_t resp[ROCKETNET_RECEIVE_CEILING];
    int resp_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client = -1;

    // State 1 (REQUESTING_COOKIE): sends at t=100.0, silent at +0.05, sends at +0.10.
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    check( pkt_bytes > 0 && client.send_rate == initial_rate );
    rocketnet_client_protocol_update( &client, time + 0.5 * interval, pkt, &pkt_bytes );
    check( pkt_bytes == 0 && client.send_rate == initial_rate );
    time += interval;
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    check( pkt_bytes > 0 && client.send_rate == initial_rate );

    // Deliver request to get cookie -> State 2 (SENDING_CONNECTION_REQUEST).
    rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                              payload, &payload_bytes, &payload_client );
    rocketnet_client_protocol_process_packet( &client, resp, resp_bytes, time, payload, sizeof( payload ), &payload_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    check( client.send_rate == initial_rate );

    // State 2 pacing: silent at +0.05, sends at +0.10.
    rocketnet_client_protocol_update( &client, time + 0.5 * interval, pkt, &pkt_bytes );
    check( pkt_bytes == 0 );
    time += interval;
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    check( pkt_bytes > 0 && client.send_rate == initial_rate );

    // Deliver cookie-bearing request to get challenge -> State 3 (SENDING_CONNECTION_RESPONSE).
    rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                              payload, &payload_bytes, &payload_client );
    rocketnet_client_protocol_process_packet( &client, resp, resp_bytes, time, payload, sizeof( payload ), &payload_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    check( client.send_rate == initial_rate );

    // State 3 pacing: silent at +0.05, sends at +0.10.
    rocketnet_client_protocol_update( &client, time + 0.5 * interval, pkt, &pkt_bytes );
    check( pkt_bytes == 0 );
    time += interval;
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    check( pkt_bytes > 0 && client.send_rate == initial_rate );

    // Deliver response to get keep-alive -> State 4 (CONNECTED).
    rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                              payload, &payload_bytes, &payload_client );
    rocketnet_client_protocol_process_packet( &client, resp, resp_bytes, time, payload, sizeof( payload ), &payload_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( client.send_rate == initial_rate );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_client_send_rate_constant_across_attempt );

// 17. t18: An endpoint MUST expose the state of each connection and the reason
// a connection ended.
static void test_witness_endpoints_expose_state_and_end_reason()
{
    const int all_states[] = {
        ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE,
        ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST,
        ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED,
    };

    for ( size_t i = 0; i < sizeof( all_states ) / sizeof( all_states[0] ); i++ )
    {
        const char * name = rocketnet_client_transport_state_name( all_states[i] );
        check( name != NULL && strcmp( name, "???" ) != 0 );
        for ( size_t j = i + 1; j < sizeof( all_states ) / sizeof( all_states[0] ); j++ )
            check( strcmp( name, rocketnet_client_transport_state_name( all_states[j] ) ) != 0 );
    }

    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7017ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 5 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    witness_drive_to_connected( &srv, &client, &time );

    check( rocketnet_client_slots_connected( srv.server.client_slots, 0 ) == 1 );
    check( rocketnet_server_protocol_end_reason( &srv.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // Time out the connection on both sides and verify both endpoints expose the reason until next connect.
    uint8_t pkt[ROCKETNET_RECEIVE_CEILING];
    int pkt_bytes = 0;
    rocketnet_client_protocol_update( &client, time + 10.0, pkt, &pkt_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT );
    rocketnet_client_protocol_update( &client, time + 11.0, pkt, &pkt_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT );

    rocketnet_server_protocol_update( &srv.server, time + 10.0 );
    check( rocketnet_client_slots_connected( srv.server.client_slots, 0 ) == 0 );
    check( rocketnet_server_protocol_end_reason( &srv.server, 0 ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_endpoints_expose_state_and_end_reason );

// 18. t11: The handshake and session sending sequences MUST start at one and
// MUST NOT be reset within the attempt.
static void test_witness_sending_sequences_start_at_one_and_never_reset()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7018ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );

    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    check( client.send_sequence_handshake == 1 );
    check( client.send_sequence_session == 1 );

    uint8_t pkt[ROCKETNET_RECEIVE_CEILING];
    int pkt_bytes = 0;
    uint8_t resp[ROCKETNET_RECEIVE_CEILING];
    int resp_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client = -1;

    // Request in state 1 -> cookie -> transition to state 2 (sequences still 1).
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                              payload, &payload_bytes, &payload_client );
    rocketnet_client_protocol_process_packet( &client, resp, resp_bytes, time, payload, sizeof( payload ), &payload_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    check( client.send_sequence_handshake == 1 );
    check( client.send_sequence_session == 1 );

    // Request in state 2 -> challenge -> builds stored_response at handshake sequence 1 (advancing to 2) and enters state 3.
    time += 0.11;
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                              payload, &payload_bytes, &payload_client );
    rocketnet_client_protocol_process_packet( &client, resp, resp_bytes, time, payload, sizeof( payload ), &payload_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    check( client.send_sequence_handshake == 2 );
    check( client.send_sequence_session == 1 );

    // Send two connection responses in state 3: stored_response carries sequence 1 and neither counter resets.
    time += 0.11;
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    check( pkt_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    const uint8_t * seq_ptr = pkt + ROCKETNET_PREFILTER_BYTES;
    check( rocketnet_read_uint32( &seq_ptr ) == 1 );
    check( client.send_sequence_handshake == 2 );
    time += 0.11;
    rocketnet_client_protocol_update( &client, time, pkt, &pkt_bytes );
    check( pkt_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( client.send_sequence_handshake == 2 );

    // Transition to CONNECTED on keep-alive: neither sequence resets.
    rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                              payload, &payload_bytes, &payload_client );
    rocketnet_client_protocol_process_packet( &client, resp, resp_bytes, time, payload, sizeof( payload ), &payload_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( client.send_sequence_handshake == 2 );
    check( client.send_sequence_session == 1 );

    uint64_t s1 = 0;
    uint64_t s2 = 0;
    check( rocketnet_client_protocol_start_payload( &client, &s1 ) && s1 == 1 );
    check( rocketnet_client_protocol_start_payload( &client, &s2 ) && s2 == 2 );
    check( client.send_sequence_session == 3 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_sending_sequences_start_at_one_and_never_reset );

// 19. t11.1: A client that accepts a cookie MUST store its last 24 bytes.
static void test_witness_client_stores_and_echoes_cookie_24_bytes()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7019ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    check( rocketnet_client_protocol_connect( &client, token, 100.0, ROCKETNET_TEST_CREATE_TS ) );
    check( client.has_cookie == 0 );
    check( witness_bytes_are_zero( client.return_cookie, ROCKETNET_COOKIE_FIELD_BYTES ) );

    uint8_t cookie_pkt[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    uint8_t * p = cookie_pkt;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_COOKIE_PACKET, client.packet_filter );
    rocketnet_write_bytes( &p, client.connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    for ( int i = 0; i < 24; i++ )
        *p++ = (uint8_t) ( 0x31 + i );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_packet( &client, cookie_pkt, sizeof( cookie_pkt ), 100.02,
                                                     payload, sizeof( payload ), &payload_bytes ) == 1 );
    check( memcmp( client.return_cookie, cookie_pkt + 14, 24 ) == 0 );

    // And the next outgoing connection request echoes those exact 24 bytes in its return-cookie field.
    uint8_t req[ROCKETNET_RECEIVE_CEILING];
    int req_bytes = 0;
    rocketnet_client_protocol_update( &client, 100.1, req, &req_bytes );
    check( req_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    check( memcmp( req + ROCKETNET_PREFILTER_BYTES, cookie_pkt + 14, 24 ) == 0 );
}

REGISTER_UNIT_TEST( test_witness_client_stores_and_echoes_cookie_24_bytes );

// 21. t12: Comparisons must be constant time.
// Behavioral regression slice: verifies every single-bit flip across all 16
// bytes of the token identifier is distinguished on the connection-request
// path without early prefix truncation.
// The ledger records the fixed-size sodium_memcmp call as source review; this
// functional case checks full-width equality, not instruction-level timing.
static void test_witness_request_comparisons_reject_every_byte_mismatch()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 8 );
    check( history != NULL );

    struct rocketnet_address_t addr_a;
    struct rocketnet_address_t addr_b;
    check( rocketnet_address_parse( &addr_a, "198.51.100.1:40000" ) );
    check( rocketnet_address_parse( &addr_b, "198.51.100.2:40000" ) );

    uint8_t token_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < ROCKETNET_TOKEN_IDENTIFIER_BYTES; i++ )
        token_id[i] = (uint8_t) ( 0x40 + i );

    rocketnet_used_token_history_record( history, token_id, &addr_a, 2000, 1000 );
    check( rocketnet_used_token_history_check( history, token_id, &addr_a, 1000 ) == 1 );
    check( rocketnet_used_token_history_check( history, token_id, &addr_b, 1000 ) == 0 );

    // Every single-bit flip across all 16 bytes of the token identifier is distinguished.
    for ( int byte_idx = 0; byte_idx < ROCKETNET_TOKEN_IDENTIFIER_BYTES; byte_idx++ )
    {
        for ( int bit = 0; bit < 8; bit++ )
        {
            uint8_t modified[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
            memcpy( modified, token_id, sizeof( modified ) );
            modified[byte_idx] ^= (uint8_t) ( 1u << bit );
            check( sodium_memcmp( modified, token_id, sizeof( token_id ) ) != 0 );
            check( rocketnet_used_token_history_check( history, modified, &addr_b, 1000 ) == 1 );
        }
    }

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_witness_request_comparisons_reject_every_byte_mismatch );

// 22. t12: An implementation must document the ratio it chose.
static void test_witness_server_mapping_ratio_and_verify_budget_decay()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    // witness_server_setup follows the 2x mapping-to-slot ratio (8 mappings for 4 slots).
    check( rocketnet_client_slots_max_clients( srv.server.client_slots ) == 4 );
    for ( int i = 0; i < 8; i++ )
    {
        struct rocketnet_address_t addr;
        memset( &addr, 0, sizeof( addr ) );
        addr.type = ROCKETNET_ADDRESS_IPV4;
        addr.data.ipv4[0] = 192;
        addr.data.ipv4[3] = (uint8_t) ( i + 1 );
        addr.port = (uint16_t) ( 50000 + i );
        check( rocketnet_encryption_map_add( srv.server.encryption_map, &addr, 200.0 ) == i );
    }
    check( rocketnet_encryption_map_full( srv.server.encryption_map ) == 1 );

    // Also verify the signature verify failure budget window ratio (90 failures per 10s window) and decay.
    struct rocketnet_signature_verify_budget_t budget;
    memset( &budget, 0, sizeof( budget ) );
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET; i++ )
    {
        check( rocketnet_signature_verify_budget_allow( &budget, &srv.client_address, 1000 ) == 1 );
        rocketnet_signature_verify_budget_record_failure( &budget, &srv.client_address, 1000 );
    }
    check( rocketnet_signature_verify_budget_allow( &budget, &srv.client_address, 1000 ) == 0 );
    check( rocketnet_signature_verify_budget_allow( &budget, &srv.client_address,
                                                    1000 + ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS - 1 ) == 0 );
    check( rocketnet_signature_verify_budget_allow( &budget, &srv.client_address,
                                                    1000 + ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS ) == 1 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_server_mapping_ratio_and_verify_budget_decay );

// 23. t12: an implementation must document its capacity and eviction policy.
static void test_witness_used_token_history_capacity_and_oldest_eviction()
{
    const int capacity = 8;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history != NULL );

    struct rocketnet_address_t addr_a;
    struct rocketnet_address_t addr_b;
    check( rocketnet_address_parse( &addr_a, "198.51.100.10:40000" ) );
    check( rocketnet_address_parse( &addr_b, "198.51.100.11:40000" ) );

    uint8_t ids[9][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 9; i++ )
    {
        memset( ids[i], (uint8_t) ( i + 1 ), ROCKETNET_TOKEN_IDENTIFIER_BYTES );
        if ( i < capacity )
            rocketnet_used_token_history_record( history, ids[i], &addr_a, 2000, 1000 );
    }

    for ( int i = 0; i < capacity; i++ )
        check( rocketnet_used_token_history_check( history, ids[i], &addr_b, 1000 ) == 0 );

    // Recording a 9th live binding evicts the oldest entry (ids[0]) first.
    rocketnet_used_token_history_record( history, ids[8], &addr_a, 2000, 1000 );
    check( rocketnet_used_token_history_check( history, ids[0], &addr_b, 1000 ) == 1 );
    for ( int i = 1; i <= capacity; i++ )
        check( rocketnet_used_token_history_check( history, ids[i], &addr_b, 1000 ) == 0 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_witness_used_token_history_capacity_and_oldest_eviction );

// 24. t13.1: A server MUST take the claim at face value and MUST NOT wait for
// any other evidence before entering the regime of rule 2.
static void test_witness_server_takes_keep_alive_ack_at_face_value()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7024ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    witness_drive_to_connected( &srv, &client, &time );

    // Before the client acknowledges keep-alive #1, keep_alive_acked is 0 (regime of rule 1).
    check( srv.server.acks->endpoints[0].keep_alive_acked == 0 );

    // Client sends a standalone ack claiming receipt of sequence 1, with no other traffic.
    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int block_bytes = rocketnet_ack_block_build( ack_block, sizeof( ack_block ), &client.accepted_session );
    check( block_bytes > 0 );

    uint8_t ack_pkt[ROCKETNET_RECEIVE_CEILING];
    int ack_bytes = rocketnet_write_sealed_packet( ack_pkt, ROCKETNET_CONNECTION_ACK_PACKET,
                                                   client.packet_filter, client.send_sequence_session++,
                                                   NULL, ack_block, block_bytes,
                                                   ROCKETNET_TEST_PROTOCOL_ID, client.connection_salt,
                                                   NULL, client.keys.session_client_to_server );
    check( ack_bytes > 0 );

    uint8_t resp[ROCKETNET_RECEIVE_CEILING];
    int resp_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client = -1;
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, ack_pkt, ack_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );

    // Server immediately enters the regime of rule 2 without waiting for any other evidence:
    // keep_alive_acked is 1, and an outbound server payload at time + 0.06 suppresses the keep-alive at time + 0.10.
    check( srv.server.acks->endpoints[0].keep_alive_acked == 1 );

    struct rocketnet_address_t to;
    uint64_t seq = 0;
    check( rocketnet_server_protocol_start_payload( &srv.server, 0, &to, &seq ) == 1 );
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE]{};
    check( rocketnet_server_protocol_seal_payload_frame( &srv.server, 0, seq, 1, frame, 4, time + 0.06 ) > 0 );

    uint8_t ka_out[ROCKETNET_RECEIVE_CEILING];
    check( rocketnet_server_protocol_write_keep_alive_if_due( &srv.server, 0, time + 0.10, ka_out ) == 0 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_server_takes_keep_alive_ack_at_face_value );

// 25. t12: Entries expire with the tokens they name.
static void test_witness_used_token_entries_expire_with_tokens()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 8 );
    check( history != NULL );

    struct rocketnet_address_t addr_a;
    struct rocketnet_address_t addr_b;
    check( rocketnet_address_parse( &addr_a, "198.51.100.20:40000" ) );
    check( rocketnet_address_parse( &addr_b, "198.51.100.21:40000" ) );

    uint8_t token_id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    memset( token_id, 0x55, sizeof( token_id ) );

    const uint64_t expire_ts = 5000;
    rocketnet_used_token_history_record( history, token_id, &addr_a, expire_ts, 4900 );

    check( rocketnet_used_token_history_check( history, token_id, &addr_a, 4999 ) == 1 );
    check( rocketnet_used_token_history_check( history, token_id, &addr_b, 4999 ) == 0 );

    // At current_timestamp == expire_ts, the entry expires with the token it names.
    check( rocketnet_used_token_history_check( history, token_id, &addr_b, expire_ts ) == 1 );
    check( rocketnet_used_token_history_check( history, token_id, &addr_b, expire_ts + 10 ) == 1 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_witness_used_token_entries_expire_with_tokens );

// 26. t13: The converse does not hold, and must not be assumed.
static void test_witness_keep_alive_suppression_gap_converse_does_not_hold()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7026ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    witness_drive_to_connected( &srv, &client, &time );

    // Acknowledge keep-alive #1 so the server enters regime 2 (interval = 0.10s).
    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int block_bytes = rocketnet_ack_block_build( ack_block, sizeof( ack_block ), &client.accepted_session );
    uint8_t ack_pkt[ROCKETNET_RECEIVE_CEILING];
    int ack_bytes = rocketnet_write_sealed_packet( ack_pkt, ROCKETNET_CONNECTION_ACK_PACKET,
                                                   client.packet_filter, client.send_sequence_session++,
                                                   NULL, ack_block, block_bytes,
                                                   ROCKETNET_TEST_PROTOCOL_ID, client.connection_salt,
                                                   NULL, client.keys.session_client_to_server );
    uint8_t resp[ROCKETNET_RECEIVE_CEILING];
    int resp_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client = -1;
    rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, ack_pkt, ack_bytes,
                                              ROCKETNET_TEST_CREATE_TS, time, resp, &resp_bytes,
                                              payload, &payload_bytes, &payload_client );
    check( srv.server.acks->endpoints[0].keep_alive_acked == 1 );

    // Forward direction: gaps strictly shorter than 0.10s suppress keep-alives completely.
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE]{};
    uint8_t ka_out[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < 3; i++ )
    {
        time += 0.08;
        struct rocketnet_address_t to;
        uint64_t seq = 0;
        check( rocketnet_server_protocol_start_payload( &srv.server, 0, &to, &seq ) == 1 );
        check( rocketnet_server_protocol_seal_payload_frame( &srv.server, 0, seq, (uint64_t) ( i + 1 ), frame, 4, time ) > 0 );
        check( rocketnet_server_protocol_write_keep_alive_if_due( &srv.server, 0, time, ka_out ) == 0 );
    }

    // Converse does not hold: a gap of 0.15s (> 0.10s interval) between outbound sends
    // where the discrete update check runs at the end of the gap after the next send
    // emits NO keep-alive in that gap.
    const double after_gap = time + 0.15;
    struct rocketnet_address_t to;
    uint64_t seq = 0;
    check( rocketnet_server_protocol_start_payload( &srv.server, 0, &to, &seq ) == 1 );
    check( rocketnet_server_protocol_seal_payload_frame( &srv.server, 0, seq, 99, frame, 4, after_gap ) > 0 );
    check( rocketnet_server_protocol_write_keep_alive_if_due( &srv.server, 0, after_gap, ka_out ) == 0 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_keep_alive_suppression_gap_converse_does_not_hold );

// 27. t13: Any change to the ack block's encoding or validity rules, to the
// keep-alive's type data or size, or to the order in which the replay window is
// marked relative to those checks, must preserve it.
static void test_witness_authentic_keep_alive_passes_post_open_checks()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7027ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    witness_drive_to_sending_response( &srv, &client, &time );

    // Send the response to the server so it assigns slot 0 and emits the establishment keep-alive.
    uint8_t pkt[ROCKETNET_RECEIVE_CEILING];
    int pkt_bytes = 0;
    rocketnet_client_protocol_update( &client, time + 0.1, pkt, &pkt_bytes );
    check( pkt_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

    uint8_t ka_pkt[ROCKETNET_RECEIVE_CEILING];
    int ka_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client = -1;
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, time + 0.1, ka_pkt, &ka_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );
    check( ka_bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    // The authentic keep-alive opens, marks replay_session, passes ack-block and type-data checks,
    // marks accepted_session, and transitions the client to CONNECTED.
    check( rocketnet_client_protocol_process_packet( &client, ka_pkt, ka_bytes, time + 0.1,
                                                     payload, sizeof( payload ), &payload_bytes ) == 1 );
    check( client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK] == 0 );
    check( client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE] == 0 );
    check( client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED] == 1 );
    check( client.replay_session.highest_opened == 1 );
    check( client.accepted_session.highest_opened == 1 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_authentic_keep_alive_passes_post_open_checks );

// 28. t13: confirmation test must then be re-derived rather than assumed to
// still hold.
static void test_witness_rejected_keep_alive_never_trips_server_confirmation_test()
{
    struct witness_server_t srv;
    witness_server_setup( &srv );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    witness_mint_token( token, 0x7028ULL, ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_EXPIRE_TS, 10 );
    double time = 100.0;
    check( rocketnet_client_protocol_connect( &client, token, time, ROCKETNET_TEST_CREATE_TS ) );
    witness_drive_to_sending_response( &srv, &client, &time );

    // Establish the slot on the server, dropping the server's keep-alive #1 on the wire.
    uint8_t pkt[ROCKETNET_RECEIVE_CEILING];
    int pkt_bytes = 0;
    rocketnet_client_protocol_update( &client, time + 0.1, pkt, &pkt_bytes );
    uint8_t dropped_ka[ROCKETNET_RECEIVE_CEILING];
    int dropped_ka_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client = -1;
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, pkt, pkt_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, time + 0.1, dropped_ka, &dropped_ka_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE );

    // Craft a keep-alive at sequence 1 sealed under session_server_to_client whose post-open
    // type-data check fails (client_index = 4 == max_clients).
    uint8_t bad_ka_plain[ROCKETNET_ACK_BLOCK_MIN_BYTES + 8];
    memset( bad_ka_plain, 0, sizeof( bad_ka_plain ) );
    uint8_t * tp = bad_ka_plain + ROCKETNET_ACK_BLOCK_MIN_BYTES;
    rocketnet_write_uint32( &tp, 4 );
    rocketnet_write_uint32( &tp, 4 );

    uint8_t bad_ka_wire[ROCKETNET_RECEIVE_CEILING];
    int bad_ka_bytes = rocketnet_write_sealed_packet( bad_ka_wire, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                      client.packet_filter, 1, NULL,
                                                      bad_ka_plain, sizeof( bad_ka_plain ),
                                                      ROCKETNET_TEST_PROTOCOL_ID, client.connection_salt,
                                                      NULL, client.keys.session_server_to_client );
    check( bad_ka_bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    // Client opens sequence 1 and marks replay_session, but refuses it at the post-open check:
    // client remains in SENDING_CONNECTION_RESPONSE and accepted_session does NOT advance.
    check( rocketnet_client_protocol_process_packet( &client, bad_ka_wire, bad_ka_bytes, time + 0.12,
                                                     payload, sizeof( payload ), &payload_bytes ) == 0 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );
    check( client.replay_session.highest_opened == 1 );
    check( client.accepted_session.highest_opened == 0 );

    // If the client sends a standalone ack from SENDING_CONNECTION_RESPONSE built from accepted_session,
    // its block does NOT name sequence 1, so the server's confirmation test (keep_alive_acked) stays 0!
    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int block_bytes = rocketnet_ack_block_build( ack_block, sizeof( ack_block ), &client.accepted_session );
    uint8_t ack_pkt[ROCKETNET_RECEIVE_CEILING];
    int ack_bytes = rocketnet_write_sealed_packet( ack_pkt, ROCKETNET_CONNECTION_ACK_PACKET,
                                                   client.packet_filter, client.send_sequence_session++,
                                                   NULL, ack_block, block_bytes,
                                                   ROCKETNET_TEST_PROTOCOL_ID, client.connection_salt,
                                                   NULL, client.keys.session_client_to_server );
    uint8_t resp[ROCKETNET_RECEIVE_CEILING];
    int resp_bytes = 0;
    check( rocketnet_server_protocol_process_packet( &srv.server, &srv.client_address, ack_pkt, ack_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, time + 0.15, resp, &resp_bytes,
                                                     payload, &payload_bytes, &payload_client ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    check( srv.server.acks->endpoints[0].keep_alive_acked == 0 );

    witness_server_teardown( &srv );
}

REGISTER_UNIT_TEST( test_witness_rejected_keep_alive_never_trips_server_confirmation_test );
