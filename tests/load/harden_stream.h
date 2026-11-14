/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The harden_load receive stream (docs/HARDEN_LOAD.md): each message's kind,
// connection and sequence, judged against what came before on its stream.
// Pure bookkeeping over the headers the roles read, so the unit suite can feed
// it planted sequences (tests/test_unit_harden_stream.cpp). No time, no
// sockets, no files.

#pragma once

#ifndef ROCKETNET_TESTS_LOAD_HARDEN_STREAM_H
#define ROCKETNET_TESTS_LOAD_HARDEN_STREAM_H

#include <stdint.h>

#define HARDEN_KIND_BURST 0
#define HARDEN_KIND_RELIABLE 1
#define HARDEN_KIND_BIG 2
#define HARDEN_KIND_INPUT 3
#define HARDEN_KIND_INPUT_RELIABLE 4
#define HARDEN_KINDS 5

struct harden_message_t
{
    uint32_t kind;
    uint64_t client_id;
    uint32_t connection;
    uint32_t sequence;
    uint32_t bytes;
};

// one receive stream. Reliable ordered kinds must arrive exactly in order.
// Unreliable kinds promise neither delivery nor order, only at most once
// (rocketnet.h): a gap is counted lost until its message arrives late, a late
// arrival is counted reordered, and a second arrival of a sequence still in
// the window is a duplicate, an error. A sequence below a connection's first
// is counted in head, not lost, so its late arrival takes it back from head;
// one older than the window cannot be told from a duplicate, so it is counted
// late_past_window only and moves neither lost nor delivered. Each count a
// late arrival takes back was given once for that sequence, so none goes
// below zero

#define HARDEN_WINDOW 1024

struct harden_stream_t
{
    uint64_t client_id = 0; // the stream's owner: a server slot's stream changes owner when another client takes the slot
    uint32_t connection = 0;
    uint32_t expected = 0;
    uint32_t first = 0;     // the connection's first sequence taken: the ones below it are in head
    bool started = false;
    uint64_t seen[HARDEN_WINDOW / 64] = {};
};

struct harden_verify_t
{
    uint64_t delivered[HARDEN_KINDS] = {};
    uint64_t lost[HARDEN_KINDS] = {};
    uint64_t head[HARDEN_KINDS] = {};
    uint64_t reordered[HARDEN_KINDS] = {};
    uint64_t late_past_window[HARDEN_KINDS] = {};
    uint64_t errors = 0;
    uint64_t wrong_owner = 0;
    uint64_t reliable_gaps = 0;
    uint64_t old_connection = 0;
    uint64_t back = 0;
    uint64_t duplicates = 0;
};

static inline bool harden_stream_seen( const harden_stream_t * stream, uint32_t sequence )
{
    return ( stream->seen[( sequence % HARDEN_WINDOW ) / 64] >> ( sequence % 64 ) ) & 1;
}

static inline void harden_stream_mark( harden_stream_t * stream, uint32_t sequence, bool seen )
{
    const uint64_t bit = UINT64_C( 1 ) << ( sequence % 64 );
    uint64_t & word = stream->seen[( sequence % HARDEN_WINDOW ) / 64];
    word = seen ? ( word | bit ) : ( word & ~bit );
}

static inline void harden_stream_take( harden_stream_t * stream, const harden_message_t * m, bool reliable, harden_verify_t * v )
{
    if ( stream->started && stream->client_id == m->client_id && m->connection < stream->connection )
    {
        // the same client's earlier connection: connection ids only grow (the
        // server's for each connect, and a client's own count of its
        // connects), so this is an old session's message delivered after the
        // new one began. An isolation error, and it never resets the stream
        v->old_connection++;
        v->errors++;
        return;
    }
    if ( !stream->started || stream->client_id != m->client_id || stream->connection != m->connection )
    {
        // a new connection's first message: sequences start at 0, so what
        // came before it was sent before this end could take it. A reliable
        // ordered kind delivers its connection's messages in order from the
        // first, so a first message past 0 names messages lost on a live
        // connection: a gap
        if ( reliable && m->sequence != 0 )
        {
            v->reliable_gaps++;
            v->errors++;
        }
        *stream = harden_stream_t();
        stream->started = true;
        stream->client_id = m->client_id;
        stream->connection = m->connection;
        if ( !reliable )
        {
            // an unreliable kind's messages before the first taken are the
            // head: lost before this end could take them, counted, never failed
            v->head[m->kind] += m->sequence;
        }
        stream->expected = m->sequence;
        stream->first = m->sequence;
    }
    if ( m->sequence < stream->expected )
    {
        if ( reliable )
        {
            v->back++;
            v->errors++;
            return;
        }
        if ( stream->expected - m->sequence > HARDEN_WINDOW )
        {
            // older than the window: no record of whether it came before, so
            // it may be a duplicate; it is named, and not counted delivered
            v->late_past_window[m->kind]++;
            return;
        }
        if ( harden_stream_seen( stream, m->sequence ) )
        {
            v->duplicates++;
            v->errors++;
            return;
        }
        uint64_t & owed = m->sequence < stream->first ? v->head[m->kind] : v->lost[m->kind];
        if ( owed == 0 )
        {
            // never: the count this arrival takes back was not given. An
            // error, never a wrapped count
            v->errors++;
            return;
        }
        harden_stream_mark( stream, m->sequence, true );
        v->reordered[m->kind]++;
        owed--;
        v->delivered[m->kind]++;
        return;
    }
    if ( m->sequence > stream->expected )
    {
        if ( reliable )
        {
            v->reliable_gaps++;
            v->errors++;
        }
        v->lost[m->kind] += m->sequence - stream->expected;
    }
    // every sequence the window moves past is cleared, this one is seen
    const uint32_t clear = m->sequence - stream->expected < HARDEN_WINDOW ? m->sequence - stream->expected : HARDEN_WINDOW;
    for ( uint32_t i = 0; i < clear; i++ )
    {
        harden_stream_mark( stream, stream->expected + i, false );
    }
    harden_stream_mark( stream, m->sequence, true );
    stream->expected = m->sequence + 1;
    v->delivered[m->kind]++;
}

#endif // ROCKETNET_TESTS_LOAD_HARDEN_STREAM_H
