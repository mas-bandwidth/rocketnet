/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CLIENT_IO_H
#define ROCKETNET_CLIENT_IO_H

#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rocketnet.h"
#include "rocketnet_constants.h"
#include "rocketnet_driver.h"
#include "rocketnet_platform.h"
#include "rocketnet_client_send_rings.h"
#include "rocketnet_client_receive_handshake.h"

// The client I/O step of docs/CLIENT-IO.md _The I/O step_ and _The wake_, and
// the RNClientIo actions it names. The thread loop is one call per iteration;
// unit tests call it directly, with a fake driver and a test clock, on the
// test's thread.

#define ROCKETNET_CLIENT_IO_BATCH 64
#define ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS 0.001   // a guard, never a cadence
#define ROCKETNET_CLIENT_IO_NUM_BUFFERS 2      // two receive buffers, no third (the design's receive path)

enum { ROCKETNET_CLIENT_IO_IDLE = 0, ROCKETNET_CLIENT_IO_WORKED = 1, ROCKETNET_CLIENT_IO_EXITED = 2 };

struct rocketnet_client_io_ring_view_t
{
    uint8_t * data; size_t frame_stride; size_t wire_offset;    // slot i: data + i * frame_stride + wire_offset
    const uint8_t * wire_bytes; size_t wire_bytes_stride;       // slot i's int byte count; 0 is aborted
};

// the receive thread fills one buffer while update processes the other --
// the IO, carried over whole

struct rocketnet_client_receive_buffer_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_NUM_CLIENT_PACKETS];
    double receive_time[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    size_t packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_NUM_CLIENT_PACKETS];
};

struct rocketnet_client_io_t
{
    const rocketnet_client_driver_t * driver;
    rocketnet_client_io_ring_view_t ring_view[2];
    rocketnet_address_t to[2];                  // each ring's destination, written before its hand-off
    rocketnet_client_send_rings_t rings;
    rocketnet_client_receive_handshake_t receive;
    rocketnet_client_receive_buffer_t * buffer[ROCKETNET_CLIENT_IO_NUM_BUFFERS];   // as many as the fixed design's receive handoff uses
    int fill_slots;                             // data slots used in the I/O thread's buffer
    double (*time)( void * context ); void * time_context;
    double guard_timeout_seconds;               // ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS
    std::atomic<bool> quit;
};

// RNClientIo initial; main thread. Takes the two ring views and the two
// receive buffers the transport owns.
inline void rocketnet_client_io_init( rocketnet_client_io_t * io, const rocketnet_client_driver_t * driver,
    const rocketnet_client_io_ring_view_t * view0, const rocketnet_client_io_ring_view_t * view1,
    rocketnet_client_receive_buffer_t * const * buffers,
    double (*time)( void * context ), void * time_context )
{
    io->driver = driver;
    io->ring_view[0] = *view0;
    io->ring_view[1] = *view1;
    memset( io->to, 0, sizeof( io->to ) );
    rocketnet_client_send_rings_init( &io->rings );
    rocketnet_client_receive_handshake_init( &io->receive );
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
    {
        io->buffer[i] = buffers[i];
        io->buffer[i]->num_packets = 0;
    }
    io->fill_slots = 0;
    io->time = time;
    io->time_context = time_context;
    io->guard_timeout_seconds = ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS;
    io->quit.store( false, std::memory_order_relaxed );
}

// RNClientIo Stop; main thread. Stores quit, then wakes the I/O thread
// (docs/CLIENT-IO.md _Stop and destroy order_). The I/O thread finishes the
// ring in flight and exits; the main thread joins, closes the driver, then
// frees.
inline void rocketnet_client_io_stop( rocketnet_client_io_t * io )
{
    io->quit.store( true, std::memory_order_release );
    io->driver->operations->wake( io->driver->instance );
}

// RNClientIo HandOff and Defer; main thread. A LATE result is the deferred
// hand-off of docs/CLIENT-IO.md _Reuse only after sent_: the committed slots
// stay in the Filling ring and leave with the next hand-off. This function
// never waits, spins or retries, and wakes the I/O thread only on HANDED_OFF.
inline int rocketnet_client_io_hand_off( rocketnet_client_io_t * io, uint64_t committed, const rocketnet_address_t * to )
{
    rocketnet_assert( !io->quit.load( std::memory_order_relaxed ) );
    io->to[io->rings.fill] = *to;
    const int result = rocketnet_client_send_rings_hand_off( &io->rings, committed );
    if ( result == ROCKETNET_CLIENT_RINGS_HANDED_OFF )
        io->driver->operations->wake( io->driver->instance );
    return result;
}

// RNClientIo Expose; main thread, at update's start. Returns the Ready buffer
// and holds it Exposed, or NULL when none is Ready.
inline rocketnet_client_receive_buffer_t * rocketnet_client_io_expose( rocketnet_client_io_t * io )
{
    const int index = rocketnet_client_receive_handshake_update_expose( &io->receive );
    return index >= 0 ? io->buffer[index] : NULL;
}

// RNClientIo Return; main thread, at update's end. Returns the exposed buffer
// Free and wakes the I/O thread when it has an owed batch.
inline void rocketnet_client_io_return( rocketnet_client_io_t * io )
{
    if ( rocketnet_client_receive_handshake_update_return( &io->receive ) )
        io->driver->operations->wake( io->driver->instance );
}

// The I/O thread's loop body (RNClientIo IoSend, IoFinishRing, IoReceive,
// IoPublish, IoOwe, IoWait; docs/CLIENT-IO.md _The I/O step_ and _The wake_).
inline int rocketnet_client_io_step( rocketnet_client_io_t * io, bool block )
{
    rocketnet_assert( io );

    // RNClientIo Stop: the main thread stores quit and wakes. Read it first;
    // a step that sees it stops once the ring in flight is finished.
    const bool quitting = io->quit.load( std::memory_order_acquire );

    bool worked = false;

    // 1. RNClientIo IoSend / IoFinishRing: send a handed-off ring. Slot i is
    // at data + i * frame_stride + wire_offset; a zero int byte count is an
    // aborted slot and stays unsent.
    const int ring = rocketnet_client_send_rings_take( &io->rings );
    if ( ring >= 0 )
    {
        const rocketnet_client_io_ring_view_t * view = &io->ring_view[ring];
        const uint64_t committed = io->rings.committed[ring];
        if ( io->driver->operations->send_batch )
        {
            rocketnet_platform_send_t sends[ROCKETNET_CLIENT_IO_BATCH];
            int num_sends = 0;
            for ( uint64_t slot = 0; slot < committed; slot++ )
            {
                int bytes = 0;
                memcpy( &bytes, view->wire_bytes + (size_t) slot * view->wire_bytes_stride, sizeof( bytes ) );
                if ( bytes == 0 )
                    continue;
                sends[num_sends].to = &io->to[ring];
                sends[num_sends].packet_data = view->data + (size_t) slot * view->frame_stride + view->wire_offset;
                sends[num_sends].packet_bytes = bytes;
                num_sends++;
                if ( num_sends == ROCKETNET_CLIENT_IO_BATCH )
                {
                    io->driver->operations->send_batch( io->driver->instance, sends, num_sends );
                    num_sends = 0;
                }
            }
            if ( num_sends > 0 )
                io->driver->operations->send_batch( io->driver->instance, sends, num_sends );
        }
        else
        {
            for ( uint64_t slot = 0; slot < committed; slot++ )
            {
                int bytes = 0;
                memcpy( &bytes, view->wire_bytes + (size_t) slot * view->wire_bytes_stride, sizeof( bytes ) );
                if ( bytes == 0 )
                    continue;
                const uint8_t * packet_data = view->data + (size_t) slot * view->frame_stride + view->wire_offset;
                io->driver->operations->send( io->driver->instance, &io->to[ring], packet_data, bytes );
            }
        }
        rocketnet_client_send_rings_finish( &io->rings, ring );
        worked = true;
    }

    // RNClientIo IoExit: once quit is stored, the ring handed off before stop
    // is sent first (the send phase above); when nothing is in flight the
    // thread exits at once: no receive, no publish, no wait.
    if ( quitting && !rocketnet_client_send_rings_in_flight( &io->rings ) )
        return ROCKETNET_CLIENT_IO_EXITED;

    // 2. RNClientIo IoReceive: drain into the buffer the handshake gives the
    // I/O thread. A received datagram is worked even when the size prefilter
    // skips it; its data slot stays used. One clock read per driver call.
    rocketnet_client_receive_handshake_t * handshake = &io->receive;
    rocketnet_client_receive_buffer_t * buffer = io->buffer[handshake->fill];

    while ( io->fill_slots < ROCKETNET_NUM_CLIENT_PACKETS )
    {
        if ( io->driver->operations->receive_batch )
        {
            const int room = ROCKETNET_NUM_CLIENT_PACKETS - io->fill_slots;
            const int max_packets = room < ROCKETNET_CLIENT_IO_BATCH ? room : ROCKETNET_CLIENT_IO_BATCH;
            rocketnet_address_t from[ROCKETNET_CLIENT_IO_BATCH];
            int packet_bytes[ROCKETNET_CLIENT_IO_BATCH];
            const double now = io->time( io->time_context );
            uint8_t * packet_data = buffer->data + (size_t) io->fill_slots * ROCKETNET_FRAME_SIZE;
            const int got = io->driver->operations->receive_batch( io->driver->instance, from, packet_data,
                ROCKETNET_FRAME_SIZE, packet_bytes, max_packets );
            if ( got <= 0 )
                break;
            for ( int i = 0; i < got; i++ )
            {
                if ( packet_bytes[i] < ROCKETNET_PREFILTER_BYTES || packet_bytes[i] > ROCKETNET_RECEIVE_CEILING )
                    continue;
                (void) rocketnet_client_receive_handshake_io_receive( handshake );
                const int index = buffer->num_packets;
                buffer->from[index] = from[i];
                buffer->receive_time[index] = now;
                buffer->packet_data[index] = buffer->data + (size_t) ( io->fill_slots + i ) * ROCKETNET_FRAME_SIZE;
                buffer->packet_bytes[index] = (size_t) packet_bytes[i];
                buffer->num_packets++;
            }
            io->fill_slots += got;
            worked = true;
        }
        else
        {
            rocketnet_address_t from;
            const double now = io->time( io->time_context );
            uint8_t * packet_data = buffer->data + (size_t) io->fill_slots * ROCKETNET_FRAME_SIZE;
            const int bytes = io->driver->operations->receive( io->driver->instance, &from, packet_data,
                ROCKETNET_MAX_PACKET_BYTES );
            if ( bytes == 0 )
                break;
            if ( bytes >= ROCKETNET_PREFILTER_BYTES && bytes <= ROCKETNET_RECEIVE_CEILING )
            {
                (void) rocketnet_client_receive_handshake_io_receive( handshake );
                const int index = buffer->num_packets;
                buffer->from[index] = from;
                buffer->receive_time[index] = now;
                buffer->packet_data[index] = packet_data;
                buffer->packet_bytes[index] = (size_t) bytes;
                buffer->num_packets++;
            }
            io->fill_slots++;
            worked = true;
        }
    }

    // 3. RNClientIo IoPublish / IoOwe: publish the filled buffer when a Free
    // one is there to fill next; otherwise store the publish as owed and keep
    // the batch while the buffer has room. A publish is worked.
    if ( rocketnet_client_receive_handshake_io_publish( handshake ) )
    {
        io->buffer[handshake->fill]->num_packets = 0;
        io->fill_slots = 0;
        worked = true;
    }
    else if ( !rocketnet_client_receive_handshake_io_owe( handshake ) )
    {
        if ( rocketnet_client_receive_handshake_io_publish( handshake ) )
        {
            io->buffer[handshake->fill]->num_packets = 0;
            io->fill_slots = 0;
            worked = true;
        }
    }

    // 4. RNClientIo IoWait / waitReadable: wait only when nothing was done.
    // readable is true only while the I/O thread's buffer has room. With a
    // full buffer this waits on the wake alone: a wait that also returned on
    // readable would return at once every time and the I/O thread would spin
    // (MCRNClientIoBrokenFullWaitReadable.cfg, FullWaitsOnWake). The owed flag
    // was stored above before this wait, so the return at the end of the next
    // update wakes it.
    if ( block && !worked && !quitting )
    {
        const bool readable = rocketnet_client_receive_handshake_io_wait( handshake );
        io->driver->operations->wait( io->driver->instance, readable, io->guard_timeout_seconds );
    }

    return worked ? ROCKETNET_CLIENT_IO_WORKED : ROCKETNET_CLIENT_IO_IDLE;
}

// RNClientIo IoExit; the I/O thread's body. Calls the step in a loop until it
// returns EXITED (docs/CLIENT-IO.md _Stop and destroy order_). The transport
// creates the thread with this function and joins it before closing the driver.
inline void rocketnet_client_io_thread_function( void * data )
{
    rocketnet_client_io_t * io = (rocketnet_client_io_t *) data;
    while ( rocketnet_client_io_step( io, true ) != ROCKETNET_CLIENT_IO_EXITED ) {}
}

// The transport's wiring of this core (sdk/rocketnet_client_transport.cpp);
// internal, not exported. True when the operations have both wait and wake:
// the transport then runs the I/O thread design through rocketnet_client_io_t,
// and otherwise the BASE path.
bool rocketnet_client_transport_uses_io_thread( const rocketnet_client_driver_operations_t * operations );

// Unit tests only: a client transport on the caller's driver operations (the
// driver's create receives driver_context) with no I/O thread. When the
// operations have wait and wake the caller runs rocketnet_client_io_step on
// rocketnet_client_transport_client_io in place of the thread, and destroy
// runs the step in place of the sleep and the join (docs/CLIENT-IO.md _Stop
// and destroy order_). Returns NULL on any create refusal.
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven( void * context, const uint8_t * connect_token,
    const struct rocketnet_client_transport_config_t * config, double time,
    const rocketnet_client_driver_operations_t * operations, void * driver_context );

// The transport's I/O core, or NULL on the BASE path.
rocketnet_client_io_t * rocketnet_client_transport_client_io( struct rocketnet_client_transport_t * client_transport );

#endif // #ifndef ROCKETNET_CLIENT_IO_H
