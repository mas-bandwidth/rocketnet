/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A Linux build without XDP (CMakeLists.txt: no clang with the BPF target, no
// libbpf or libxdp, or ROCKETNET_XDP=OFF) compiles the XDP transport out. The
// xdp row stays in the registry, so a transport list naming xdp refuses it at
// create with TRANSPORT_NOT_BUILT, never falling silently to another
// transport, and a list that goes on past xdp is granted the next candidate.
// That the library carries no XDP code is checked on the built archive by
// tools/check_xdp_compiled_out.py.
//
// The cases register only in a Linux build without XDP; an XDP build's suite
// is unchanged. Every case runs in memory: the xdp refusal opens nothing and
// the loopback transport opens no socket, with no clock or network.

#include "test_unit_registry.h"
#include "test/portable_crt.h"

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && !ROCKETNET_XDP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// the env pin wins over the struct's list by design, so a CI leg pinned by
// ROCKETNET_SERVER_TRANSPORT is set aside for the case and put back after

struct xdp_compiled_out_env_pin
{
    bool was_set;
    char value[256];
};

static void xdp_compiled_out_pin_set_aside( xdp_compiled_out_env_pin * saved )
{
    saved->was_set = rocketnet_test_getenv( "ROCKETNET_SERVER_TRANSPORT", saved->value, sizeof(saved->value) ) != 0;
    unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
}

static void xdp_compiled_out_pin_put_back( const xdp_compiled_out_env_pin * saved )
{
    if ( saved->was_set )
        setenv( "ROCKETNET_SERVER_TRANSPORT", saved->value, 1 );
}

// production-like keys: non-zero and not the published development keys
static void xdp_compiled_out_config( struct rocketnet_server_transport_config_t * config, const char * transport )
{
    rocketnet_server_transport_default_config( config );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
    config->max_clients = 1;
    snprintf( config->transport, sizeof( config->transport ), "%s", transport );
}

static void test_xdp_compiled_out_pin_refuses_not_built()
{
    xdp_compiled_out_env_pin pin;
    xdp_compiled_out_pin_set_aside( &pin );

    struct rocketnet_server_transport_config_t config;
    xdp_compiled_out_config( &config, "xdp" );
    int status = -1;
    check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:40000", &config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_TRANSPORT_NOT_BUILT );
    check( strcmp( rocketnet_status_name( (enum rocketnet_status_t) status ), "TRANSPORT_NOT_BUILT" ) == 0 );

    // named twice, still one refusal for the same reason
    xdp_compiled_out_config( &config, "xdp,xdp" );
    status = -1;
    check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:40000", &config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_TRANSPORT_NOT_BUILT );

    xdp_compiled_out_pin_put_back( &pin );
}

REGISTER_UNIT_TEST( test_xdp_compiled_out_pin_refuses_not_built );

static void test_xdp_compiled_out_list_takes_the_next_candidate()
{
    xdp_compiled_out_env_pin pin;
    xdp_compiled_out_pin_set_aside( &pin );

    struct rocketnet_server_transport_config_t config;
    xdp_compiled_out_config( &config, "xdp,loopback" );
    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:40000", &config, 1000.0, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    if ( server )
    {
        check( strcmp( rocketnet_server_transport_io_name( server ), "loopback" ) == 0 );
        rocketnet_server_transport_destroy( server );
    }

    xdp_compiled_out_pin_put_back( &pin );
}

REGISTER_UNIT_TEST( test_xdp_compiled_out_list_takes_the_next_candidate );

static void test_xdp_compiled_out_has_no_memory_plan()
{
    // no XDP plan is reported, so the ladder's plan loop logs xdp as unavailable
    struct rocketnet_server_transport_config_t config;
    xdp_compiled_out_config( &config, "xdp" );
    rocketnet_server_memory_plan_t plan;
    check( !rocketnet_server_transport_memory_plan( &config, "xdp", plan ) );
}

REGISTER_UNIT_TEST( test_xdp_compiled_out_has_no_memory_plan );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && !ROCKETNET_XDP
