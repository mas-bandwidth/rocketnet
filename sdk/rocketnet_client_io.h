/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CLIENT_IO_H
#define ROCKETNET_SDK_ROCKETNET_CLIENT_IO_H

#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rocketnet_internal.h"
#include "rocketnet_constants.h"
#include "rocketnet_driver.h"
#include "rocketnet_platform.h"
#include "rocketnet_buffer_handoff.h"
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
// The idle wait's timeout. The I/O thread wakes on its socket or its wake
// (proofs/tla/RNClientIo.tla IoWake needs no timeout); this only bounds a lost wake,
// so an idle client wakes at most 20 times a second. At 1 ms, 1000 clients on
// one host woke 340,000 times a second and starved threads for seconds.
#define ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS 0.05    // a guard, never a cadence

// The receive buffers: the I/O thread fills one, one waits ready, update
// holds one (the triple buffer of proofs/tla/RNBufferHandoff.tla, MaxReady 1).

#define ROCKETNET_CLIENT_IO_NUM_BUFFERS 3

// The send rings: game threads fill one, and up to two wait for or are sent
// by the I/O thread (proofs/tla/RNBufferHandoff.tla, MaxReady 2).

#define ROCKETNET_CLIENT_IO_NUM_RINGS 3

enum
{
    ROCKETNET_CLIENT_IO_IDLE = 0,
    ROCKETNET_CLIENT_IO_WORKED = 1,
    ROCKETNET_CLIENT_IO_EXITED = 2
};

// rocketnet_client_io_hand_off results: the ring went to the I/O thread, there
// was nothing to hand off, or the exchange was deferred (no ring free, or the
// mutex busy) and the committed slots stay in the Filling ring.

enum
{
    ROCKETNET_CLIENT_RINGS_HANDED_OFF = 0,
    ROCKETNET_CLIENT_RINGS_EMPTY = 1,
    ROCKETNET_CLIENT_RINGS_LATE = 2
};

struct rocketnet_client_io_ring_view_t
{
    uint8_t * data;
    size_t frame_stride;
    size_t wire_offset;    // slot i: data + i * frame_stride + wire_offset
    const uint8_t * wire_bytes;
    size_t wire_bytes_stride;       // slot i's int byte count; 0 is aborted
};

// the I/O thread fills one buffer while update processes another

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
    uint8_t data[ROCKETNET_FRAME_SIZE * ROCKETNET_NUM_CLIENT_PACKETS];
};

// The I/O thread's open block (docs/CLIENT-IO-DECRYPT.md): an I/O-owned copy
// of the session open key, protocol id, connection salt and filter value, an
// open epoch (0 none, odd live generation, a retired value), and the I/O
// thread's own replay window (proofs/tla/RNClientIoDecrypt.tla epoch, ioKeys, ioWin).
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

// Both handoffs are rocketnet_buffer_handoff_t (proofs/tla/RNBufferHandoff.tla):
// each ring and each receive buffer has one owner at a time, and only their
// indices cross threads, under the transport's mutex, which covers nothing
// else. The main thread only try-acquires it; the I/O thread acquires it.

struct rocketnet_client_io_t
{
    const rocketnet_client_transport_io_t * transport;
    rocketnet_platform_mutex_t * mutex;         // the handoff mutex: index exchanges only
    rocketnet_client_io_ring_view_t ring_view[ROCKETNET_CLIENT_IO_NUM_RINGS];

    // send: game threads fill send_fill; a published ring's committed count
    // and destination are written by the main thread before the exchange
    // that publishes it and read by the I/O thread after the one that takes it

    rocketnet_buffer_handoff_t<ROCKETNET_CLIENT_IO_NUM_RINGS> send;
    int send_fill;                              // main thread only: the ring game threads fill
    int send_ring;                              // I/O thread only: the ring it holds (sent), or -1
    int send_final;                             // the ring stop hands the I/O thread with quit, or -1
    uint64_t committed[ROCKETNET_CLIENT_IO_NUM_RINGS];
    rocketnet_address_t to[ROCKETNET_CLIENT_IO_NUM_RINGS];   // each ring's destination, bound at its first hand-off attempt
    bool to_held[ROCKETNET_CLIENT_IO_NUM_RINGS];             // main thread only: the Filling ring's destination is bound (a deferred hand-off)

    // receive: the I/O thread fills receive_fill; update holds receive_held

    rocketnet_buffer_handoff_t<ROCKETNET_CLIENT_IO_NUM_BUFFERS> receive;
    int receive_fill;                           // I/O thread only
    int receive_held;                           // main thread only, or -1
    rocketnet_client_receive_buffer_t * buffer[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    int fill_slots;                             // data slots used in the I/O thread's buffer
    double ( *time )( void * context );
    void * time_context;
    double guard_timeout_seconds;               // ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS
    std::atomic<bool> quit;
    std::atomic<bool> stopped; // released only after the final core access
    struct rocketnet_client_io_open_t open;
    uint64_t transport_faults;                     // a receive count or byte count this step refused
    std::atomic<uint64_t> receive_too_large;    // written by the I/O thread only: datagrams the byte-count check refused as too large
    uint64_t receive_too_large_folded;          // main thread only: the part of receive_too_large already added to prefilter_too_large
    uint64_t * prefilter_too_large;             // main thread only, optional: the PREFILTER_TOO_LARGE counter rocketnet_client_io_fold_counters adds to
};

// RNBufferHandoff Init; main thread. Takes the ring views and the receive
// buffers the transport owns, and the transport's handoff mutex. Ring 0 is
// the first Filling ring and buffer 0 the I/O thread's first buffer.

inline void rocketnet_client_io_init( rocketnet_client_io_t * io, const rocketnet_client_transport_io_t * transport,
                                      rocketnet_platform_mutex_t * mutex, const rocketnet_client_io_ring_view_t * views,
                                      rocketnet_client_receive_buffer_t * const * buffers,
                                      double ( *time )( void * context ), void * time_context )
{
    io->transport = transport;
    io->mutex = mutex;

    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        io->ring_view[i] = views[i];
        io->committed[i] = 0;
        io->to_held[i] = false;
    }

    memset( io->to, 0, sizeof( io->to ) );

    rocketnet_buffer_handoff_init( &io->send, 0 );

    io->send_fill = 0;
    io->send_ring = -1;
    io->send_final = -1;
    rocketnet_buffer_handoff_init( &io->receive, 0 );

    io->receive_fill = 0;
    io->receive_held = -1;

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

    io->stopped.store( false, std::memory_order_relaxed );

    memset( io->open.session_server_to_client, 0, sizeof( io->open.session_server_to_client ) );
    io->open.protocol_id = 0;
    memset( io->open.connection_salt, 0, sizeof( io->open.connection_salt ) );
    memset( io->open.packet_filter, 0, sizeof( io->open.packet_filter ) );

    io->open.epoch.store( ROCKETNET_CLIENT_IO_EPOCH_NONE, std::memory_order_relaxed );

    memset( &io->open.replay, 0, sizeof( io->open.replay ) );
    io->open.loaded_epoch = ROCKETNET_CLIENT_IO_EPOCH_NONE;
    io->transport_faults = 0;
    io->receive_too_large.store( 0, std::memory_order_relaxed );
    io->receive_too_large_folded = 0;
    io->prefilter_too_large = NULL;
}

// Main thread, at update after the expose. The I/O thread counts the
// datagrams its byte-count check refuses as too large in its own field; this
// adds the growth since the last call to the PREFILTER_TOO_LARGE counter,
// which the main thread's prefilter also counts into, so one oversize datagram
// reads PREFILTER_TOO_LARGE wherever it is refused.

inline void rocketnet_client_io_fold_counters( rocketnet_client_io_t * io )
{
    const uint64_t too_large = io->receive_too_large.load( std::memory_order_relaxed );

    if ( io->prefilter_too_large )
        *io->prefilter_too_large += too_large - io->receive_too_large_folded;

    io->receive_too_large_folded = too_large;
}

// proofs/tla/RNClientIoDecrypt.tla PublishKeys; main thread, where the challenge
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

// proofs/tla/RNClientIoDecrypt.tla RetireKeys; main thread, at the integrity limit,
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

// proofs/tla/RNClientIoDecrypt.tla IoDropKeys; I/O thread. Zeroes the copy and the
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

// proofs/tla/RNClientIoDecrypt.tla IoTakeKeys, IoDropKeys and IoKeepKeys; I/O thread,
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

// proofs/tla/RNClientIoDecrypt.tla IoCheck, the wire floor rung: the per-type size
// check the main thread runs first (sdk/rocketnet_wire.h); 0 for a type that
// is not a session type.

inline int rocketnet_client_io_session_floor( int type )
{
    switch ( type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET: return ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:    return ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET: return ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_ACK_PACKET:        return ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES;
        default:                                     return 0;
    }
}

// proofs/tla/RNClientIoDecrypt.tla IoCheck then IoOpen; I/O thread, per datagram,
// before the fill is published. A datagram that is not a session packet, one from any
// source but the dialed server (update drops it as junk before the protocol,
// so it must not mark the I/O thread's window), or any datagram while the copy
// is not live, passes with the raw verdict. Otherwise IoCheck
// runs the cheap rungs that need no protocol state (the prefilter, the type's
// wire floor, the zero sequence) and the I/O thread's own window's
// rocketnet_replay_already_received last; then IoOpen runs
// rocketnet_open_sealed_packet with the copy and, on success,
// rocketnet_replay_advance on the I/O thread's own window, and the plaintext
// replaces the ciphertext behind the wire header. The wire byte count is kept.
// The I/O thread writes no counter in the transport's counter block.

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

// One received datagram becomes the next record of the buffer in hand.

inline void rocketnet_client_io_fill_record( rocketnet_client_io_t * io, rocketnet_client_receive_buffer_t * buffer,
                                             const rocketnet_address_t * from, double now, uint8_t * packet_data, size_t packet_bytes )
{
    const int index = buffer->num_packets++;
    buffer->from[index] = *from;
    buffer->receive_time[index] = now;
    buffer->packet_data[index] = packet_data;
    buffer->packet_bytes[index] = packet_bytes;
    rocketnet_client_io_decrypt_datagram( io, &buffer->from[index], packet_data, (int) packet_bytes, &buffer->verdict[index], &buffer->sequence[index] );
}

// proofs/tla/RNClientIo.tla Stop: stores quit and may mark the filling ring final, and never waits.
// proofs/tla/RNBufferHandoff.tla MainStop: the same store ends the producer and wakes the worker.
// A filling ring with committed slots passes to the I/O thread with the quit store, so this
// takes no mutex and never waits: no producer touches it afterwards, and the
// I/O thread sends it after every ready ring, then exits. Then the wake.
// The main thread joins, closes the transport, then frees.

inline void rocketnet_client_io_stop( rocketnet_client_io_t * io, uint64_t committed, const rocketnet_address_t * to )
{
    const int ring = io->send_fill;

    if ( committed > 0 )
    {
        if ( !io->to_held[ring] )
            io->to[ring] = *to;

        io->committed[ring] = committed;
        io->send_final = ring;
    }

    io->quit.store( true, std::memory_order_release );

    if ( io->transport->operations->wake )
        io->transport->operations->wake( io->transport->instance );
}

// proofs/tla/RNClientIo.tla HandOffTo: publishes the filling ring and names the next filling ring.
// proofs/tla/RNClientIo.tla DeferTo: leaves the committed slots in the filling ring when the exchange cannot run.
// proofs/tla/RNBufferHandoff.tla ProducerExchange: try-acquires the mutex and swaps the producer index.
// A LATE result is the deferred hand-off of docs/CLIENT-IO.md _Reuse only
// after sent_: no ring is free, or the I/O thread holds the mutex this
// instant, so the committed slots stay in the Filling ring and leave with the
// next hand-off. This function never waits, spins or retries: it try-acquires
// the mutex for one index exchange, and wakes the I/O thread after releasing
// it, only on HANDED_OFF. The first attempt that finds committed slots binds
// the Filling ring's destination to `to`; a later attempt for the same ring
// (after a LATE) leaves it bound, so the slots leave for the address they were
// committed with even when `to` has changed since, as the server address has
// when disconnect has reset the protocol (RNClientIo SentToCommitted). The
// one rebinding is the family race's commit (rocketnet_client_io_rebind_held).
// On HANDED_OFF send_fill names the next Filling ring; the caller empties it.

// proofs/tla/RNClientRace.tla CallerCommit; main thread. A Filling ring whose
// destination a deferred hand-off bound to from is bound to to instead. The
// family race calls it at its commit with the losing and the kept server:
// the slots committed after the commit are the kept attempt's, and those
// committed before it were for both families while both were in flight, so
// every slot of the ring leaves for the kept server (RNClientRace
// PostCommitSlotsKept).

inline void rocketnet_client_io_rebind_held( rocketnet_client_io_t * io, const rocketnet_address_t * from, const rocketnet_address_t * to )
{
    const int ring = io->send_fill;
    if ( io->to_held[ring] && rocketnet_address_equal( &io->to[ring], from ) )
        io->to[ring] = *to;
}

inline int rocketnet_client_io_hand_off( rocketnet_client_io_t * io, uint64_t committed, const rocketnet_address_t * to )
{
    const bool quitting = io->quit.load( std::memory_order_acquire );
    rocketnet_assert( !quitting );

    if ( quitting || committed == 0 )
        return ROCKETNET_CLIENT_RINGS_EMPTY;

    const int ring = io->send_fill;
    if ( !io->to_held[ring] )
    {
        io->to[ring] = *to;
        io->to_held[ring] = true;
    }

    io->committed[ring] = committed;
    if ( !rocketnet_platform_mutex_try_acquire( io->mutex ) )
        return ROCKETNET_CLIENT_RINGS_LATE;

    const int next = rocketnet_buffer_handoff_publish( &io->send, ring, ROCKETNET_CLIENT_IO_NUM_RINGS - 1 );

    rocketnet_platform_mutex_release( io->mutex );

    if ( next < 0 )
        return ROCKETNET_CLIENT_RINGS_LATE;

    io->to_held[ring] = false;
    io->send_fill = next;
    io->committed[next] = 0;
    if ( io->transport->operations->wake )
        io->transport->operations->wake( io->transport->instance );

    return ROCKETNET_CLIENT_RINGS_HANDED_OFF;
}

// proofs/tla/RNBufferHandoff.tla ConsumerExchange: gives back a buffer update
// still holds and takes the ready one, in one try-acquired index exchange;
// wakes the I/O thread after releasing the mutex when it owed a publish.
// False when the mutex was busy: nothing moved, and the next call retries.

inline bool rocketnet_client_io_receive_exchange( rocketnet_client_io_t * io, bool take )
{
    if ( !rocketnet_platform_mutex_try_acquire( io->mutex ) )
        return false;
    if ( io->receive_held >= 0 )
        rocketnet_buffer_handoff_release( &io->receive, io->receive_held );

    io->receive_held = take ? rocketnet_buffer_handoff_take( &io->receive ) : -1;

    const bool owed = rocketnet_buffer_handoff_owed( &io->receive );

    rocketnet_platform_mutex_release( io->mutex );

    if ( owed && io->transport->operations->wake )
        io->transport->operations->wake( io->transport->instance );

    return true;
}

// proofs/tla/RNClientIo.tla Expose: returns the ready buffer and holds it for this update.
// proofs/tla/RNClientIoDecrypt.tla Expose: the same ready buffer, for update to read in arrival order.
// Main thread, at update's start. Returns the ready
// buffer and holds it, or NULL when none is ready or the mutex was busy (the
// datagrams wait for the next update; none is lost).

inline rocketnet_client_receive_buffer_t * rocketnet_client_io_expose( rocketnet_client_io_t * io )
{
    if ( io->receive_held >= 0 )
        io->buffer[io->receive_held]->num_packets = 0;
    if ( !rocketnet_client_io_receive_exchange( io, true ) )
        return NULL;

    return io->receive_held >= 0 ? io->buffer[io->receive_held] : NULL;
}

// proofs/tla/RNClientIo.tla Return: gives the held buffer back so the I/O thread can fill it again.
// proofs/tla/RNClientIoDecrypt.tla Return: the same give-back of the buffer update held.
// Main thread, at update's end. Gives the held buffer back
// so the I/O thread can publish into it during the frame; a busy mutex keeps
// it held until the next expose returns it.

inline void rocketnet_client_io_return( rocketnet_client_io_t * io )
{
    if ( io->receive_held < 0 )
        return;

    io->buffer[io->receive_held]->num_packets = 0;
    (void) rocketnet_client_io_receive_exchange( io, false );
}

// proofs/tla/RNClientIo.tla IoSend: sends each committed slot of one ring whose byte count is nonzero.
// Slot i is at data + i * frame_stride +
// wire_offset; a zero int byte count is an aborted slot and stays unsent.

inline void rocketnet_client_io_send_ring( rocketnet_client_io_t * io, int ring )
{
    const rocketnet_client_io_ring_view_t * view = &io->ring_view[ring];
    const uint64_t committed = io->committed[ring];

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
}

// proofs/tla/RNClientIo.tla IoSend: sends every committed slot of the ring in hand.
// proofs/tla/RNClientIo.tla IoReceive: reads datagrams into the fill buffer while it has room.
// proofs/tla/RNClientIo.tla IoPublish: publishes a filled buffer and takes a free one.
// proofs/tla/RNClientIo.tla IoOwe: leaves the fill buffer in hand when no free buffer is waiting.
// proofs/tla/RNClientIo.tla IoWait: blocks when the step did no work and quit is clear.
// proofs/tla/RNClientIo.tla IoWake: that wait returns when the socket or the wake is signalled.
// proofs/tla/RNClientIo.tla IoClearWake: a wake already set makes that wait return at once and clears it.
// proofs/tla/RNClientIo.tla IoExit: after the final ring is sent, stores stopped and leaves.
// proofs/tla/RNClientIoDecrypt.tla IoReceive: the same drain opens each datagram before the buffer is published.
// proofs/tla/RNClientIoDecrypt.tla IoPublish: publishes the fill buffer after the open.
// proofs/tla/RNClientIoDecrypt.tla IoOwe: keeps the fill buffer when the publish cannot take a free one.
// proofs/tla/RNClientIoDecrypt.tla IoWait: the same idle wait.
// proofs/tla/RNClientIoDecrypt.tla IoWake: the same return from the idle wait.
// proofs/tla/RNClientIoDecrypt.tla IoClearWake: the same already-set wake returning at once.
// proofs/tla/RNBufferHandoff.tla WorkerWait: the idle worker waits on the wake.
// proofs/tla/RNBufferHandoff.tla WorkerWakes: that wait returns and the worker runs again.

inline int rocketnet_client_io_step( rocketnet_client_io_t * io, bool block )
{
    rocketnet_assert( io );

    // a step after the exit does nothing: the final ring went once

    if ( io->stopped.load( std::memory_order_acquire ) )
        return ROCKETNET_CLIENT_IO_EXITED;

    // RNClientIo Stop: the main thread stores quit and wakes. Read it first:
    // every ring the main thread published, and the final ring stop handed
    // over, happened before the store this load sees.

    const bool quitting = io->quit.load( std::memory_order_acquire );

    bool worked = false;

    // proofs/tla/RNClientIo.tla IoFinishRing: releases a ring whose slots were sent and clears the ring in hand.
    // proofs/tla/RNClientIoShutdown.tla FinishRing: the same release, the sending ring becoming sent.
    // proofs/tla/RNClientIoShutdown.tla TakeRing: takes the oldest handed-off ring into the sending hand.
    // proofs/tla/RNBufferHandoff.tla ConsumerExchange: gives back the ring sent last step and takes the oldest ready one.
    // The send runs outside the mutex. Once quit is stored this repeats until no ring is ready.

    do
    {
        rocketnet_platform_mutex_acquire( io->mutex );

        if ( io->send_ring >= 0 )
            rocketnet_buffer_handoff_release( &io->send, io->send_ring );

        io->send_ring = rocketnet_buffer_handoff_take( &io->send );
        rocketnet_platform_mutex_release( io->mutex );

        if ( io->send_ring >= 0 )
        {
            rocketnet_client_io_send_ring( io, io->send_ring );

            worked = true;
        }
    } while ( quitting && io->send_ring >= 0 );

    if ( quitting )
    {
        // proofs/tla/RNClientIo.tla IoExit: no ring is left in hand, so the final ring is sent, then stopped is stored.
        // proofs/tla/RNBufferHandoff.tla WorkerExit: the worker sends the final ring and does not wait again.
        // proofs/tla/RNClientIoShutdown.tla WorkerExit: the same exit after the last ring.
        if ( io->send_final >= 0 )
            rocketnet_client_io_send_ring( io, io->send_final );

        rocketnet_client_io_drop_keys( io );

        io->stopped.store( true, std::memory_order_release );
        return ROCKETNET_CLIENT_IO_EXITED;
    }

    // proofs/tla/RNClientIo.tla IoReceive: drains datagrams into the buffer the I/O thread holds.
    // proofs/tla/RNClientIoDecrypt.tla IoReceive: each of those datagrams is opened before the buffer is published.
    // Every received datagram becomes a record, a runt or an oversize one
    // too: the main thread's prefilter (sdk/rocketnet_client_protocol.cpp,
    // sizes checked in sdk/rocketnet_protocol.cpp) refuses it and counts
    // PREFILTER_TOO_SHORT or PREFILTER_TOO_LARGE. One clock read per
    // transport call.

    rocketnet_client_receive_buffer_t * buffer = io->buffer[io->receive_fill];

    // proofs/tla/RNClientIoDecrypt.tla IoTakeKeys, IoDropKeys, IoKeepKeys: the epoch
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

            // proofs/tla/RNClientIoCompactLayout.tla Receive, Step and EndBatch: a faulty datagram is skipped and each
            // other one moved down to slot fill_slots + accepted, the slot its record names.
            int accepted = 0;

            for ( int i = 0; i < count; i++ )
            {
                if ( packet_bytes[i] < 0 || packet_bytes[i] > ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES )
                {
                    io->transport_faults++;
                    if ( packet_bytes[i] > ROCKETNET_FRAME_SIZE )
                        io->receive_too_large.store( io->receive_too_large.load( std::memory_order_relaxed ) + 1, std::memory_order_relaxed );
                    continue;
                }

                if ( accepted != i )
                    memmove( packet_data + (size_t) accepted * ROCKETNET_FRAME_SIZE,
                             packet_data + (size_t) i * ROCKETNET_FRAME_SIZE,
                             (size_t) ( packet_bytes[i] < ROCKETNET_FRAME_SIZE ? packet_bytes[i] : ROCKETNET_FRAME_SIZE ) );

                rocketnet_client_io_fill_record( io, buffer, &from[i], now,
                                                 buffer->data + (size_t) ( io->fill_slots + accepted ) * ROCKETNET_FRAME_SIZE,
                                                 (size_t) packet_bytes[i] );
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
                if ( bytes > ROCKETNET_RECEIVE_PACKET_BYTES )
                    io->receive_too_large.store( io->receive_too_large.load( std::memory_order_relaxed ) + 1, std::memory_order_relaxed );
                break;
            }

            rocketnet_client_io_fill_record( io, buffer, &from, now, packet_data, (size_t) bytes );
            io->fill_slots++;
            worked = true;
        }
    }

    // proofs/tla/RNClientIo.tla IoPublish: publishes the filled buffer and takes a free one when one is waiting.
    // proofs/tla/RNClientIoCompactLayout.tla Publish: the next buffer fills from slot 0 again.
    // proofs/tla/RNClientIo.tla IoOwe: records the publish as owed when no free buffer is waiting.
    // proofs/tla/RNClientIoDecrypt.tla IoPublish: publishes the fill buffer after each datagram was opened.
    // proofs/tla/RNClientIoDecrypt.tla IoOwe: keeps that buffer when the publish cannot take a free one.
    // proofs/tla/RNBufferHandoff.tla ProducerExchange: the worker publishes the filled buffer
    // when no buffer is waiting ready and one is free, taking the free one in
    // the same exchange. Otherwise the publish is owed: update's next exchange
    // reads it under the same mutex and wakes this thread, and the buffer keeps
    // filling while it has room. A publish is worked.

    if ( buffer->num_packets > 0 )
    {
        rocketnet_platform_mutex_acquire( io->mutex );

        const int next = rocketnet_buffer_handoff_publish( &io->receive, io->receive_fill, 1 );

        rocketnet_platform_mutex_release( io->mutex );

        if ( next >= 0 )
        {
            io->receive_fill = next;
            io->buffer[next]->num_packets = 0;
            io->fill_slots = 0;
            worked = true;
        }
    }

    // proofs/tla/RNClientIo.tla IoWait: waits only when nothing was done.
    // proofs/tla/RNClientIo.tla IoWake: the wait returns when the socket or the wake is signalled.
    // proofs/tla/RNClientIo.tla IoClearWake: a wake already set makes that wait return at once and clears it.
    // proofs/tla/RNClientIoDecrypt.tla IoWait: the same idle wait.
    // proofs/tla/RNClientIoDecrypt.tla IoWake: the same return from that wait.
    // proofs/tla/RNClientIoDecrypt.tla IoClearWake: the same already-set wake returning at once.
    // proofs/tla/RNBufferHandoff.tla WorkerWait: the idle worker waits here.
    // proofs/tla/RNBufferHandoff.tla WorkerWakes: that wait returns and the worker runs again.
    // waitReadable: wait only when nothing was done.
    // readable is true only while the I/O thread's buffer has room. With a
    // full buffer this waits on the wake alone: a wait that also returned on
    // readable would return at once every time and the I/O thread would spin
    // (FullWaitsOnWake). An owed publish was recorded under the mutex above,
    // before this wait, so update's next exchange wakes it.

    if ( block && !worked && !quitting && io->transport->operations->wait )
    {
        const bool readable = io->fill_slots < ROCKETNET_NUM_CLIENT_PACKETS;
        io->transport->operations->wait( io->transport->instance, readable, io->guard_timeout_seconds );
    }

    return worked ? ROCKETNET_CLIENT_IO_WORKED : ROCKETNET_CLIENT_IO_IDLE;
}

// proofs/tla/RNClientIo.tla IoExit: runs the step until it has stored stopped and returned exited.
// The transport creates the thread with this function and joins it before closing the transport.

inline void rocketnet_client_io_thread_function( rocketnet_platform_thread_t * thread, void * data )
{
    rocketnet_client_io_t * io = (rocketnet_client_io_t *) data;

    while ( true )
    {
        const int result = rocketnet_client_io_step( io, true );
        if ( result == ROCKETNET_CLIENT_IO_EXITED )
            break;

        // Built-in loopback has no native wait object. Keep its bounded idle polling on the worker; the caller still exchanges only indices.

        if ( result == ROCKETNET_CLIENT_IO_IDLE && !io->transport->operations->wait )
            rocketnet_platform_thread_sleep( thread, 0.001 );
    }
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
#if ROCKETNET_DEV
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven( void * context, const uint8_t * connect_token,
                                                                                     const struct rocketnet_client_transport_config_t * config, double time,
                                                                                     const rocketnet_client_transport_io_operations_t * operations, void * transport_context );

// Unit tests only: as create_step_driven, with the Unix timestamp supplied by
// the test so token expiry is deterministic and no wall clock is read. It
// reads the whole current config (rocketnet_client_transport_default_config_current),
// so a loopback client carries its loopback_hub.
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_at_timestamp( void * context, const uint8_t * connect_token,
                                                                                                  const struct rocketnet_client_transport_config_t * config, double time, uint64_t current_timestamp,
                                                                                                  const rocketnet_client_transport_io_operations_t * operations, void * transport_context );

// As create_step_driven_at_timestamp, and when status is not NULL it is
// written with ROCKETNET_STATUS_OK or the refusal's section-2.1 code.
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_at_timestamp_ex( void * context, const uint8_t * connect_token,
                                                                                                     const struct rocketnet_client_transport_config_t * config, double time, uint64_t current_timestamp,
                                                                                                     const rocketnet_client_transport_io_operations_t * operations, void * transport_context, int * status );

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
#endif // #if ROCKETNET_DEV

// The transport's I/O core, or NULL on the BASE path.
rocketnet_client_io_t * rocketnet_client_transport_client_io( struct rocketnet_client_transport_t * client_transport );

#endif // ROCKETNET_SDK_ROCKETNET_CLIENT_IO_H
