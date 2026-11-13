/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// What the client transport tells its caller at create and in update
// (sdk/rocketnet_client_transport.cpp): the memory plan and socket lines it
// logs through the caller's log function, and the state changes an update
// reports through the caller's state_change_callback. Each client is
// step-driven over the fake client transport (tests/fake_client_driver.h):
// no socket, no thread; time is an argument starting at 1000.0. Each
// failing check prints the fault first.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

extern void rocketnet_client_transport_seed_refusal_state_for_test( rocketnet_client_transport_t *, uint64_t, const uint8_t *, const uint8_t *, const uint8_t * );
extern void rocketnet_client_transport_thread_stack_fault_for_test( bool fault );

// ---------------------------------------------------------------------------
// a step-driven client over the fake driver

static double r2_create_now = 1000.0;
static double r2_create_time( void * ) { return r2_create_now; }

static void r2_create_token( uint8_t * token, bool ipv6 )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 5;
    if ( ipv6 )
    {
        // [2001:db8::1]:40000
        const uint8_t ipv6_address[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV6;
        memcpy( signed_fields.ipv6_address, ipv6_address, 18 );
    }
    else
    {
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
        signed_fields.ipv4_address[0] = 127;
        signed_fields.ipv4_address[3] = 1;
        signed_fields.ipv4_address[4] = 0x40;
        signed_fields.ipv4_address[5] = 0x9c;
    }
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static rocketnet_client_transport_t * r2_create_client( rocketnet_fake_client_transport_io_t * fake, bool ipv6, struct rocketnet_client_transport_config_t * config )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_create_token( token, ipv6 );

    config->time_callback = r2_create_time;
    r2_create_now = 1000.0;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( NULL, token, config, 1000.0, &rocketnet_fake_client_operations_io, fake );
    check( transport != NULL );
    return transport;
}

// ---------------------------------------------------------------------------
// the log lines the create writes

#define R2_LOG_LINES 64
#define R2_LOG_LINE_BYTES 256

static char r2_log[R2_LOG_LINES][R2_LOG_LINE_BYTES];
static int r2_log_count = 0;

static void r2_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    (void) length;
    if ( r2_log_count < R2_LOG_LINES )
    {
        snprintf( r2_log[r2_log_count], R2_LOG_LINE_BYTES, "%s", message );
        r2_log_count++;
    }
}

static void r2_log_begin()
{
    r2_log_count = 0;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( r2_log_capture, NULL );
}

static void r2_log_end()
{
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
}

static int r2_log_lines_with( const char * text )
{
    int lines = 0;
    for ( int i = 0; i < r2_log_count; i++ )
    {
        if ( strstr( r2_log[i], text ) != NULL )
        {
            lines++;
        }
    }
    return lines;
}

static void r2_log_print( const char * id, const char * text )
{
    for ( int i = 0; i < r2_log_count; i++ )
    {
        if ( strstr( r2_log[i], text ) != NULL )
        {
            printf( "mutation gap %s: logged \"%s\"\n", id, r2_log[i] );
        }
    }
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6801, 2/6803 (the portable plan's socket count)
//
// The create logs the memory plan of every candidate it weighs. A portable
// client opens one socket per address family, so its plan says 2 sockets.

static void test_gaps_r2_ct_portable_memory_plan_logs_two_sockets()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );

    r2_log_begin();
    rocketnet_client_transport_t * transport = r2_create_client( fake, false, &config );
    r2_log_end();

    const int plans = r2_log_lines_with( "client memory plan [portable]:" );
    const int two_sockets = r2_log_lines_with( "allocator bytes, 2 sockets;" );
    if ( plans != 1 || two_sockets != 1 )
    {
        printf( "mutation gap hunt 2/6801,6803: the portable client's memory plan was logged %d time(s), %d of them with 2 sockets (one socket per family):\n", plans, two_sockets );
        r2_log_print( "hunt 2/6801,6803", "client memory plan" );
    }
    check( plans == 1 );
    check( two_sockets == 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_portable_memory_plan_logs_two_sockets );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7056 (the IPv6 bind says whether the socket is dual stack)
// mutation gap: hunt 2/7064 (no preferred port line where the platform has none)
//
// The suite answers "IPv6 available" (tests/test.cpp), so a client of an
// IPv6 server binds IPv6 and says which IPv6 socket it is: dual stack where
// the platform's client sockets are, IPv6 only where they are not. No
// platform the suite builds for has a preferred client port, so no create
// says it is using one.

static void test_gaps_r2_ct_ipv6_bind_logs_its_stack_and_no_preferred_port()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );

    r2_log_begin();
    rocketnet_client_transport_t * transport = r2_create_client( fake, true, &config );
    r2_log_end();

    const bool dual_stack = rocketnet_platform_client_dual_stack();
    const int said_dual = r2_log_lines_with( "client socket is dual stack ipv4 and ipv6" );
    const int said_ipv6_only = r2_log_lines_with( "client socket is ipv6 only" );
    if ( said_dual != ( dual_stack ? 1 : 0 ) || said_ipv6_only != ( dual_stack ? 0 : 1 ) )
    {
        printf( "mutation gap hunt 2/7056: an IPv6 bind on a platform whose client sockets are %s logged \"dual stack\" %d time(s) and \"ipv6 only\" %d time(s)\n",
                dual_stack ? "dual stack" : "IPv6 only", said_dual, said_ipv6_only );
    }
    check( said_dual == ( dual_stack ? 1 : 0 ) );
    check( said_ipv6_only == ( dual_stack ? 0 : 1 ) );

    if ( rocketnet_platform_preferred_client_port() == 0 )
    {
        const int preferred = r2_log_lines_with( "client socket using preferred port" );
        if ( preferred != 0 )
        {
            printf( "mutation gap hunt 2/7064: a platform with no preferred client port logged that the socket uses one:\n" );
            r2_log_print( "hunt 2/7064", "preferred port" );
        }
        check( preferred == 0 );
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_ipv6_bind_logs_its_stack_and_no_preferred_port );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/7597 (an update's state change reaches the caller's callback)
//
// A connected client hears nothing for longer than the token's 5 second
// timeout: the update that notices ends the connection, and the caller's
// state_change_callback hears that change once, from CONNECTED to the state
// the client is now in.

struct r2_state_changes_t
{
    int calls;
    int previous;
    int current;
};

static r2_state_changes_t r2_state_changes;

static void r2_state_change( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state )
{
    (void) context;
    r2_state_changes.calls++;
    r2_state_changes.previous = (int) previous_state;
    r2_state_changes.current = (int) current_state;
}

static void test_gaps_r2_ct_update_reports_a_state_change_to_the_callback()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.state_change_callback = r2_state_change;

    rocketnet_client_transport_t * transport = r2_create_client( fake, false, &config );
    rocketnet_client_transport_seed_refusal_state_for_test( transport, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_test_filter_value,
                                                            rocketnet_test_salt, rocketnet_test_session_s2c_key );
    check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    memset( &r2_state_changes, 0, sizeof( r2_state_changes ) );

    r2_create_now = 1010.0;
    rocketnet_client_transport_update( transport, r2_create_now );

    const int state = rocketnet_client_transport_state( transport );
    check( state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const bool heard = r2_state_changes.calls == 1 && r2_state_changes.previous == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                       r2_state_changes.current == state;
    if ( !heard )
    {
        printf( "mutation gap hunt 2/7597: the update moved the client from CONNECTED (%d) to %d, and the state_change_callback heard %d call(s) (last %d to %d)\n",
                (int) ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED, state, r2_state_changes.calls, r2_state_changes.previous, r2_state_changes.current );
    }
    check( heard );

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_update_reports_a_state_change_to_the_callback );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6822 (a threaded plan with no thread stack size is refused)
//
// A threaded client's plan needs the platform's thread stack size; where the
// platform cannot report it (pthread_attr_init or pthread_attr_getstacksize
// failing) the plan is refused and the required memory reads 0. The seam
// stands in that failure.

static void test_gaps_r2_ct_threaded_plan_without_a_stack_size_is_refused()
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    check( config.step_driven == 0 );

    const uint64_t planned = rocketnet_client_transport_required_memory( &config, "portable" );
    check( planned > 0 );

    rocketnet_client_transport_thread_stack_fault_for_test( true );
    const uint64_t faulted = rocketnet_client_transport_required_memory( &config, "portable" );
    rocketnet_client_transport_thread_stack_fault_for_test( false );

    if ( faulted != 0 )
    {
        printf( "mutation gap hunt 2/6822: a threaded portable plan whose thread stack size reads 0 was granted: %llu bytes required, not 0 (refused)\n",
                (unsigned long long) faulted );
    }
    check( faulted == 0 );
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_threaded_plan_without_a_stack_size_is_refused );

#endif // #if ROCKETNET_DEV
