/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_platform.h"
#include "rocketnet_allocator.h"
#include "portable_crt.h"
#include "suite_runner.h"
#include "test_env_source.h"
#include "test_unit_registry.h"
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#if defined( _WIN32 )
#include <fcntl.h>
#include <io.h>
#endif // #if defined( _WIN32 )
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

int rocketnet_unit_test_count();
const char * rocketnet_unit_test_name( int index );
void rocketnet_unit_prepare();
int rocketnet_unit_run_named( const char * name );

// the message layer's tests, in test_channels.cpp

int rocketnet_channels_test_count();
const char * rocketnet_channels_test_name( int index );
const char * rocketnet_channels_test_resolve( const char * name );
int rocketnet_channels_run_named( const char * name );
void rocketnet_channels_prepare( const char * name, void * ( **malloc_function )( void * context, size_t bytes ), void ( **free_function )( void * context, void * p ) );
void rocketnet_channels_prepare_log( const char * name );

static int rocketnet_suite_test_count()
{
    return rocketnet_unit_test_count() + rocketnet_channels_test_count() + rocketnet_unit_tests_count();
}

static const char * rocketnet_suite_test_name( int index )
{
    const int units = rocketnet_unit_test_count();
    if ( index < units )
        return rocketnet_unit_test_name( index );

    const int channels = rocketnet_channels_test_count();
    const int cindex = index - units;
    if ( cindex < channels )
        return rocketnet_channels_test_name( cindex );

    return rocketnet_unit_tests_name( cindex - channels );
}

// the DEV unit suite opens no socket: the platform's network lookup reads this
// instead of the IPv6 probe socket
// (rocketnet_platform_set_network_lookups_for_testing, rocketnet_internal.h).
// true is the probe's answer on a host with an IPv6 stack.

#if ROCKETNET_DEV
static bool rocketnet_unit_fake_ipv6_available()
{
    return true;
}

static const rocketnet_platform_network_lookups_t rocketnet_unit_fake_network_lookups = {
    rocketnet_unit_fake_ipv6_available,
};

// The unit tier starts no OS network stack: the platform start (WSAStartup on
// Windows) is answered by this fake, and every other platform call in the
// table stays real. Cases that inject their own refusal copy the current table
// and restore it, so a case's own init cycles stay on this fake too.

static int rocketnet_unit_fake_platform_start( void )
{
    return 0;
}

static rocketnet_platform_priority_calls_t rocketnet_unit_platform_calls;

#endif // #if ROCKETNET_DEV

// Derive only SDK configuration names from the same descriptor tables that
// the SDK reads. Test-runner and diagnostic variables remain untouched.

static bool rocketnet_unit_append_config_names( std::vector<std::string> & names, const char * prefix,
                                                const rocketnet_config_field_t * fields, int count )
{
    for ( int i = 0; i < count; ++i )
    {
        if ( ( fields[i].flags & ROCKETNET_CONFIG_NO_ENV ) != 0 )
            continue;

        char name[128];
        if ( !rocketnet_config_env_var_name_for( prefix, fields[i].name, name, sizeof( name ) ) )
            return false;
        names.push_back( name );
    }

    return true;
}

static int rocketnet_unit_clear_environment()
{
    std::vector<std::string> names;
    int count = 0;
    const rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &count );
    if ( !rocketnet_unit_append_config_names( names, "client", fields, count ) )
        return 1;
    fields = rocketnet_server_transport_config_fields( &count );
    if ( !rocketnet_unit_append_config_names( names, "server", fields, count ) )
        return 1;
    fields = rocketnet_message_config_fields( &count );
    if ( !rocketnet_unit_append_config_names( names, "client", fields, count ) ||
         !rocketnet_unit_append_config_names( names, "server", fields, count ) )
        return 1;
#if ROCKETNET_DEV
    fields = rocketnet_client_budget_config_fields( &count );
    if ( !rocketnet_unit_append_config_names( names, "client", fields, count ) )
        return 1;
    fields = rocketnet_server_budget_config_fields( &count );
    if ( !rocketnet_unit_append_config_names( names, "server", fields, count ) )
        return 1;
#else // #if ROCKETNET_DEV
    // the send budget fields: the accessors are ROCKETNET_DEV code
    names.push_back( "ROCKETNET_CLIENT_SEND_KBPS" );
    names.push_back( "ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS" );
#endif // #if ROCKETNET_DEV

    // these reads are outside the descriptor engine: startup logging, served
    // addresses, operator memory budgets and the native XDP gateway address

    const char * const explicit_names[] = {
        "ROCKETNET_LOG_LEVEL",
        "ROCKETNET_SERVER_IPV4_ADDRESS",
        "ROCKETNET_SERVER_IPV6_ADDRESS",
        "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES",
        "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES",
        "ROCKETNET_GATEWAY_ETHERNET_ADDRESS",
#if ROCKETNET_DEV
        "ROCKETNET_DEV_SESSION_SEQUENCE_START",
        "ROCKETNET_DEV_CLIENT_LATENCY_MS",
        "ROCKETNET_DEV_CLIENT_JITTER_MS",
        "ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT",
        "ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT",
        "ROCKETNET_DEV_SERVER_LATENCY_MS",
        "ROCKETNET_DEV_SERVER_JITTER_MS",
        "ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT",
        "ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT",
#endif // #if ROCKETNET_DEV
    };
    for ( size_t i = 0; i < sizeof( explicit_names ) / sizeof( explicit_names[0] ); ++i )
    {
        names.push_back( explicit_names[i] );
    }
    std::sort( names.begin(), names.end() );
    names.erase( std::unique( names.begin(), names.end() ), names.end() );

    // names only: no configuration value is read, copied or logged

    for ( size_t i = 0; i < names.size(); ++i )
    {
#if defined( _WIN32 )
        if ( _putenv_s( names[i].c_str(), "" ) != 0 )
#else
        if ( unsetenv( names[i].c_str() ) != 0 )
#endif // #if defined( _WIN32 )
            return 1;
    }

    return 0;
}

static int rocketnet_unit_worker( const char * name )
{
#if defined( _WIN32 )
    // MSVC treats _IOLBF as full buffering and rejects a zero buffer size.
    // A full buffer flushes when it fills, wherever that falls in a line, so
    // it is sized above any one worker's output (the ed25519 witness alone
    // prints about 6 KB): the worker's lines reach the shared pipe in the one
    // write of the fflush below, and parallel workers never tear a line. A
    // text-mode stream may split a flush into several writes as it converts
    // newlines, so stdout is binary: a flush is then a single write.

    if ( _setmode( _fileno( stdout ), _O_BINARY ) == -1 ||
         setvbuf( stdout, NULL, _IOFBF, 1 << 20 ) != 0 ||
         setvbuf( stderr, NULL, _IONBF, 0 ) != 0 )
        return 1;
#else
    setvbuf( stdout, NULL, _IOLBF, 0 );
    setvbuf( stderr, NULL, _IOLBF, 0 );
#endif // #if defined( _WIN32 )

    // DEV0 runs ordinary production startup discovery here. That lifecycle
    // setup is functional evidence, rather than OS-free unit evidence.

    if ( rocketnet_unit_clear_environment() != 0 )
        return 1;

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    // allocator cases pass their hooks to init

    void * ( *malloc_function )( void * context, size_t bytes ) = NULL;
    void ( *free_function )( void * context, void * p ) = NULL;
    rocketnet_channels_prepare( name, &malloc_function, &free_function );

#if ROCKETNET_DEV
    rocketnet_platform_set_network_lookups_for_testing( &rocketnet_unit_fake_network_lookups );
    rocketnet_unit_platform_calls = *rocketnet_platform_priority_calls_real();
    rocketnet_unit_platform_calls.platform_start = rocketnet_unit_fake_platform_start;
    rocketnet_platform_set_priority_calls_for_testing( &rocketnet_unit_platform_calls );
#endif // #if ROCKETNET_DEV

    if ( rocketnet_init( NULL, malloc_function, free_function ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    rocketnet_unit_prepare();

    // the suite's config isolation clears every transport variable above. Two
    // cases create a client transport through the public API before stepping
    // it themselves; run those step-driven, after the isolation, so the unit
    // suite starts no client I/O thread.

    const bool step_driven_case = strcmp( name, "test_token_time_game_clock_admits_a_fixed_token" ) == 0 ||
                                  strcmp( name, "test_public_link_simulator_revision_1_prefixes" ) == 0;
    if ( step_driven_case )
    {
        if ( rocketnet_test_env_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", "1", 1 ) != 0 )
            return 1;
    }

    rocketnet_channels_prepare_log( name );

    int result = rocketnet_channels_run_named( name );
    if ( result == 2 )
    {
        result = rocketnet_unit_tests_run_named( name );
    }
    if ( result == 2 )
    {
        result = rocketnet_unit_run_named( name );
    }
    if ( step_driven_case && rocketnet_test_env_unsetenv( "ROCKETNET_CLIENT_STEP_DRIVEN" ) != 0 )
    {
        result = 1;
    }
    if ( !rocketnet_test_loopback_hub_release() )
    {
        result = 1;
    }
    rocketnet_term();
    fflush( stdout );

    return result;
}

#if defined( _MSC_VER )
#define ROCKETNET_TEST_FP_NOINLINE __declspec(noinline)
#elif defined( __GNUC__ ) || defined( __clang__ )
#define ROCKETNET_TEST_FP_NOINLINE __attribute__((noinline))
#else
#define ROCKETNET_TEST_FP_NOINLINE
#endif // #if defined( _MSC_VER )

// Canary: the estate's strict floating-point floor is -ffp-contract=off
// (GCC/Clang) or /fp:precise (MSVC). Compiled with contraction enabled, a*b+c
// here rounds once into an FMA and changes the result; under the floor it takes
// two roundings. The inputs ride through volatile so nothing folds at compile
// time, and the arithmetics live in out-of-line helpers so the backend is free
// to contract (and the probe is about the setting, not common-subexpression
// folding). One rounding yields 0x33800000 (float) / 0x3C90000000000000
// (double); the floor yields 0x00000000 / 0x0000000000000000.

ROCKETNET_TEST_FP_NOINLINE static int rocketnet_fp_check_float( float a, float b, float c )
{
    const float r = a * b + c;
    uint32_t bits;
    memcpy( &bits, &r, sizeof( bits ) );

    if ( bits != UINT32_C( 0x00000000 ) )
    {
        printf( "FAIL test_fp_contract_off: float a*b+c bits 0x%08X, expected 0x00000000\n", (unsigned) bits );
        return 1;
    }

    return 0;
}

ROCKETNET_TEST_FP_NOINLINE static int rocketnet_fp_check_double( double a, double b, double c )
{
    const double r = a * b + c;
    uint64_t bits;
    memcpy( &bits, &r, sizeof( bits ) );

    if ( bits != UINT64_C( 0x0000000000000000 ) )
    {
        printf( "FAIL test_fp_contract_off: double a*b+c bits 0x%016llX, expected 0x0000000000000000\n", (unsigned long long) bits );
        return 1;
    }

    return 0;
}

static int test_fp_contract_off()
{
    const uint32_t ab_bits = UINT32_C( 0x3F800800 );
    const uint32_t c_bits = UINT32_C( 0xBF801000 );
    volatile float av, bv, cv;
    float a, b, c;
    memcpy( &a, &ab_bits, sizeof( a ) );
    memcpy( &b, &ab_bits, sizeof( b ) );
    memcpy( &c, &c_bits, sizeof( c ) );
    av = a;
    bv = b;
    cv = c;

    const uint64_t dab_bits = UINT64_C( 0x3FF0000002000000 );
    const uint64_t dc_bits = UINT64_C( 0xBFF0000004000000 );
    volatile double dav, dbv, dcv;
    double da, db, dc;
    memcpy( &da, &dab_bits, sizeof( da ) );
    memcpy( &db, &dab_bits, sizeof( db ) );
    memcpy( &dc, &dc_bits, sizeof( dc ) );
    dav = da;
    dbv = db;
    dcv = dc;

    int rc = rocketnet_fp_check_float( av, bv, cv );
    rc |= rocketnet_fp_check_double( dav, dbv, dcv );
    return rc;
}

int main( int argc, char ** argv )
{
    if ( test_fp_contract_off() )
        return 1;

    // The unit suite runs over the loopback transport with no sockets, so a
    // transport pin in the environment must not reach it: both pins are
    // cleared for the whole run and restored on exit, the way
    // posix_pair_create lifts them around its creates
    // (functional/posix_socket_errors.cpp). Worker processes inherit the
    // cleared environment; a case that tests pin handling itself sets and
    // reads its own pins.

    char saved_server_transport[256] = "";
    char saved_client_transport[256] = "";
    const bool server_pinned = rocketnet_test_getenv( "ROCKETNET_SERVER_TRANSPORT", saved_server_transport, sizeof( saved_server_transport ) ) != 0;
    const bool client_pinned = rocketnet_test_getenv( "ROCKETNET_CLIENT_TRANSPORT", saved_client_transport, sizeof( saved_client_transport ) ) != 0;
#if defined( _WIN32 )
    _putenv_s( "ROCKETNET_SERVER_TRANSPORT", "" );
    _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", "" );
#else
    unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
    unsetenv( "ROCKETNET_CLIENT_TRANSPORT" );
#endif // #if defined( _WIN32 )

    int result;

    // ./bin/test <name> runs that one test alone, in this process: a suite
    // name, or a message-layer test's alias (the name a card's TEST line uses)

    if ( argc == 2 && argv[1][0] != '-' )
    {
        const char * resolved = rocketnet_channels_test_resolve( argv[1] );
        result = rocketnet_unit_worker( resolved ? resolved : argv[1] );
    }
    else
    {
        // Unit runs keep RSS telemetry but leave OS-resource enforcement to
        // the functional suite's test_unit_case_memory row. That row invokes
        // this binary with --enforce-rss, reusing this process's registry.
        result = suite_main( argc, argv, rocketnet_unit_worker, rocketnet_suite_test_count, rocketnet_suite_test_name,
                             NULL, 0, NULL, 120.0 * ROCKETNET_TEST_CASE_TIME_SCALE, 0, false, false );
    }

#if defined( _WIN32 )
    if ( server_pinned )
    {
        _putenv_s( "ROCKETNET_SERVER_TRANSPORT", saved_server_transport );
    }
    if ( client_pinned )
    {
        _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", saved_client_transport );
    }
#else
    if ( server_pinned )
    {
        setenv( "ROCKETNET_SERVER_TRANSPORT", saved_server_transport, 1 );
    }
    if ( client_pinned )
    {
        setenv( "ROCKETNET_CLIENT_TRANSPORT", saved_client_transport, 1 );
    }
#endif // #if defined( _WIN32 )
    return result;
}
