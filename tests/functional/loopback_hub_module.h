/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The entry points of the two shared objects that
    test_loopback_hub_two_shared_objects loads (tests/functional/loopback_hub_shared_objects.cpp):
    tests/functional/loopback_hub_server_module.cpp and
    tests/functional/loopback_hub_client_module.cpp. Each module links its own copy
    of the development library and exports only these names; the host passes
    the hub the server module creates to the client module, so the two copies
    meet only through the hub.
*/

#pragma once

#ifndef ROCKETNET_TESTS_FUNCTIONAL_LOOPBACK_HUB_MODULE_H
#define ROCKETNET_TESTS_FUNCTIONAL_LOOPBACK_HUB_MODULE_H

#include "rocketnet.h"

#include <stdint.h>

// each module exports its entry points and nothing else: the library it
// links is a static copy whose symbols stay inside the module
#if defined( _WIN32 )
#define LOOPBACK_HUB_MODULE_EXPORT extern "C" __declspec( dllexport )
#else // #if defined( _WIN32 )
#define LOOPBACK_HUB_MODULE_EXPORT extern "C" __attribute__( ( visibility( "default" ) ) )
#endif // #if defined( _WIN32 )

#define LOOPBACK_HUB_MODULE_PROTOCOL_ID 0x4C4F4F504855420AULL
#define LOOPBACK_HUB_MODULE_CLIENT_ID 7

// the server module: init its copy, create the hub for one client and a
// started loopback server on it; returns 1 and writes the hub, the server's
// address and its public key, or 0
typedef int ( *loopback_hub_server_module_start_t )( struct rocketnet_loopback_hub_t ** hub, char * address, int address_bytes, uint8_t * public_key );

// one server frame at the caller's time: update, receive, answer each "ping"
// with "pong", send; returns the pings answered
typedef int ( *loopback_hub_server_module_update_t )( double time );

// stop and destroy the server, destroy the hub, term; returns 1 when each
// step reported ROCKETNET_STATUS_OK
typedef int ( *loopback_hub_server_module_stop_t )( void );

// the client module: init its copy, mint a development token for the server,
// create a loopback client on the hub and connect; returns 1 or 0
typedef int ( *loopback_hub_client_module_start_t )( struct rocketnet_loopback_hub_t * hub, const char * address, const uint8_t * public_key );

// one client frame at the caller's time: update, receive, send "ping" once
// connected, send; returns 1 once "pong" arrived, -1 for a client in a
// negative state, else 0
typedef int ( *loopback_hub_client_module_update_t )( double time );

// rocketnet_loopback_hub_destroy called from the client module's copy
typedef enum rocketnet_status_t ( *loopback_hub_client_module_destroy_hub_t )( struct rocketnet_loopback_hub_t * hub );

// disconnect and destroy the client, updating from the caller's last frame
// time, term; returns 1 when each step reported ROCKETNET_STATUS_OK
typedef int ( *loopback_hub_client_module_stop_t )( double time );

#endif // ROCKETNET_TESTS_FUNCTIONAL_LOOPBACK_HUB_MODULE_H
