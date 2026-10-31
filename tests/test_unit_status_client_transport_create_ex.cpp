/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The _ex client transport create (docs/ERRORS-AND-LOGGING.md 2.3, Layer 4):
// every failure exit reachable without a socket or real time writes its
// section-2.1 code to the status out-parameter, and the non-_ex form is a thin
// wrapper passing NULL.
//
// Reachable in the unit tier:
//   * C1 (NULL connect_token) -> INVALID_ARGUMENT
//   * the pre-init refusal -> NOT_INITIALIZED
//   * C2 (config env apply failure) -> CONFIG_ENV_INVALID
//   * C3 (config validation failure) -> CONFIG_OUT_OF_RANGE
//   * the published test packet filter in a production build ->
//     TEST_PACKET_FILTER_IN_PRODUCTION
//   * C5 (transport ladder parse failure) -> INVALID_TRANSPORT_LIST
//   * C6 (transport block malloc refusal) -> OUT_OF_MEMORY
//   * C7 (fragmentation storage malloc refusal) -> OUT_OF_MEMORY
//   * C8 (invalid connect token) -> INVALID_CONNECT_TOKEN
//   * C9 (expired connect token) -> CONNECT_TOKEN_EXPIRED
//   * a token sealed to the development server key in a production build ->
//     DEV_CONNECT_TOKEN_IN_PRODUCTION
//
// The case is registered in both the development and the production
// (ROCKETNET_DEV=0) unit suites. C2 sets an environment variable through the
// development test environment table, so it runs in the development suite;
// the two production refusals run in the production suite. Every other step
// runs in both, over a config carrying its own packet filter.
//
// Not reachable without a socket or real time:
//   * C4 (reassembly memory budget), whose maximum valid geometry (256 slots x
//     the 256-fragment payload ceiling) is far under the 512 MiB component
//     budget;
//   * C10 (transport ladder exhausted), which needs a socket open to refuse;
//   * C11 (mutex/thread create failure), which needs a platform primitive to
//     fail.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_config.h"

#if ROCKETNET_DEV
#include "test_env_source.h"
#endif // #if ROCKETNET_DEV

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

struct allocator_state
{
    int live;
    int calls;
    int fail_at;
};

static void * allocate( void * context, size_t bytes )
{
    allocator_state * state = (allocator_state *) context;
    const int call = ++state->calls;
    if ( state->fail_at == call )
        return NULL;

    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;
    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memset( (void *) address, 0xA5, bytes );
    ++state->live;

    return (void *) address;
}

static void release( void * context, void * pointer )
{
    allocator_state * state = (allocator_state *) context;
    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    --state->live;
    free( base );
}

static void check_create_status( int expected, const uint8_t * token, const struct rocketnet_client_transport_config_t * config,
                                 allocator_state * state, int * status )
{
    *status = -1;
    check( rocketnet_client_transport_create_ex( state, token, config, 1000.0, status ) == NULL );
    check( *status == expected );
    check( state->live == 0 );
}

// the default config with a packet filter of the caller's own: a production
// build refuses the published test filter the default config carries
static void own_filter_config( struct rocketnet_client_transport_config_t * config )
{
    rocketnet_client_transport_default_config( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
}

#if ROCKETNET_ASSERTS
// the NULL argument below is the documented counted no-op, which fires
// rocketnet_assert in a build with asserts on: count it instead of breaking
static int client_create_ex_expected_null_asserts = 0;

static void client_create_ex_expected_null_assert( const char *, const char *, const char *, int )
{
    client_create_ex_expected_null_asserts++;
}
#endif // #if ROCKETNET_ASSERTS

static void test_client_transport_create_ex_reports_every_status()
{
    allocator_state state = { 0, 0, 0 };
    int status = -1;

    // C1: a NULL connect_token even INVALID_ARGUMENT, counted
#if ROCKETNET_ASSERTS
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    client_create_ex_expected_null_asserts = 0;
    rocketnet_assert_function( client_create_ex_expected_null_assert );
#endif // #if ROCKETNET_ASSERTS
    check( rocketnet_client_transport_create_ex( &state, NULL, NULL, 1000.0, &status ) == NULL );
#if ROCKETNET_ASSERTS
    rocketnet_assert_function( saved_assert );

    check( client_create_ex_expected_null_asserts == 1 );
#endif // #if ROCKETNET_ASSERTS
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );

    // the worker starts initialized; terminate so the fault-injecting
    // allocator can be installed and the pre-init refusal is reachable. the
    // token is non-NULL so the refusal is the pre-init check, not C1.

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    check_create_status( ROCKETNET_STATUS_NOT_INITIALIZED, token, NULL, &state, &status );

    check( rocketnet_init_ex( &state, allocate, release, &status ) );
    check( status == ROCKETNET_STATUS_OK );

#if ROCKETNET_DEV
    // C2: an invalid environment overlay refuses at the env apply, read from
    // a table this case owns
    {
        rocketnet_test_env_t env;
        env.set( "ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES", "not-a-number" );
        status = -1;
        check( rocketnet_client_transport_create_current_with_env( &state, token, NULL, 1000.0, &env.source, &status ) == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
        check( state.live == 0 );
    }

#endif // #if ROCKETNET_DEV

    struct rocketnet_client_transport_config_t production;
    own_filter_config( &production );

    // C3: a config field out of range refuses at validation
    {
        struct rocketnet_client_transport_config_t config;
        own_filter_config( &config );
        config.max_fragmented_messages = 2000000000;
        check_create_status( ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, token, &config, &state, &status );
    }

#if !ROCKETNET_DEV
    // the default config carries the published test packet filter, which a
    // production build refuses
    check_create_status( ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION, token, NULL, &state, &status );
#endif // #if !ROCKETNET_DEV

    // C5: an unparseable transport candidate list refuses at the ladder parse
    {
        struct rocketnet_client_transport_config_t config;
        own_filter_config( &config );
        snprintf( config.transport, sizeof( config.transport ), "%s", "no-such-transport" );
        check_create_status( ROCKETNET_STATUS_INVALID_TRANSPORT_LIST, token, &config, &state, &status );
    }

    // C6: the transport block malloc refuses

    state.fail_at = state.calls + 1;
    check_create_status( ROCKETNET_STATUS_OUT_OF_MEMORY, token, &production, &state, &status );
    state.fail_at = 0;

    // C7: the send arena malloc refuses (the second allocation)

    state.fail_at = state.calls + 2;
    check_create_status( ROCKETNET_STATUS_OUT_OF_MEMORY, token, &production, &state, &status );
    state.fail_at = 0;

    // C7: the reassembly set malloc refuses (the third allocation)

    state.fail_at = state.calls + 3;
    check_create_status( ROCKETNET_STATUS_OUT_OF_MEMORY, token, &production, &state, &status );
    state.fail_at = 0;

    // a valid token for the connect-refusal cases. the client reads the
    // signed fields directly at connect (no signature check until the server),
    // so the signed data is enough to reach C8 and C9.

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );

    {
        uint8_t good[ROCKETNET_CONNECT_TOKEN_BYTES];
        memset( good, 0, sizeof( good ) );
        rocketnet_write_signed_connect_data( &signed_fields, good );
        memcpy( token, good, sizeof( good ) );
    }

    // C8: address_flags zero is an invalid connect token
    {
        signed_fields.address_flags = 0;

        uint8_t bad[ROCKETNET_CONNECT_TOKEN_BYTES];
        memset( bad, 0, sizeof( bad ) );
        rocketnet_write_signed_connect_data( &signed_fields, bad );
        check_create_status( ROCKETNET_STATUS_INVALID_CONNECT_TOKEN, bad, &production, &state, &status );
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    }

    // C9: an expired token (create < expire <= current timestamp, so the token
    // passes the pre-checks and hits the expiry branch)
    {
        signed_fields.create_timestamp = 1;
        signed_fields.expire_timestamp = 2;

        uint8_t expired[ROCKETNET_CONNECT_TOKEN_BYTES];
        memset( expired, 0, sizeof( expired ) );
        rocketnet_write_signed_connect_data( &signed_fields, expired );
        check_create_status( ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED, expired, &production, &state, &status );
        signed_fields.create_timestamp = 1;
        signed_fields.expire_timestamp = 1ULL << 40;
    }

#if !ROCKETNET_DEV
    // a valid token sealed to the published development server key is
    // refused in a production build, before any socket opens
    check_create_status( ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION, token, &production, &state, &status );
#endif // #if !ROCKETNET_DEV

    // a clean term, then restore the default allocator for the worker's term

    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_client_transport_create_ex_reports_every_status );
