/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_RECEIVE_DELIVERY_CHECK_H
#define ROCKETNET_SDK_ROCKETNET_RECEIVE_DELIVERY_CHECK_H

// The receive delivery check, shared by every transport whose receive crosses
// from a worker thread to the main thread (development builds only). A
// transport's test fixture supplies a rig: a worker pass that banks datagrams
// and ends as the worker's pass ends (its commit), and one receive call of
// main. The check runs a script of passes and calls on the calling thread, in
// a fixed order, and requires every receive call to return every datagram
// banked by a pass that ended before the call began, in order, and nothing
// else: proofs/tla/RNReceiveHandoff.tla DeliveredByNextCall. A rule that leaves
// a datagram for a later call (one update of input latency) fails it. No
// socket, no thread and no clock: a unit test.
//
// The script is a list of steps separated by spaces: "w<n>" is one worker
// pass banking n datagrams (n may be 0: a pass that banks nothing), "m" is one
// receive call. Each rig numbers its datagrams by the ids the check gives it.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ROCKETNET_RECEIVE_DELIVERY_MAX_IDS 256

struct rocketnet_receive_delivery_rig_t
{
    void * user;

    // one worker pass: bank the datagrams first .. first + count - 1, then end
    // the pass as the worker does (its commit under the handoff mutex)
    bool ( *worker_pass )( void * user, uint32_t first, int count );

    // one receive call of main: write the ids it returned, in order, into ids
    // and return how many, or -1 when the fixture failed
    int ( *main_call )( void * user, uint32_t * ids, int capacity );
};

// the scripts every transport runs: two passes in one frame, a burst after
// idle calls, several passes between calls, and passes that bank nothing
static const char * const rocketnet_receive_delivery_scripts[] = {
    "w1 m w1 w1 m",
    "m m m w3 m",
    "w1 w1 w1 m w2 m m w1 m",
    "w1 m w1 m w1 w1 w1 m w0 m",
    "w2 w0 m w1 w0 w1 m w0 m",
    "w1 m w1 w1 w1 w1 m w1 w1 m m w4 w1 m",
};

static inline bool rocketnet_receive_delivery_check( const struct rocketnet_receive_delivery_rig_t * rig, const char * name, const char * script )
{
    uint32_t next = 1;          // the next id a pass banks
    uint32_t delivered = 1;     // the next id a call must return
    uint32_t ids[ROCKETNET_RECEIVE_DELIVERY_MAX_IDS];
    int step = 0;

    const char * p = script;
    while ( *p )
    {
        if ( *p == ' ' )
        {
            p++;
            continue;
        }

        step++;

        if ( *p == 'w' )
        {
            char * end = NULL;
            const long count = strtol( p + 1, &end, 10 );
            if ( end == p + 1 || count < 0 || next + (uint32_t) count > ROCKETNET_RECEIVE_DELIVERY_MAX_IDS )
            {
                printf( "receive delivery %s: bad script step %d of \"%s\"\n", name, step, script );
                return false;
            }
            if ( !rig->worker_pass( rig->user, next, (int) count ) )
            {
                printf( "receive delivery %s: worker pass at step %d of \"%s\" failed\n", name, step, script );
                return false;
            }
            next += (uint32_t) count;
            p = end;
            continue;
        }

        if ( *p != 'm' )
        {
            printf( "receive delivery %s: bad script step %d of \"%s\"\n", name, step, script );
            return false;
        }
        p++;

        const int returned = rig->main_call( rig->user, ids, ROCKETNET_RECEIVE_DELIVERY_MAX_IDS );
        if ( returned < 0 )
        {
            printf( "receive delivery %s: receive call at step %d of \"%s\" failed\n", name, step, script );
            return false;
        }

        // every datagram banked before this call began, in order, and nothing else

        const int expected = (int) ( next - delivered );
        bool exact = returned == expected;
        for ( int i = 0; exact && i < returned; i++ )
        {
            exact = ids[i] == delivered + (uint32_t) i;
        }
        if ( !exact )
        {
            printf( "receive delivery %s: the receive call at step %d of \"%s\" returned %d datagrams (first id %u); "
                    "every datagram banked before it began is %d, ids %u to %u\n",
                    name, step, script, returned, returned > 0 ? ids[0] : 0u, expected, delivered, next - 1 );
            return false;
        }
        delivered = next;
    }

    return true;
}

// runs every shared script; the rig's fixture is fresh for each (reset is
// called before each script, and may be NULL when one fixture serves all)
static inline bool rocketnet_receive_delivery_check_all( const struct rocketnet_receive_delivery_rig_t * rig, const char * name,
                                                         bool ( *reset )( void * user ) )
{
    bool valid = true;
    for ( size_t i = 0; i < sizeof( rocketnet_receive_delivery_scripts ) / sizeof( rocketnet_receive_delivery_scripts[0] ); i++ )
    {
        if ( reset && !reset( rig->user ) )
        {
            printf( "receive delivery %s: fixture reset failed\n", name );
            return false;
        }
        valid = rocketnet_receive_delivery_check( rig, name, rocketnet_receive_delivery_scripts[i] ) && valid;
    }
    return valid;
}

#endif // ROCKETNET_SDK_ROCKETNET_RECEIVE_DELIVERY_CHECK_H
