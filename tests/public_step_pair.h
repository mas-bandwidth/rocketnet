/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The shared development-only step-driven client/server pair setup for the
// test_public_step_driven_client_* cases in
// tests/test_unit_public_step_driven_client.cpp (unit) and
// functional/process_thread_counts.cpp (functional). The loopback transport
// carries packets in memory; callers provide every update time from 1000.0,
// and no test sleeps or opens a socket. The includer defines `check` before
// including. The pair setup itself counts no threads; the functional thread
// case passes a thread-count callback and the unit side leaves it NULL.

#pragma once

#ifndef ROCKETNET_TESTS_PUBLIC_STEP_PAIR_H
#define ROCKETNET_TESTS_PUBLIC_STEP_PAIR_H

#include "unit_shutdown.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_config.h"
#include "loopback_hub.h"

#define PUBLIC_STEP_PROTOCOL 0x51E7D41A92B3C6D8ULL

struct public_step_pair_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    double time;
    int threads_before_pair;
    int threads_before_client;
    int threads_after_client;
};

struct public_step_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static void public_step_identity( public_step_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0xA5 ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static void public_step_server_config( rocketnet_server_config_t * config, const public_step_identity_t * identity )
{
    rocketnet_server_default_config( config );
    config->protocol_id = PUBLIC_STEP_PROTOCOL;
    config->max_clients = 1;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
    config->loopback_hub = rocketnet_test_loopback_hub();
}

static void public_step_token( uint16_t port, const uint8_t * server_public, const public_step_identity_t * identity, uint8_t * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );

    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, server_public,
                                                     PUBLIC_STEP_PROTOCOL, addresses, 1, 0x5EED, 1000, 1ULL << 40, 300, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
}

// The string token for a running server, sealed to the key the server made.

static void public_step_token_string( const rocketnet_server_t * server, const public_step_identity_t * identity, char * token_string )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_step_token( rocketnet_server_port( server ), server_public, identity, token );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

static double public_step_now = 1000.0;
static double public_step_clock( void * ) { return public_step_now; }

static void public_step_client_config( rocketnet_client_test_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->loopback_hub = rocketnet_test_loopback_hub();
    config->step_driven = true;
    config->time_callback = public_step_clock;
}

static void public_step_pair_create( public_step_pair_t * pair, const public_step_identity_t * identity,
                                     uint16_t fixed_port = 0, int ( *thread_count )() = NULL )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    pair->threads_before_pair = thread_count ? thread_count() : -1;
    rocketnet_quiet( 1 );

    rocketnet_server_config_t server_config;
    public_step_server_config( &server_config, identity );

    char bind[64];
    snprintf( bind, sizeof( bind ), "127.0.0.1:%u", (unsigned) fixed_port );
    pair->server = rocketnet_server_create( NULL, bind, NULL, &server_config, NULL );
    check( pair->server != NULL );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    public_step_token_string( pair->server, identity, token );

    rocketnet_client_test_config_t client_config;
    public_step_client_config( &client_config );
    pair->threads_before_client = thread_count ? thread_count() : -1;
    pair->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( pair->client != NULL );
    check( rocketnet_client_connect( pair->client, token ) == ROCKETNET_STATUS_OK );
    pair->threads_after_client = thread_count ? thread_count() : -1;
}

static void public_step_pair_stop_server_and_wait( public_step_pair_t * pair )
{
    rocketnet_server_stop( pair->server );

    // poll the completion query within a fixed budget; do not sleep

    for ( int step = 0; step < 32 && !rocketnet_server_stopped( pair->server ); step++ )
    {
    }

    check( rocketnet_server_stopped( pair->server ) );
}

static void public_step_pair_destroy( public_step_pair_t * pair )
{
    rn_test_client_stop_and_destroy( pair->client );
    public_step_pair_stop_server_and_wait( pair );
    rocketnet_server_destroy( pair->server );
}

// RNClientIo CallerStep: update and send_packets execute the non-blocking I/O
// step on this caller's thread; no I/O-thread action exists in this mode.

static void public_step_pump( public_step_pair_t * pair )
{
    public_step_now = pair->time;
    rocketnet_client_update( pair->client, pair->time );
    rocketnet_client_send_packets( pair->client );
    rocketnet_server_update( pair->server, pair->time );
    rocketnet_server_send_packets( pair->server );
    pair->time += 0.01;
}

static void public_step_connect( public_step_pair_t * pair )
{
    for ( int i = 0; i < 400 && rocketnet_client_state( pair->client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        public_step_pump( pair );
    }
    check( rocketnet_client_state( pair->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair->server, 0 ) != 0 );
}

#endif // ROCKETNET_TESTS_PUBLIC_STEP_PAIR_H
