/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// What the unit suite's production-posture build declares that a game never
// does. The step-driven client and its receive stamp clock are the public
// client config's step_driven and time_callback, development builds only.
// Games include rocketnet.h alone.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_TESTING_H
#define ROCKETNET_SDK_ROCKETNET_TESTING_H

#include "rocketnet.h"

#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus

// the development network simulator calls, declared here for the unit
// suite's production-posture build, whose test hooks carry them

#if !ROCKETNET_DEV
void rocketnet_client_network_simulator( struct rocketnet_client_t * client, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );
void rocketnet_server_network_simulator( struct rocketnet_server_t * server, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );
#endif // #if !ROCKETNET_DEV

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // ROCKETNET_SDK_ROCKETNET_TESTING_H
