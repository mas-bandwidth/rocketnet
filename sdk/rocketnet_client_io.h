/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once
#include <stdio.h>

#ifndef ROCKETNET_CLIENT_IO_H
#define ROCKETNET_CLIENT_IO_H

#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rocketnet_internal.h"
#include "rocketnet_constants.h"
#include "rocketnet_driver.h"
#include "rocketnet_platform.h"
#include "rocketnet_client_send_rings.h"
#include "rocketnet_client_receive_handshake.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_replay.h"
#include "rocketnet_keys.h"
#include "rocketnet_wire.h"
#include "rocketnet_client_protocol.h"

// The secret wipe of the I/O thread's open copy: sodium_memzero, the
// repository's helper, behind a function of sdk/rocketnet_client_transport.cpp
// so the header needs no sodium include.
void rocketnet_client_io_wipe( void * data, size_t bytes );

// The client I/O step of docs/CLIENT-IO.md _The I/O step_ and _The wake_, and
// the RNClientIo actions it names. The thread loop is one call per iteration;
// unit tests call it directly, with a fake transport and a test clock, on the
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

// Each record carries the I/O thread's verdict (ROCKETNET_CLIENT_IO_VERDICT_*,
// sdk/rocketnet_client_protocol.h) and, when opened, the reconstructed 64-bit
// session sequence (docs/CLIENT-IO-DECRYPT.md, If decrypt moves). An opened
// record keeps its wire header and its wire byte count; the plaintext replaces
// the ciphertext behind the header.

struct rocketnet_client_receive_buffer_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_NUM_CLIENT_PACKETS];
    double receive_time[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t * packet_data[ROCKETNET_NUM_CLIENT_PACKETS];
    size_t packet_bytes[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t verdict[ROCKETNET_NUM_CLIENT_PACKETS];
    uint64_t sequence[ROCKETNET_NUM_CLIENT_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_NUM_CLIENT_PACKETS];
};

// The I/O thread's open block (docs/CLIENT-IO-DECRYPT.md): an I/O-owned copy
// of the session open key, protocol id, connection salt and filter value, an
// open epoch (0 none, odd live generation, a retired value), and the I/O
// thread's own replay window (tla/RNClientIoDecrypt.tla epoch, ioKeys, ioWin).
// The main thread writes the copy then stores the epoch with release
// (PublishKeys); to retire it stores retired with seq_cst before zeroing its
// own keys (RetireKeys). The I/O thread loads the epoch with acquire at the top
// of each drain (IoTakeKeys, IoDropKeys) and never reads client->keys.
#define ROCKETNET_CLIENT_IO_EPOCH_NONE     0
#define ROCKETNET_CLIENT_IO_EPOCH_LIVE     1u            // the one live generation of a transport's one attempt
#define ROCKETNET_CLIENT_IO_EPOCH_RETIRED  0xFFFFFFFFu

struct rocketnet_client_io_open_t
{
    uint8_t session_server_to_client[ROCKETNET_KEY_BYTES];
    uint64_t protocol_id;
    uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES];
    rocketnet_address_t server_address;     // only the dialed server's datagrams are opened
    std::atomic<uint32_t> epoch;
    struct rocketnet_replay_protection_t replay;
    uint32_t loaded_epoch;          // the epoch the I/O thread last took; I/O thread only
};

struct rocketnet_client_io_t
{
    const rocketnet_client_transport_io_t * transport;
    rocketnet_client_io_ring_view_t ring_view[2];
    rocketnet_address_t to[2];                  // each ring's destination, bound at its first hand-off attempt
    bool to_held[2];                            // main thread only: the Filling ring's destination is bound (a deferred hand-off)
    rocketnet_client_send_rings_t rings;
    rocketnet_client_receive_handshake_t receive;
    rocketnet_client_receive_buffer_t * buffer[ROCKETNET_CLIENT_IO_NUM_BUFFERS];   // as many as the fixed design's receive handoff uses
    int fill_slots;                             // data slots used in the I/O thread's buffer
    double (*time)( void * context ); void * time_context;
    double guard_timeout_seconds;               // ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS
    std::atomic<bool> quit;
    std::atomic<bool> stopped; // released only after the final core access
    struct rocketnet_client_io_open_t open;
    uint64_t transport_faults;                     // a receive count or byte count this step refused
    uint64_t * prefilter_too_large;             // optional: counts PREFILTER_TOO_LARGE for a datagram rejected by the byte-count check
    uint64_t diag_steps, diag_rings, diag_sent, diag_received, diag_full, diag_empty; double diag_last_rx, diag_last_ring; int diag_reported;
};

// RNClientIo initial; main thread. Takes the two ring views and the two
// receive buffers the transport owns.
inline void rocketnet_client_io_init( rocketnet_client_io_t * io, const rocketnet_client_transport_io_t * transport,
    const rocketnet_client_io_ring_view_t * view0, const rocketnet_client_io_ring_view_t * view1,
    rocketnet_client_receive_buffer_t * const * buffers,
    double (*time)( void * context ), void * time_context )
{
    io->transport = transport;
    io->ring_view[0] = *view0;
    io->ring_view[1] = *view1;
    memset( io->to, 0, sizeof( io->to ) );
    io->to_held[0] = false;
    io->to_held[1] = false;
    rocketnet_client_send_rings_init( &io->rings );
    rocketnet_client_receive_handshake_init( &io->receive );
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
    {
        io->buffer[i] = buffers[i];
        io->buffer[i]->num_packets = 0;
    }
    io->fill_slots = 0;
    io->diag_steps = io->diag_rings = io->diag_sent = io->diag_received = io->diag_full = io->diag_empty = 0; io->diag_last_rx = 0.0; io->diag_last_ring = 0.0; io->diag_reported = 0;
    io->time = time;
    io->time_context = time_context;
    io->guard_timeout_seconds = ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS;
    io->quit.store( false, std::memory_order_relaxed );
    io->stopped.store( false, std::memory_order_relaxed );
    memset( io->open.session_server_to_client, 0, sizeof( io->open.session_server_to_client ) );
    io->open.protocol_id = 0;
    memset( io->open.connection_salt, 0, sizeof( io->open.connection_salt ) );
    memset( io->open.packet_filter, 0, sizeof( io->open.packet_filter ) );
    io->open.epoch.store( ROCKETNET_CLIENT_IO_EPOCH_NONE, std::memory_order_relaxed );
    memset( &io->open.replay, 0, sizeof( io->open.replay ) );
    io->open.loaded_epoch = ROCKETNET_CLIENT_IO_EPOCH_NONE;
    io->transport_faults = 0;
    io->prefilter_too_large = NULL;
}

// tla/RNClientIoDecrypt.tla PublishKeys; main thread, where the challenge
// derives the session keys. Writes the I/O-owned open copy, then stores the
// live epoch with release; the I/O thread takes the copy at the top of its next
// drain. One attempt per transport: a publish once the epoch has left none is
// refused, so the main thread never writes the copy while the I/O thread may
// read it.
inline void rocketnet_client_io_publish_keys( rocketnet_client_io_t * io, const uint8_t * session_server_to_client,
    uint64_t protocol_id, const uint8_t * connection_salt, const uint8_t * packet_filter,
    const rocketnet_address_t * server_address )
{
    if ( io->open.epoch.load( std::memory_order_relaxed ) != ROCKETNET_CLIENT_IO_EPOCH_NONE )
        return;
    memcpy( io->open.session_server_to_client, session_server_to_client, ROCKETNET_KEY_BYTES );
    io->open.protocol_id = protocol_id;
    memcpy( io->open.connection_salt, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( io->open.packet_filter, packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
    io->open.server_address = *server_address;
    io->open.epoch.store( ROCKETNET_CLIENT_IO_EPOCH_LIVE, std::memory_order_release );
}

// tla/RNClientIoDecrypt.tla RetireKeys; main thread, at the integrity limit,
// disconnect and destroy, before the main thread zeroes its own keys. Stores
// retired with seq_cst and wakes the I/O thread, so a blocked I/O thread reads
// the epoch and drops its copy (README-RNClientIoDecrypt.md, Findings). The
// copy is the I/O thread's to zero; the main thread never writes it here.
inline void rocketnet_client_io_retire_keys( rocketnet_client_io_t * io )
{
    if ( io->open.epoch.load( std::memory_order_relaxed ) == ROCKETNET_CLIENT_IO_EPOCH_RETIRED )
        return;
    io->open.epoch.store( ROCKETNET_CLIENT_IO_EPOCH_RETIRED, std::memory_order_seq_cst );
    if ( io->transport->operations->wake )
        io->transport->operations->wake( io->transport->instance );
}

// tla/RNClientIoDecrypt.tla IoDropKeys; I/O thread. Zeroes the copy and the
// window; nothing opens after.
inline void rocketnet_client_io_drop_keys( rocketnet_client_io_t * io )
{
    rocketnet_client_io_wipe( io->open.session_server_to_client, sizeof( io->open.session_server_to_client ) );
    io->open.protocol_id = 0;
    rocketnet_client_io_wipe( io->open.connection_salt, sizeof( io->open.connection_salt ) );
    rocketnet_client_io_wipe( io->open.packet_filter, sizeof( io->open.packet_filter ) );
    memset( &io->open.server_address, 0, sizeof( io->open.server_address ) );
    memset( &io->open.replay, 0, sizeof( io->open.replay ) );
}

// tla/RNClientIoDecrypt.tla IoTakeKeys, IoDropKeys and IoKeepKeys; I/O thread,
// at the top of each drain. One acquire load of the epoch: a new live value
// takes the copy and resets the I/O thread's window (IoTakeKeys), retired
// zeroes the copy (IoDropKeys), no change keeps it (IoKeepKeys).
inline void rocketnet_client_io_take_keys( rocketnet_client_io_t * io )
{
    const uint32_t epoch = io->open.epoch.load( std::memory_order_acquire );
    if ( epoch == io->open.loaded_epoch )
        return;
    io->open.loaded_epoch = epoch;
    if ( epoch == ROCKETNET_CLIENT_IO_EPOCH_RETIRED )
        rocketnet_client_io_drop_keys( io );
    else if ( epoch != ROCKETNET_CLIENT_IO_EPOCH_NONE )
        memset( &io->open.replay, 0, sizeof( io->open.replay ) );
}

// tla/RNClientIoDecrypt.tla IoCheck, the wire floor rung: the per-type size
// check the main thread runs first (sdk/rocketnet_wire.h); 0 for a type that
// is not a session type.
inline int rocketnet_client_io_session_floor( int type )
{
    switch ( type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET:    return ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:       return ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET:    return ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_ACK_PACKET:           return ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:      return ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES;
        default:                                        return 0;
    }
}

// tla/RNClientIoDecrypt.tla IoCheck then IoOpen; I/O thread, per datagram,
// before IoPublish. A datagram that is not a session packet, one from any
// source but the dialed server (update drops it as junk before the protocol,
// so it must not mark the I/O thread's window), or any datagram while the copy
// is not live, passes with the raw verdict. Otherwise IoCheck
// runs the cheap rungs that need no protocol state (the prefilter, the type's
// wire floor, the zero sequence) and the I/O thread's own window's
// rocketnet_replay_already_received last; then IoOpen runs
// rocketnet_open_sealed_packet with the copy and, on success,
// rocketnet_replay_advance on the I/O thread's own window, and the plaintext
// replaces the ciphertext behind the wire header. The wire byte count is kept.
// The I/O thread writes no counter.
inline void rocketnet_client_io_decrypt_datagram( rocketnet_client_io_t * io, const rocketnet_address_t * from,
    uint8_t * packet_data, int packet_bytes, uint8_t * out_verdict, uint64_t * out_sequence )
{
    *out_verdict = ROCKETNET_CLIENT_IO_VERDICT_RAW;
    *out_sequence = 0;
    if ( io->open.loaded_epoch == ROCKETNET_CLIENT_IO_EPOCH_NONE ||
         io->open.loaded_epoch == ROCKETNET_CLIENT_IO_EPOCH_RETIRED )
        return;
    if ( !rocketnet_address_equal( from, &io->open.server_address ) )
        return;
    const int type = rocketnet_prefilter_check( packet_data, packet_bytes, io->open.packet_filter );
    const int floor = rocketnet_client_io_session_floor( type );
    if ( floor == 0 )
        return;
    if ( packet_bytes < floor )
    {
        *out_verdict = ROCKETNET_CLIENT_IO_VERDICT_WRONG_SIZE;
        return;
    }
    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), io->open.replay.highest_opened );
    if ( sequence == 0 )
    {
        *out_verdict = ROCKETNET_CLIENT_IO_VERDICT_ZERO_SEQUENCE;
        return;
    }
    if ( rocketnet_replay_already_received( &io->open.replay, sequence ) )
    {
        *out_verdict = ROCKETNET_CLIENT_IO_VERDICT_ALREADY_RECEIVED;
        return;
    }
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet_data, packet_bytes,
        (uint8_t) type, sequence, io->open.protocol_id, io->open.connection_salt, NULL,
        io->open.session_server_to_client );
    if ( opened_bytes < 0 )
    {
        *out_verdict = ROCKETNET_CLIENT_IO_VERDICT_DID_NOT_OPEN;
        return;
    }
    rocketnet_assert( opened_bytes == packet_bytes - ROCKETNET_PACKET_OVERHEAD_BYTES );
    memcpy( packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, opened, (size_t) opened_bytes );
    rocketnet_client_io_wipe( opened, (size_t) opened_bytes );
    rocketnet_replay_advance( &io->open.replay, sequence );
    *out_verdict = ROCKETNET_CLIENT_IO_VERDICT_OPENED;
    *out_sequence = sequence;
}

// RNClientIo Stop; main thread. Stores quit, then wakes the I/O thread
// (docs/CLIENT-IO.md _Stop and destroy order_). The I/O thread finishes the
// ring in flight and exits; the main thread joins, closes the transport, then
// frees.
inline void rocketnet_client_io_stop( rocketnet_client_io_t * io )
{
    io->quit.store( true, std::memory_order_release );
    if ( io->transport->operations->wake )
        io->transport->operations->wake( io->transport->instance );
}

// RNClientIo HandOff and Defer; main thread. A LATE result is the deferred
// hand-off of docs/CLIENT-IO.md _Reuse only after sent_: the committed slots
// stay in the Filling ring and leave with the next hand-off. This function
// never waits, spins or retries, and wakes the I/O thread only on HANDED_OFF.
// The first attempt that finds committed slots binds the Filling ring's
// destination to `to`; a later attempt for the same ring (after a LATE)
// leaves it bound, so the slots leave for the address they were committed
// with even when `to` has changed since, as the server address has when
// disconnect has reset the protocol (RNClientIo SentToCommitted). A
// HANDED_OFF result unbinds the ring for its next fill.
inline int rocketnet_client_io_hand_off( rocketnet_client_io_t * io, uint64_t committed, const rocketnet_address_t * to )
{
    const bool quitting = io->quit.load( std::memory_order_acquire );
    rocketnet_assert( !quitting );
    if ( quitting )
        return ROCKETNET_CLIENT_RINGS_EMPTY;
    const int ring = io->rings.fill;
    if ( committed > 0 && !io->to_held[ring] )
    {
        io->to[ring] = *to;
        io->to_held[ring] = true;
    }
    const int result = rocketnet_client_send_rings_hand_off( &io->rings, committed );
    if ( result == ROCKETNET_CLIENT_RINGS_HANDED_OFF )
        io->to_held[ring] = false;
    if ( result == ROCKETNET_CLIENT_RINGS_HANDED_OFF && io->transport->operations->wake )
        io->transport->operations->wake( io->transport->instance );
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
    if ( rocketnet_client_receive_handshake_update_return( &io->receive ) && io->transport->operations->wake )
        io->transport->operations->wake( io->transport->instance );
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
    io->diag_steps++;
    {
        const double dnow = io->time( io->time_context );
        if ( io->diag_last_rx == 0.0 ) io->diag_last_rx = dnow;
        const double gap = dnow - io->diag_last_rx;
        if ( ( gap > 2.0 && io->diag_reported == 0 ) || ( gap > 6.0 && io->diag_reported == 1 ) )
        {
            io->diag_reported++;
            fprintf( stderr, "client_io_diag: t=%.3f io=%p gap=%.3f steps=%llu rings=%llu since_ring=%.3f sent=%llu received=%llu full=%llu empty=%llu fill_slots=%d fill=%d\n",
                dnow, (void*) io, gap, (unsigned long long) io->diag_steps, (unsigned long long) io->diag_rings, dnow - io->diag_last_ring,
                (unsigned long long) io->diag_sent, (unsigned long long) io->diag_received, (unsigned long long) io->diag_full, (unsigned long long) io->diag_empty,
                io->fill_slots, (int) io->receive.fill );
            fflush( stderr );
        }
    }

    // 1. RNClientIo IoSend / IoFinishRing: send a handed-off ring. Slot i is
    // at data + i * frame_stride + wire_offset; a zero int byte count is an
    // aborted slot and stays unsent.
    const int ring = rocketnet_client_send_rings_take( &io->rings );
    if ( ring >= 0 )
    {
        io->diag_rings++; io->diag_last_ring = io->time( io->time_context );
        const rocketnet_client_io_ring_view_t * view = &io->ring_view[ring];
        const uint64_t committed = io->rings.committed[ring];
        if ( io->transport->operations->send_batch )
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
                    io->transport->operations->send_batch( io->transport->instance, sends, num_sends );
                    num_sends = 0;
                }
            }
            if ( num_sends > 0 )
                io->transport->operations->send_batch( io->transport->instance, sends, num_sends );
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
                io->transport->operations->send( io->transport->instance, &io->to[ring], packet_data, bytes );
            }
        }
        rocketnet_client_send_rings_finish( &io->rings, ring );
        worked = true;
    }

    // RNClientIo IoExit: once quit is stored, the ring handed off before stop
    // is sent first (the send phase above); when nothing is in flight the
    // thread exits at once: no receive, no publish, no wait.
    if ( quitting && !rocketnet_client_send_rings_in_flight( &io->rings ) )
    {
        rocketnet_client_io_drop_keys( io );
        io->stopped.store( true, std::memory_order_release );
        return ROCKETNET_CLIENT_IO_EXITED;
    }

    // 2. RNClientIo IoReceive: drain into the buffer the handshake gives the
    // I/O thread. Every received datagram becomes a record, a runt or an
    // oversize one too: the main thread's prefilter
    // (sdk/rocketnet_client_protocol.cpp:1248, sizes checked at
    // sdk/rocketnet_protocol.cpp:176-180) refuses it and counts
    // PREFILTER_TOO_SHORT or PREFILTER_TOO_LARGE. One clock read per transport
    // call.
    rocketnet_client_receive_handshake_t * handshake = &io->receive;
    rocketnet_client_receive_buffer_t * buffer = io->buffer[handshake->fill];

    // tla/RNClientIoDecrypt.tla IoTakeKeys, IoDropKeys, IoKeepKeys: the epoch
    // is read once at the top of the drain. The keys are published before the
    // connection response is handed off, and this read comes before the drain
    // of the step that sends it, so every reply to the response is received
    // after the copy is taken.
    rocketnet_client_io_take_keys( io );

    while ( io->fill_slots < ROCKETNET_NUM_CLIENT_PACKETS )
    {
        if ( io->transport->operations->receive_batch )
        {
            const int room = ROCKETNET_NUM_CLIENT_PACKETS - io->fill_slots;
            const int max_packets = room < ROCKETNET_CLIENT_IO_BATCH ? room : ROCKETNET_CLIENT_IO_BATCH;
            rocketnet_address_t from[ROCKETNET_CLIENT_IO_BATCH];
            int packet_bytes[ROCKETNET_CLIENT_IO_BATCH];
            const double now = io->time( io->time_context );
            uint8_t * packet_data = buffer->data + (size_t) io->fill_slots * ROCKETNET_FRAME_SIZE;
            const int got = io->transport->operations->receive_batch( io->transport->instance, from, packet_data,
                ROCKETNET_FRAME_SIZE, packet_bytes, max_packets );
            // a count past the slots asked for, or a byte count that is
            // negative or beyond the UDP envelope, is a transport fault and is not stored
            if ( got < 0 )
            {
                io->transport_faults++;
                break;
            }
            if ( got == 0 )
                break;
            int count = got;
            if ( got > max_packets )
            {
                io->transport_faults++;
                count = max_packets;
            }
            int accepted = 0;
            for ( int i = 0; i < count; i++ )
            {
                if ( packet_bytes[i] < 0 || packet_bytes[i] > ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES )
                {
                    io->transport_faults++;
                    if ( io->prefilter_too_large && packet_bytes[i] > ROCKETNET_FRAME_SIZE )
                        (*io->prefilter_too_large)++;
                    continue;
                }
                if ( accepted != i )
                    memmove( packet_data + (size_t) accepted * ROCKETNET_FRAME_SIZE,
                        packet_data + (size_t) i * ROCKETNET_FRAME_SIZE,
                        (size_t) ( packet_bytes[i] < ROCKETNET_FRAME_SIZE ? packet_bytes[i] : ROCKETNET_FRAME_SIZE ) );
                (void) rocketnet_client_receive_handshake_io_receive( handshake );
                io->diag_received++; io->diag_last_rx = now; io->diag_reported = 0;
                const int index = buffer->num_packets;
                buffer->from[index] = from[i];
                buffer->receive_time[index] = now;
                buffer->packet_data[index] = buffer->data + (size_t) ( io->fill_slots + accepted ) * ROCKETNET_FRAME_SIZE;
                buffer->packet_bytes[index] = (size_t) packet_bytes[i];
                rocketnet_client_io_decrypt_datagram( io, &buffer->from[index], buffer->packet_data[index],
                    (int) buffer->packet_bytes[index], &buffer->verdict[index], &buffer->sequence[index] );
                buffer->num_packets++;
                accepted++;
            }
            if ( accepted == 0 )
                break;
            io->fill_slots += accepted;
            worked = true;
        }
        else
        {
            rocketnet_address_t from;
            const double now = io->time( io->time_context );
            uint8_t * packet_data = buffer->data + (size_t) io->fill_slots * ROCKETNET_FRAME_SIZE;
            const int bytes = io->transport->operations->receive( io->transport->instance, &from, packet_data,
                ROCKETNET_RECEIVE_PACKET_BYTES );
            if ( bytes == 0 )
                break;
            if ( bytes < 0 || bytes > ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES )
            {
                io->transport_faults++;
                if ( io->prefilter_too_large && bytes > ROCKETNET_RECEIVE_PACKET_BYTES )
                    (*io->prefilter_too_large)++;
                break;
            }
            (void) rocketnet_client_receive_handshake_io_receive( handshake );
            io->diag_received++; io->diag_last_rx = now; io->diag_reported = 0;
            const int index = buffer->num_packets;
            buffer->from[index] = from;
            buffer->receive_time[index] = now;
            buffer->packet_data[index] = packet_data;
            buffer->packet_bytes[index] = (size_t) bytes;
            rocketnet_client_io_decrypt_datagram( io, &buffer->from[index], buffer->packet_data[index],
                (int) buffer->packet_bytes[index], &buffer->verdict[index], &buffer->sequence[index] );
            buffer->num_packets++;
            io->fill_slots++;
            worked = true;
        }
    }

    if ( io->fill_slots >= ROCKETNET_NUM_CLIENT_PACKETS ) io->diag_full++;
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
    if ( block && !worked && !quitting && io->transport->operations->wait )
    {
        const bool readable = rocketnet_client_receive_handshake_io_wait( handshake );
        io->transport->operations->wait( io->transport->instance, readable, io->guard_timeout_seconds );
    }

    return worked ? ROCKETNET_CLIENT_IO_WORKED : ROCKETNET_CLIENT_IO_IDLE;
}

// RNClientIo IoExit; the I/O thread's body. Calls the step in a loop until it
// returns EXITED (docs/CLIENT-IO.md _Stop and destroy order_). The transport
// creates the thread with this function and joins it before closing the transport.
inline void rocketnet_client_io_thread_function( void * data )
{
    rocketnet_client_io_t * io = (rocketnet_client_io_t *) data;
    while ( rocketnet_client_io_step( io, true ) != ROCKETNET_CLIENT_IO_EXITED ) {}
}

// The transport's wiring of this core (sdk/rocketnet_client_transport.cpp);
// internal, not exported. True when the operations have both wait and wake:
// the transport then runs the I/O thread design through rocketnet_client_io_t,
// and otherwise the BASE path.
bool rocketnet_client_transport_uses_io_thread( const rocketnet_client_transport_io_operations_t * operations );

// Unit tests only: a client transport on the caller's transport operations (the
// transport's create receives transport_context) with no I/O thread. When the
// operations have wait and wake the caller runs rocketnet_client_io_step on
// rocketnet_client_transport_client_io in place of the thread. After stop,
// explicit steps or transport_update advance completion; stopped only polls,
// and destroy requires completion. Returns NULL on any create refusal.
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven( void * context, const uint8_t * connect_token,
    const struct rocketnet_client_transport_config_t * config, double time,
    const rocketnet_client_transport_io_operations_t * operations, void * transport_context );

// Unit tests only: as create_step_driven, with the Unix timestamp supplied by
// the test so token expiry is deterministic and no wall clock is read.
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_at_timestamp( void * context, const uint8_t * connect_token,
    const struct rocketnet_client_transport_config_t * config, double time, uint64_t current_timestamp,
    const rocketnet_client_transport_io_operations_t * operations, void * transport_context );

// As the step-driven create above, and when status is not NULL it is written
// with ROCKETNET_STATUS_OK or the refusal's section-2.1 code (a config whose
// step_driven field asks for a transport with no non-blocking step is refused).
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_ex( void * context, const uint8_t * connect_token,
    const struct rocketnet_client_transport_config_t * config, double time,
    const rocketnet_client_transport_io_operations_t * operations, void * transport_context, int * status );

// Unit tests only: execute one legacy receive-worker iteration on a transport
// created by the no-thread fixture, stamping receives with synthetic time.
// Never use concurrently with a real worker.
void rocketnet_client_transport_step_legacy_for_test( struct rocketnet_client_transport_t * client_transport, double time );

// The transport's I/O core, or NULL on the BASE path.
rocketnet_client_io_t * rocketnet_client_transport_client_io( struct rocketnet_client_transport_t * client_transport );

#endif // #ifndef ROCKETNET_CLIENT_IO_H
