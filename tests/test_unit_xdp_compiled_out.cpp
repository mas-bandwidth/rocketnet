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

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_config.h"
#include "loopback_hub.h"
#include "unit_shutdown.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && !ROCKETNET_XDP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// production-like keys: non-zero and not the published development keys
static void xdp_compiled_out_config( struct rocketnet_server_transport_config_t * config, const char * transport )
{
    rocketnet_server_transport_default_config_current( config );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
    config->max_clients = 1;
    snprintf( config->transport, sizeof( config->transport ), "%s", transport );
}

static void test_xdp_compiled_out_pin_refuses_not_built()
{

    struct rocketnet_server_transport_config_t config;
    xdp_compiled_out_config( &config, "xdp" );

    int status = -1;
    check( rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:40000", &config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_TRANSPORT_NOT_BUILT );
    check( strcmp( rocketnet_status_name( (enum rocketnet_status_t) status ), "TRANSPORT_NOT_BUILT" ) == 0 );

    // named twice, still one refusal for the same reason

    xdp_compiled_out_config( &config, "xdp,xdp" );
    status = -1;
    check( rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:40000", &config, 1000.0, &status ) == NULL );
    check( status == ROCKETNET_STATUS_TRANSPORT_NOT_BUILT );

}

REGISTER_UNIT_TEST( test_xdp_compiled_out_pin_refuses_not_built );

static void test_xdp_compiled_out_list_takes_the_next_candidate()
{

    struct rocketnet_server_transport_config_t config;
    xdp_compiled_out_config( &config, "xdp,loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:40000", &config, 1000.0, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );

    if ( server )
    {
        check( strcmp( rocketnet_server_transport_io_name( server ), "loopback" ) == 0 );

        rn_test_server_transport_stop_and_destroy( server );
    }

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
