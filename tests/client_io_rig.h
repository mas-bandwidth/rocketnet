/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Storage for rocketnet_client_io_init in unit tests: every send ring with a
// few slots, every receive buffer, and the handoff mutex. The receive buffers
// are large, so the rig lives on the heap. No socket, no thread, no time.

#pragma once

#ifndef ROCKETNET_CLIENT_IO_RIG_H
#define ROCKETNET_CLIENT_IO_RIG_H

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_platform.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROCKETNET_CLIENT_IO_RIG_SLOTS 4

struct rocketnet_client_io_rig_t
{
    uint8_t ring[ROCKETNET_CLIENT_IO_NUM_RINGS][ROCKETNET_CLIENT_IO_RIG_SLOTS * ROCKETNET_FRAME_STRIDE];
    int counts[ROCKETNET_CLIENT_IO_NUM_RINGS][ROCKETNET_CLIENT_IO_RIG_SLOTS];
    rocketnet_client_io_ring_view_t view[ROCKETNET_CLIENT_IO_NUM_RINGS];
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    rocketnet_platform_mutex_t mutex;
};

static inline rocketnet_client_io_rig_t * rocketnet_client_io_rig_create()
{
    rocketnet_client_io_rig_t * rig = new rocketnet_client_io_rig_t();
    memset( rig->ring, 0, sizeof( rig->ring ) );
    memset( rig->counts, 0, sizeof( rig->counts ) );
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        rig->view[i].data = rig->ring[i];
        rig->view[i].frame_stride = ROCKETNET_FRAME_STRIDE;
        rig->view[i].wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
        rig->view[i].wire_bytes = (const uint8_t *) rig->counts[i];
        rig->view[i].wire_bytes_stride = sizeof( int );
    }
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
        rig->buffers[i] = new rocketnet_client_receive_buffer_t;
    if ( !rocketnet_platform_mutex_create( &rig->mutex ) )
    {
        printf( "client io rig: mutex create failed\n" );
        exit( 1 );
    }
    return rig;
}

static inline void rocketnet_client_io_rig_init( rocketnet_client_io_rig_t * rig, rocketnet_client_io_t * io,
    const rocketnet_client_transport_io_t * transport, double (*time)( void * context ), void * time_context )
{
    rocketnet_client_io_init( io, transport, &rig->mutex, rig->view, rig->buffers, time, time_context );
}

// the bytes of slot `slot` of ring `ring`, where the I/O step reads them
static inline uint8_t * rocketnet_client_io_rig_slot( rocketnet_client_io_rig_t * rig, int ring, int slot )
{
    return rig->ring[ring] + slot * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET;
}

static inline void rocketnet_client_io_rig_destroy( rocketnet_client_io_rig_t * rig )
{
    rocketnet_platform_mutex_destroy( &rig->mutex );
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
        delete rig->buffers[i];
    delete rig;
}

#endif // #ifndef ROCKETNET_CLIENT_IO_RIG_H
