/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CLIENT_RECEIVE_HANDSHAKE_H
#define ROCKETNET_CLIENT_RECEIVE_HANDSHAKE_H

#include <atomic>
#include "rocketnet_internal.h"
#include "rocketnet_constants.h"

// The receive handoff of docs/CLIENT-IO.md _The receive path_ and the receive
// actions of tla/RNClientIo.tla. The I/O thread fills one of two buffers and
// publishes it Fill to Ready; update exposes a Ready buffer and returns it
// Exposed to Free at the end of every frame. Each buffer's state has one
// owner per transition: the I/O thread loads Free with acquire before it
// stores Fill, publishes with a release store, update loads Ready with
// acquire before it stores Exposed, and update returns with a seq_cst store.
// The handshake holds no datagrams and makes no driver call; counts are the
// caller's per-buffer datagram counts, written and read only across one of
// those state transitions.
//
// The wake decision is the one place two threads store one atomic and load
// another: the I/O thread stores receive_publish_owed true with seq_cst and
// then loads the other buffer's state with seq_cst (IoOwe), and update stores
// its Exposed buffer Free with seq_cst and then loads receive_publish_owed
// with seq_cst (Return). At least one side therefore sees the other's store,
// so either the I/O thread sees the buffer and publishes, or update sees the
// datagrams owed and wakes it. With release and acquire alone both loads can
// miss and the I/O thread sleeps on a batch nobody wakes.

enum
{
    ROCKETNET_CLIENT_BUFFER_FILL = 0,       // the I/O thread reads into this buffer
    ROCKETNET_CLIENT_BUFFER_READY,          // filled and published, waiting for update
    ROCKETNET_CLIENT_BUFFER_EXPOSED,        // update holds this buffer this frame
    ROCKETNET_CLIENT_BUFFER_FREE,           // either thread may take it next
};

struct rocketnet_client_receive_handshake_t
{
    std::atomic<int> state[2];              // one ROCKETNET_CLIENT_BUFFER_<STATE> per buffer
    std::atomic<bool> receive_publish_owed; // I/O thread holds a batch it cannot publish yet
    int count[2];                           // datagrams in each buffer, ordered by state
    int fill;                               // the buffer the I/O thread fills; I/O thread only
};

// RNClientIo.tla Init: buffer 0 is Filling and buffer 1 is Free at the start.
inline void rocketnet_client_receive_handshake_init( rocketnet_client_receive_handshake_t * handshake )
{
    handshake->state[0].store( ROCKETNET_CLIENT_BUFFER_FILL, std::memory_order_relaxed );
    handshake->state[1].store( ROCKETNET_CLIENT_BUFFER_FREE, std::memory_order_relaxed );
    handshake->receive_publish_owed.store( false, std::memory_order_relaxed );
    handshake->count[0] = 0;
    handshake->count[1] = 0;
    handshake->fill = 0;
}

// RNClientIo.tla IoReceive (I/O thread): one datagram into the buffer being
// filled while it has room. Returns false when the buffer is full.
inline bool rocketnet_client_receive_handshake_io_receive( rocketnet_client_receive_handshake_t * handshake )
{
    const int buffer = handshake->fill;
    if ( handshake->state[buffer].load( std::memory_order_relaxed ) != ROCKETNET_CLIENT_BUFFER_FILL )
        return false;
    if ( handshake->count[buffer] >= ROCKETNET_NUM_CLIENT_PACKETS )
        return false;
    handshake->count[buffer]++;
    return true;
}

// RNClientIo.tla IoPublish (I/O thread): publish the filled buffer (Fill to
// Ready, release) when a Free buffer is there to fill next (acquire load,
// then Fill and a zero count). Returns true when the batch was published.
inline bool rocketnet_client_receive_handshake_io_publish( rocketnet_client_receive_handshake_t * handshake )
{
    const int filled = handshake->fill;
    if ( handshake->count[filled] <= 0 )
        return false;
    const int next = filled ^ 1;
    if ( handshake->state[next].load( std::memory_order_acquire ) != ROCKETNET_CLIENT_BUFFER_FREE )
        return false;
    handshake->count[next] = 0;
    handshake->state[next].store( ROCKETNET_CLIENT_BUFFER_FILL, std::memory_order_relaxed );
    handshake->fill = next;
    handshake->state[filled].store( ROCKETNET_CLIENT_BUFFER_READY, std::memory_order_release );
    handshake->receive_publish_owed.store( false, std::memory_order_relaxed );
    return true;
}

// RNClientIo.tla IoOwe (I/O thread): a filled buffer with no Free buffer to
// publish into records the publish as owed (seq_cst store) and re-reads the
// other buffer's state (seq_cst load). Returns true when the publish is owed
// and the thread must wait; false when the re-read found the buffer Free, so
// the caller publishes instead.
inline bool rocketnet_client_receive_handshake_io_owe( rocketnet_client_receive_handshake_t * handshake )
{
    const int filled = handshake->fill;
    if ( handshake->count[filled] <= 0 )
        return false;
    const int other = filled ^ 1;
    if ( handshake->state[other].load( std::memory_order_acquire ) == ROCKETNET_CLIENT_BUFFER_FREE )
        return false;
    handshake->receive_publish_owed.store( true, std::memory_order_seq_cst );
    if ( handshake->state[other].load( std::memory_order_seq_cst ) == ROCKETNET_CLIENT_BUFFER_FREE )
        return false;
    return true;
}

// RNClientIo.tla IoWait (I/O thread): the readable argument for the driver's
// wait. True while the filled buffer has room; false while it is full, so a
// full buffer leaves datagrams in the socket and waits on the wake alone.
inline bool rocketnet_client_receive_handshake_io_wait( rocketnet_client_receive_handshake_t * handshake )
{
    return handshake->count[handshake->fill] < ROCKETNET_NUM_CLIENT_PACKETS;
}

// RNClientIo.tla Expose (main thread): expose the Ready buffer (Ready to
// Exposed after an acquire load) and return its index, or -1 when update
// already holds an Exposed buffer or none is Ready.
inline int rocketnet_client_receive_handshake_update_expose( rocketnet_client_receive_handshake_t * handshake )
{
    int ready = -1;
    for ( int buffer = 0; buffer < 2; buffer++ )
    {
        const int state = handshake->state[buffer].load( std::memory_order_acquire );
        if ( state == ROCKETNET_CLIENT_BUFFER_EXPOSED )
            return -1;
        if ( state == ROCKETNET_CLIENT_BUFFER_READY )
            ready = buffer;
    }
    if ( ready < 0 )
        return -1;
    handshake->state[ready].store( ROCKETNET_CLIENT_BUFFER_EXPOSED, std::memory_order_relaxed );
    return ready;
}

// RNClientIo.tla Return (main thread): return the buffer update holds
// (Exposed to Free, seq_cst store, count zeroed first), then load the owed
// flag (seq_cst load). Returns true when the caller must wake the I/O thread.
inline bool rocketnet_client_receive_handshake_update_return( rocketnet_client_receive_handshake_t * handshake )
{
    for ( int buffer = 0; buffer < 2; buffer++ )
    {
        if ( handshake->state[buffer].load( std::memory_order_relaxed ) == ROCKETNET_CLIENT_BUFFER_EXPOSED )
        {
            handshake->count[buffer] = 0;
            handshake->state[buffer].store( ROCKETNET_CLIENT_BUFFER_FREE, std::memory_order_seq_cst );
            break;
        }
    }
    return handshake->receive_publish_owed.load( std::memory_order_seq_cst );
}

#endif // #ifndef ROCKETNET_CLIENT_RECEIVE_HANDSHAKE_H
