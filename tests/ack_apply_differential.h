/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_ACK_APPLY_DIFFERENTIAL_H
#define ROCKETNET_TESTS_ACK_APPLY_DIFFERENTIAL_H

// The deferred ack apply against the inline apply on the same trace
// (sdk/rocketnet_ack.h, rocketnet_server_acks_t; proofs/tla/RNAckApplyPhase.tla).
// Three server acks, A inline, B and C deferred, take the same sends, slot
// resets and blocks, update after update. A applies each block where it
// arrives. B and C queue them; B's clients apply at its close (the
// single-call update), C's through the caller's schedule (the jobs). After
// every update the batches (ids, client indexes, connection ids, in order),
// both counters and every endpoint's whole state must be equal.
//
// The trace: a few clients; sends between updates, some keep-alives, some
// burned sequences; in the receive phase each client's blocks (fresh, the
// last one again for the memo, one with up to 255 ranges), a send recorded
// mid-receive (settled first, as the protocol's write_session_packet does)
// and a slot reset mid-receive (settled first, as connection_reset does).
// Some updates queue more blocks than the queue holds, and some more ranges
// than the range store holds, so the early gather runs.
//
// The includer defines ACK_APPLY_CHECK( condition ). The schedule is called
// once per update with C open: schedule( acks, order, count ) must call
// rocketnet_server_acks_apply_client for clients of order[] in any way it
// likes (any order, repeats, threads, some left out).

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <string.h>

#define ACK_APPLY_CLIENTS 6

struct ack_apply_stats_t
{
    int blocks;
    int early_gathers;          // a receive_block that found the queue full and gathered first
    int settles_send;
    int settles_reset;
    int memo_repeats;
    int long_blocks;
    uint64_t acked;
};

static uint64_t ack_apply_next( uint64_t * state )
{
    uint64_t z = ( *state += 0x9E3779B97F4A7C15ull );
    z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
    z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
    return z ^ ( z >> 31 );
}

static uint64_t ack_apply_below( uint64_t * state, uint64_t n )
{
    return n ? ack_apply_next( state ) % n : 0;
}

// a validated block shape: largest at or a little above the highest sent,
// a first run, then up to max_ranges (gap, run) pairs, never below 1
static void ack_apply_block( uint64_t * rng, uint64_t highest, int max_ranges, struct rocketnet_ack_block_t * block )
{
    memset( block, 0, sizeof( *block ) );
    uint64_t largest = highest ? highest : 1;
    if ( ack_apply_below( rng, 12 ) == 0 )
    {
        largest = highest + 1 + ack_apply_below( rng, 3 );                     // above everything sent
    }
    else
    {
        const uint64_t lag = ack_apply_below( rng, 8 );
        largest = largest > lag ? largest - lag : 1;
    }

    uint64_t first = 1 + ack_apply_below( rng, max_ranges > 8 ? 4 : 40 );
    if ( ack_apply_below( rng, 20 ) == 0 )
    {
        first = ROCKETNET_SENT_PACKET_BUFFER_SIZE - 64 + ack_apply_below( rng, 256 ); // reaches below the window
    }
    if ( first > largest )
    {
        first = largest;
    }
    block->largest = largest;
    block->first_range_length = (uint32_t) first;

    const int ranges = (int) ack_apply_below( rng, (uint64_t) max_ranges + 1 );
    uint64_t low = largest - first + 1;
    for ( int i = 0; i < ranges; i++ )
    {
        const uint64_t gap = 1 + ack_apply_below( rng, max_ranges > 8 ? 2 : 8 );
        const uint64_t length = 1 + ack_apply_below( rng, max_ranges > 8 ? 2 : 24 );
        if ( low <= gap + length + 1 )
        {
            break;
        }
        block->range_gap[i] = (uint16_t) gap;
        block->range_length[i] = (uint16_t) length;
        block->num_ranges = i + 1;
        low = low - gap - length;
    }
}

struct ack_apply_world_t
{
    struct rocketnet_server_acks_t * acks[3];  // A inline, B deferred whole, C deferred by schedule
    uint64_t connection_ids[3][ACK_APPLY_CLIENTS * ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    uint64_t unknown[3];
    uint64_t batch_full[3];
    uint64_t next_sequence[ACK_APPLY_CLIENTS];
    uint64_t connection[ACK_APPLY_CLIENTS];
    struct rocketnet_ack_block_t last[ACK_APPLY_CLIENTS];
};

static void ack_apply_send( struct ack_apply_world_t * w, uint64_t * rng, int c, double time )
{
    if ( ack_apply_below( rng, 10 ) == 0 )
    {
        w->next_sequence[c]++;                                                 // burned: never recorded
    }
    const uint64_t sequence = w->next_sequence[c]++;
    const int has_id = ack_apply_below( rng, 5 ) != 0;
    const int bytes = 40 + (int) ack_apply_below( rng, 1200 );
    const bool keep_alive = ack_apply_below( rng, 8 ) == 0;
    for ( int k = 0; k < 3; k++ )
    {
        (void) rocketnet_ack_endpoint_record_sent_classified( &w->acks[k]->endpoints[c], sequence, bytes,
                                                              has_id ? ( (uint64_t) c << 40 ) + sequence : 0, has_id, time );
        if ( keep_alive )
        {
            rocketnet_ack_endpoint_mark_keep_alive( &w->acks[k]->endpoints[c], sequence );
        }
    }
}

template <typename Schedule>
static void ack_apply_differential( uint64_t seed, int updates, Schedule schedule, struct ack_apply_stats_t * stats )
{
    struct ack_apply_world_t * w = (struct ack_apply_world_t *) rocketnet_malloc( NULL, sizeof( struct ack_apply_world_t ) );
    ACK_APPLY_CHECK( w != NULL );
    if ( !w )
    {
        return;
    }
    memset( w, 0, sizeof( *w ) );
    for ( int k = 0; k < 3; k++ )
    {
        w->acks[k] = rocketnet_server_acks_create( NULL, ACK_APPLY_CLIENTS );
        ACK_APPLY_CHECK( w->acks[k] != NULL );
        if ( k > 0 )
        {
            ACK_APPLY_CHECK( rocketnet_server_acks_defer( w->acks[k] ) );
        }
    }
    for ( int c = 0; c < ACK_APPLY_CLIENTS; c++ )
    {
        w->next_sequence[c] = 1;
        w->connection[c] = 0x100 + (uint64_t) c;
    }

    uint64_t rng = seed;
    double time = 1000.0;
    int order[ACK_APPLY_CLIENTS];

    for ( int update = 0; update < updates; update++ )
    {
        time += 0.001 * (double) ( 1 + ack_apply_below( &rng, 20 ) );

        // the fill: sends, none for some clients so their last block repeats

        for ( int c = 0; c < ACK_APPLY_CLIENTS; c++ )
        {
            const uint64_t sends = ack_apply_below( &rng, 3 ) == 0 ? 0 : ack_apply_below( &rng, 40 );
            for ( uint64_t s = 0; s < sends; s++ )
            {
                ack_apply_send( w, &rng, c, time );
            }
        }

        // the receive phase

        for ( int k = 0; k < 3; k++ )
        {
            rocketnet_server_acks_begin_receive( w->acks[k], time );
        }

        const uint64_t shape = ack_apply_below( &rng, 16 );
        int events = 1 + (int) ack_apply_below( &rng, 3 * ACK_APPLY_CLIENTS );
        if ( shape == 0 )
        {
            events = 2 * w->acks[1]->pending_capacity + (int) ack_apply_below( &rng, 64 ); // past the queue
        }
        else if ( shape == 1 )
        {
            events = 3000 + (int) ack_apply_below( &rng, 400 );                           // long blocks: past the range store
        }

        for ( int e = 0; e < events; e++ )
        {
            const int c = (int) ack_apply_below( &rng, ACK_APPLY_CLIENTS );
            const uint64_t kind = ack_apply_below( &rng, 40 );

            if ( kind == 0 )
            {
                // a send mid-receive: the protocol settles the client first

                for ( int k = 0; k < 3; k++ )
                {
                    rocketnet_server_acks_settle( w->acks[k], c );
                }
                stats->settles_send++;
                ack_apply_send( w, &rng, c, time );
                continue;
            }

            if ( kind == 1 )
            {
                // a slot reset mid-receive (a disconnect, the integrity
                // limit): settled, then the slot's next connection

                for ( int k = 0; k < 3; k++ )
                {
                    rocketnet_server_acks_settle( w->acks[k], c );
                    rocketnet_server_acks_reset_slot( w->acks[k], c );
                }
                stats->settles_reset++;
                w->next_sequence[c] = 1;
                w->connection[c] += 0x1000;
                memset( &w->last[c], 0, sizeof( w->last[c] ) );
                continue;
            }

            struct rocketnet_ack_block_t block;
            const uint64_t highest = w->acks[0]->endpoints[c].highest_sequence;
            if ( kind < 12 && w->last[c].largest )
            {
                block = w->last[c];
                stats->memo_repeats++;
            }
            else if ( kind == 12 || shape == 1 )
            {
                ack_apply_block( &rng, highest, ROCKETNET_ACK_BLOCK_MAX_RANGES, &block );
                stats->long_blocks++;
            }
            else
            {
                ack_apply_block( &rng, highest, 6, &block );
            }
            w->last[c] = block;

            const int pending_before = w->acks[1]->num_pending;
            for ( int k = 0; k < 3; k++ )
            {
                rocketnet_server_acks_receive_block( w->acks[k], c, &block, time, w->connection[c], w->connection_ids[k],
                                                     &w->unknown[k], &w->batch_full[k] );
            }
            if ( block.largest != 0 && w->acks[1]->num_pending <= pending_before )
            {
                stats->early_gathers++;
            }
            stats->blocks++;
        }

        // open; C's clients go to the schedule, B's wait for its close

        for ( int k = 0; k < 3; k++ )
        {
            rocketnet_server_acks_open( w->acks[k] );
        }

        for ( int c = 0; c < ACK_APPLY_CLIENTS; c++ )
        {
            order[c] = c;
        }
        for ( int c = ACK_APPLY_CLIENTS - 1; c > 0; c-- )
        {
            const int j = (int) ack_apply_below( &rng, (uint64_t) c + 1 );
            const int t = order[c];
            order[c] = order[j];
            order[j] = t;
        }
        schedule( w->acks[2], order, ACK_APPLY_CLIENTS, &rng );

        for ( int k = 0; k < 3; k++ )
        {
            rocketnet_server_acks_close( w->acks[k], w->connection_ids[k], &w->unknown[k], &w->batch_full[k] );
        }

        // the comparison

        for ( int k = 1; k < 3; k++ )
        {
            const struct rocketnet_server_acks_t * a = w->acks[0];
            const struct rocketnet_server_acks_t * b = w->acks[k];
            ACK_APPLY_CHECK( b->phase == ROCKETNET_SERVER_ACKS_CLOSED );
            ACK_APPLY_CHECK( b->num_pending == 0 );
            ACK_APPLY_CHECK( a->num_acked == b->num_acked );
            const int n = a->num_acked < b->num_acked ? a->num_acked : b->num_acked;
            for ( int i = 0; i < n; i++ )
            {
                ACK_APPLY_CHECK( a->acked_packet_ids[i] == b->acked_packet_ids[i] );
                ACK_APPLY_CHECK( a->acked_client_index[i] == b->acked_client_index[i] );
                ACK_APPLY_CHECK( w->connection_ids[0][i] == w->connection_ids[k][i] );
            }
            ACK_APPLY_CHECK( w->unknown[0] == w->unknown[k] );
            ACK_APPLY_CHECK( w->batch_full[0] == w->batch_full[k] );
            for ( int c = 0; c < ACK_APPLY_CLIENTS; c++ )
            {
                ACK_APPLY_CHECK( rocketnet_ack_endpoint_state_equal( &a->endpoints[c], &b->endpoints[c] ) );
                ACK_APPLY_CHECK( a->endpoints[c].largest_acked_sequence == b->endpoints[c].largest_acked_sequence );
                ACK_APPLY_CHECK( a->endpoints[c].keep_alive_acked == b->endpoints[c].keep_alive_acked );
                ACK_APPLY_CHECK( a->endpoints[c].rtt == b->endpoints[c].rtt );
                ACK_APPLY_CHECK( memcmp( &a->endpoints[c].block_memo, &b->endpoints[c].block_memo, sizeof( a->endpoints[c].block_memo ) ) == 0 );
            }
        }
        stats->acked += (uint64_t) w->acks[0]->num_acked;
    }

    for ( int k = 0; k < 3; k++ )
    {
        rocketnet_server_acks_destroy( w->acks[k] );
    }
    rocketnet_free( NULL, w );
}

#endif // #ifndef ROCKETNET_TESTS_ACK_APPLY_DIFFERENTIAL_H
