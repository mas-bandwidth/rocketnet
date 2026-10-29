/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_TESTS_LOOPBACK_HUB_H
#define ROCKETNET_TESTS_LOOPBACK_HUB_H

// The test process's loopback hub. Every unit and functional case runs in a
// process of its own, so one hub per process is one hub per case: created at
// its first use with rocketnet_test_loopback_hub_clients clients (8 unless the
// case asks for more first), and destroyed by the worker after the case and
// before rocketnet_term; a case that terms the library itself releases it
// first. A destroy refused because an endpoint is still registered fails the
// case. Its allocator context is NULL unless the case names one first
// (rocketnet_test_loopback_hub_context), as a case whose allocator reads its
// context must.

#include "../sdk/rocketnet.h"

#include <stdio.h>
#include <stdlib.h>

#define ROCKETNET_TEST_LOOPBACK_HUB_CLIENTS 8

struct rocketnet_test_loopback_hub_state_t
{
    struct rocketnet_loopback_hub_t * hub;
    int clients;
    void * context;
};

inline struct rocketnet_test_loopback_hub_state_t * rocketnet_test_loopback_hub_state()
{
    static struct rocketnet_test_loopback_hub_state_t state = { NULL, ROCKETNET_TEST_LOOPBACK_HUB_CLIENTS, NULL };
    return &state;
}

// the allocator context the hub is created with; called before the case's
// first rocketnet_test_loopback_hub
inline void rocketnet_test_loopback_hub_context( void * context )
{
    struct rocketnet_test_loopback_hub_state_t * state = rocketnet_test_loopback_hub_state();
    if ( state->hub )
    {
        printf( "error: the test loopback hub was created before the case named its allocator context\n" );
        exit( 1 );
    }

    state->context = context;
}

// sizes the hub for a case with more clients than the default; called before
// the case's first rocketnet_test_loopback_hub
inline void rocketnet_test_loopback_hub_clients( int clients )
{
    struct rocketnet_test_loopback_hub_state_t * state = rocketnet_test_loopback_hub_state();
    if ( state->hub && state->clients < clients )
    {
        printf( "error: the test loopback hub was created for %d clients before the case asked for %d\n", state->clients, clients );
        exit( 1 );
    }

    if ( clients > state->clients )
        state->clients = clients;
}

inline struct rocketnet_loopback_hub_t * rocketnet_test_loopback_hub()
{
    struct rocketnet_test_loopback_hub_state_t * state = rocketnet_test_loopback_hub_state();
    if ( !state->hub )
    {
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        state->hub = rocketnet_loopback_hub_create( state->context, state->clients, &status );
        if ( !state->hub )
        {
            printf( "error: the test loopback hub for %d clients was refused: %s\n", state->clients, rocketnet_status_name( status ) );
            exit( 1 );
        }
    }

    return state->hub;
}

// destroys the process's hub, if the case made one; false when the destroy
// was refused (an endpoint still registered)
inline bool rocketnet_test_loopback_hub_release()
{
    struct rocketnet_test_loopback_hub_state_t * state = rocketnet_test_loopback_hub_state();
    if ( !state->hub )
    {
        state->clients = ROCKETNET_TEST_LOOPBACK_HUB_CLIENTS;
        state->context = NULL;
        return true;
    }

    const enum rocketnet_status_t status = rocketnet_loopback_hub_destroy( state->hub );
    if ( status != ROCKETNET_STATUS_OK )
    {
        printf( "error: the test loopback hub destroy was refused: %s\n", rocketnet_status_name( status ) );
        return false;
    }

    state->hub = NULL;
    state->clients = ROCKETNET_TEST_LOOPBACK_HUB_CLIENTS;
    state->context = NULL;

    return true;
}

#endif // ROCKETNET_TESTS_LOOPBACK_HUB_H
