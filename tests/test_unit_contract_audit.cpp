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
// allocator and a dev token with no address are API misuse; the default
// num_channels fits the build; and the reassembly refusal names its fixed
// limit. Everything runs in one process over the in-memory loopback transport
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

#if ROCKETNET_DEV

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

// C7: the reassembly refusal names the fixed reassembly limit it checks, not
// the operator memory budget, which that check never reads

static void test_contract_reassembly_refusal_names_its_limit()
{
    contract_capture_t capture;
    contract_capture_begin( &capture );

    // a title's own packet filter: a production build refuses the test one first
    static const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x43, 0x37, 0x52, 0x4D };

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    memcpy( config.packet_filter, packet_filter, sizeof( config.packet_filter ) );
    config.max_clients = ROCKETNET_MAX_CLIENTS;
    config.max_fragmented_messages = 256;
    int status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED );
    check( strstr( contract_last_error, "fixed reassembly limit" ) != NULL );
    check( strstr( contract_last_error, "operator memory budget" ) == NULL );

    contract_capture_end( &capture );
}

REGISTER_UNIT_TEST( test_contract_reassembly_refusal_names_its_limit );

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
