/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The local address a server binds (bind_ipv4_address, bind_ipv6_address in
// the server config): rocketnet_server_transport_resolve_bind_address read
// against each rule, and the public create carrying the fields through to a
// refusal. Unset binds the served family's wildcard on the served port; set,
// that address on the served port; every refusal is INVALID_ADDRESS.
//
// The environment forms are ambient: they apply to a served family and are
// ignored, with a log line, for a family the server does not serve, while
// the config fields keep refusing that mismatch. Both paths that read the
// environment are covered: the public create, and a bare transport create.
//
// No sockets, no real time: the resolve is pure, and the servers run on the
// loopback transport in memory.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_layers.h"
#include "unit_shutdown.h"
#include "loopback_hub.h"
#if ROCKETNET_DEV
#include "test_env_source.h"
#endif // #if ROCKETNET_DEV

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static struct rocketnet_address_t bind_served( const char * text )
{
    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, text ) );
    return address;
}

static bool bind_resolve( const char * ipv4, const char * ipv6, const char * served, int serves_both, struct rocketnet_address_t * bound, int * status )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    snprintf( config.bind_ipv4_address, sizeof( config.bind_ipv4_address ), "%s", ipv4 );
    snprintf( config.bind_ipv6_address, sizeof( config.bind_ipv6_address ), "%s", ipv6 );
    const struct rocketnet_address_t public_address = bind_served( served );
    *status = ROCKETNET_STATUS_OK;
    return rocketnet_server_transport_resolve_bind_address( &config, &public_address, serves_both, bound, status );
}

static void bind_refused( const char * ipv4, const char * ipv6, const char * served, int serves_both )
{
    struct rocketnet_address_t bound;
    int status = ROCKETNET_STATUS_OK;
    check( !bind_resolve( ipv4, ipv6, served, serves_both, &bound, &status ) );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );
}

// unset: the served family's wildcard on the served port, as before the field
static void test_server_bind_address_unset_is_wildcard()
{
    rocketnet_quiet( 1 );

    struct rocketnet_address_t bound;
    int status = ROCKETNET_STATUS_OK;

    check( bind_resolve( "", "", "203.0.113.7:40000", 0, &bound, &status ) );
    check( bound.type == ROCKETNET_ADDRESS_IPV4 && bound.port == 40000 );
    check( bound.data.ipv4[0] == 0 && bound.data.ipv4[1] == 0 && bound.data.ipv4[2] == 0 && bound.data.ipv4[3] == 0 );

    check( bind_resolve( "", "", "[2001:db8::7]:40000", 0, &bound, &status ) );
    check( bound.type == ROCKETNET_ADDRESS_IPV6 && bound.port == 40000 );
    for ( int i = 0; i < 8; i++ )
    {
        check( bound.data.ipv6[i] == 0 );
    }

    // both families: the IPv6 wildcard, one dual-stack socket
    check( bind_resolve( "", "", "203.0.113.7:40000", 1, &bound, &status ) );
    check( bound.type == ROCKETNET_ADDRESS_IPV6 && bound.port == 40000 );
    for ( int i = 0; i < 8; i++ )
    {
        check( bound.data.ipv6[i] == 0 );
    }
}

// set: that address on the served port; the served address may differ (NAT)
static void test_server_bind_address_set_binds_it_on_the_served_port()
{
    rocketnet_quiet( 1 );

    struct rocketnet_address_t bound;
    int status = ROCKETNET_STATUS_OK;

    check( bind_resolve( "10.0.1.5", "", "203.0.113.7:40000", 0, &bound, &status ) );
    check( status == ROCKETNET_STATUS_OK );
    struct rocketnet_address_t expected = bind_served( "10.0.1.5:40000" );
    check( rocketnet_address_equal( &bound, &expected ) );

    check( bind_resolve( "", "fd00::5", "[2001:db8::7]:40000", 0, &bound, &status ) );
    expected = bind_served( "[fd00::5]:40000" );
    check( rocketnet_address_equal( &bound, &expected ) );

    check( bind_resolve( "", "[fd00::5]", "[2001:db8::7]:40000", 0, &bound, &status ) );
    check( rocketnet_address_equal( &bound, &expected ) );

    // port zero served: the bind takes zero too, and the kernel's port is adopted as before
    check( bind_resolve( "127.0.0.1", "", "127.0.0.1:0", 0, &bound, &status ) );
    expected = bind_served( "127.0.0.1:0" );
    check( rocketnet_address_equal( &bound, &expected ) );
}

static void test_server_bind_address_refusals()
{
    rocketnet_quiet( 1 );

    // does not parse
    bind_refused( "10.0.1", "", "203.0.113.7:40000", 0 );
    bind_refused( "", "fd00::5::1", "[2001:db8::7]:40000", 0 );

    // the other family in the field
    bind_refused( "fd00::5", "", "203.0.113.7:40000", 0 );
    bind_refused( "", "10.0.1.5", "[2001:db8::7]:40000", 0 );

    // a port
    bind_refused( "10.0.1.5:40000", "", "203.0.113.7:40000", 0 );
    bind_refused( "", "[fd00::5]:40000", "[2001:db8::7]:40000", 0 );
    bind_refused( "10.0.1.5:0", "", "203.0.113.7:40000", 0 );
    bind_refused( "", "[fd00::5]:0", "[2001:db8::7]:40000", 0 );

    // multicast and broadcast
    bind_refused( "224.0.0.1", "", "203.0.113.7:40000", 0 );
    bind_refused( "239.255.255.250", "", "203.0.113.7:40000", 0 );
    bind_refused( "255.255.255.255", "", "203.0.113.7:40000", 0 );
    bind_refused( "", "ff02::1", "[2001:db8::7]:40000", 0 );

    // IPv4-mapped IPv6 binds an IPv4 socket address; link-local needs a scope
    bind_refused( "", "::ffff:10.0.0.5", "[2001:db8::7]:40000", 0 );
    bind_refused( "", "fe80::1", "[2001:db8::7]:40000", 0 );
    bind_refused( "", "febf::1", "[2001:db8::7]:40000", 0 );

    // a family the server does not serve
    bind_refused( "", "fd00::5", "203.0.113.7:40000", 0 );
    bind_refused( "10.0.1.5", "", "[2001:db8::7]:40000", 0 );

    // both families served: one dual-stack socket binds no specific address
    bind_refused( "10.0.1.5", "", "203.0.113.7:40000", 1 );
    bind_refused( "", "fd00::5", "203.0.113.7:40000", 1 );

    // the edges that pass: either side of IPv4 multicast, the first IPv6 address
    // above link-local, and the mapped prefix one off
    struct rocketnet_address_t bound;
    int status = ROCKETNET_STATUS_OK;
    check( bind_resolve( "223.255.255.255", "", "203.0.113.7:40000", 0, &bound, &status ) );
    check( bind_resolve( "240.0.0.1", "", "203.0.113.7:40000", 0, &bound, &status ) );
    check( bind_resolve( "", "fec0::1", "[2001:db8::7]:40000", 0, &bound, &status ) );
    check( bind_resolve( "", "::fffe:10.0.0.5", "[2001:db8::7]:40000", 0, &bound, &status ) );
}

#if ROCKETNET_DEV

// the public config carries the fields to the transport: a bind address the
// resolve refuses refuses the start with INVALID_ADDRESS, and one it accepts
// starts the server
static enum rocketnet_status_t bind_public_start( const char * bind_ipv4 )
{
    struct rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.bind_ipv4_address == NULL && config.bind_ipv6_address == NULL );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    config.protocol_id = 0x62696e6461646472ULL;
    config.max_clients = 1;
    memcpy( config.backend_public_key, backend_public, sizeof( config.backend_public_key ) );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.bind_ipv4_address = bind_ipv4;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    check( server != NULL && status == ROCKETNET_STATUS_OK );
    status = rocketnet_server_start( server );
    rn_test_server_stop_and_destroy( server );
    return status;
}

static void test_server_bind_address_public_start()
{
    rocketnet_quiet( 1 );
    check( bind_public_start( "224.0.0.1" ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( bind_public_start( "fd00::5" ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( bind_public_start( "127.0.0.1" ) == ROCKETNET_STATUS_OK );
    check( bind_public_start( NULL ) == ROCKETNET_STATUS_OK );
}

// the rule itself: a field the environment changed for a family not served
// goes back to its value before the environment; a served family keeps the
// environment's value, and a field the environment did not change is left
// for the resolve to judge
static void test_server_bind_address_ambient_rule()
{
    rocketnet_quiet( 1 );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    snprintf( config.bind_ipv6_address, sizeof( config.bind_ipv6_address ), "%s", "fd00::5" );
    rocketnet_server_transport_ignore_ambient_bind( &config, "", "", 1, 0 );
    check( config.bind_ipv6_address[0] == '\0' );

    snprintf( config.bind_ipv6_address, sizeof( config.bind_ipv6_address ), "%s", "fd00::5" );
    rocketnet_server_transport_ignore_ambient_bind( &config, "", "", 1, 1 );
    check( strcmp( config.bind_ipv6_address, "fd00::5" ) == 0 );

    // set in the struct, the environment silent: kept, and the resolve refuses it
    rocketnet_server_transport_ignore_ambient_bind( &config, "", "fd00::5", 1, 0 );
    check( strcmp( config.bind_ipv6_address, "fd00::5" ) == 0 );

    // the environment overrode the struct's value: back to the struct's
    snprintf( config.bind_ipv4_address, sizeof( config.bind_ipv4_address ), "%s", "10.0.0.9" );
    rocketnet_server_transport_ignore_ambient_bind( &config, "10.0.0.1", "fd00::5", 0, 1 );
    check( strcmp( config.bind_ipv4_address, "10.0.0.1" ) == 0 );
}

static void bind_env( const char * ipv4, const char * ipv6 )
{
    if ( ipv4 )
    {
        check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_BIND_IPV4_ADDRESS", ipv4, 1 ) == 0 );
    }
    else
    {
        check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_BIND_IPV4_ADDRESS" ) == 0 );
    }
    if ( ipv6 )
    {
        check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_BIND_IPV6_ADDRESS", ipv6, 1 ) == 0 );
    }
    else
    {
        check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_BIND_IPV6_ADDRESS" ) == 0 );
    }
}

// the public create: the environment's IPv6 bind address on an IPv4 server
// is ignored and the server starts; its IPv4 one applies, so a multicast
// one refuses the start; the config field of the other family still refuses
static void test_server_bind_address_environment_public()
{
    rocketnet_quiet( 1 );

    bind_env( NULL, "fd00::5" );
    check( bind_public_start( NULL ) == ROCKETNET_STATUS_OK );

    bind_env( "224.0.0.1", NULL );
    check( bind_public_start( NULL ) == ROCKETNET_STATUS_INVALID_ADDRESS );

    bind_env( "127.0.0.1", "fd00::5" );
    check( bind_public_start( NULL ) == ROCKETNET_STATUS_OK );

    bind_env( NULL, NULL );
    check( bind_public_start( "fd00::5" ) == ROCKETNET_STATUS_INVALID_ADDRESS );
}

// a bare transport create reads the environment itself: the same rule
static enum rocketnet_status_t bind_transport_create( const char * bind_ipv6 )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = 1;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    snprintf( config.bind_ipv6_address, sizeof( config.bind_ipv6_address ), "%s", bind_ipv6 );

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_internal( NULL, "127.0.0.1:40611", &config, 1000.0, &status );
    if ( server )
    {
        rn_test_server_transport_stop_and_destroy( server );
    }
    return server ? ROCKETNET_STATUS_OK : (enum rocketnet_status_t) status;
}

static void test_server_bind_address_environment_transport()
{
    rocketnet_quiet( 1 );

    bind_env( NULL, "fd00::5" );
    check( bind_transport_create( "" ) == ROCKETNET_STATUS_OK );

    bind_env( "224.0.0.1", NULL );
    check( bind_transport_create( "" ) == ROCKETNET_STATUS_INVALID_ADDRESS );

    bind_env( NULL, NULL );
    check( bind_transport_create( "fd00::5" ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    check( bind_transport_create( "" ) == ROCKETNET_STATUS_OK );
}

#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_server_bind_address_unset_is_wildcard );
REGISTER_UNIT_TEST( test_server_bind_address_set_binds_it_on_the_served_port );
REGISTER_UNIT_TEST( test_server_bind_address_refusals );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_server_bind_address_public_start );
REGISTER_UNIT_TEST( test_server_bind_address_ambient_rule );
REGISTER_UNIT_TEST( test_server_bind_address_environment_public );
REGISTER_UNIT_TEST( test_server_bind_address_environment_transport );
#endif // #if ROCKETNET_DEV
