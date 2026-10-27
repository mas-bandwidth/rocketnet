/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// What the tests, tools and benches set on a client that a game never does:
// the receive stamp clock, and, in a development build, the step-driven client
// (no network thread; update and send_packets run its I/O step on the caller's
// thread). Games include rocketnet.h alone. The step-driven setting and its
// environment control do not exist in a production library.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_TESTING_H
#define ROCKETNET_SDK_ROCKETNET_TESTING_H

#include "rocketnet.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rocketnet_client_testing_t
{
    // the receive stamp clock; NULL is the platform clock; runs on the
    // network thread, and on the caller's thread during disconnect and destroy
    double (*time_callback)( void * context );

    // Development only: true runs the client's I/O step inside update and
    // send_packets on the caller's thread, with no network thread. The field
    // stays in this private test struct in production so non-stepping clock
    // fixtures keep one source shape; production creation ignores it.
    bool step_driven;
};

// rocketnet_client_create with the testing fields; a NULL testing is the
// plain create

struct rocketnet_client_t * rocketnet_client_create_testing( void * context,
                                                             const struct rocketnet_client_config_t * config,
                                                             const struct rocketnet_client_testing_t * testing,
                                                             enum rocketnet_status_t * status );

// the development network simulator calls, declared here for the unit
// suite's production-posture build, whose test hooks carry them

#if !ROCKETNET_DEV
void rocketnet_client_network_simulator( struct rocketnet_client_t * client, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );
void rocketnet_server_network_simulator( struct rocketnet_server_t * server, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );
#endif // !ROCKETNET_DEV

#ifdef __cplusplus
}

// C++ callers: a client config with the testing fields beside the public ones. The
// default_config and create overloads take it where the public config goes,
// so a test declares its config with this type and sets either kind of field.

struct rocketnet_client_test_config_t : rocketnet_client_config_t, rocketnet_client_testing_t
{
};

// templates, so a NULL argument still picks the plain C call

template <typename config_t> static inline enum rocketnet_status_t rocketnet_client_default_config( config_t * config )
{
    rocketnet_client_testing_t * testing = config;
    *testing = rocketnet_client_testing_t();
    return rocketnet_client_default_config( static_cast<rocketnet_client_config_t *>( config ) );
}

template <typename config_t> static inline struct rocketnet_client_t * rocketnet_client_create( void * context, const config_t * config, enum rocketnet_status_t * status )
{
    return rocketnet_client_create_testing( context, config, config, status );
}

#endif // __cplusplus

#endif // ROCKETNET_SDK_ROCKETNET_TESTING_H
