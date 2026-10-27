#pragma once

#ifndef ROCKETNET_TOOLS_LOAD_SPLIT_CAPTURE_H
#define ROCKETNET_TOOLS_LOAD_SPLIT_CAPTURE_H

/* The load_split delivery capture: per connection and per counter evidence
 * for a failed start or a kick, printed in the split_* line format the
 * evidence logs already hold.
 *
 * Each connection is sampled once per SPLIT_CAPTURE_INTERVAL_S: its packet
 * counts, its resends where the side keeps them, and its delivery capture
 * (sdk/rocketnet_layers.h: sequences received, sent and acknowledged, the
 * last accepted receive, the reliable window and the send budget). A
 * connection seen connected at one sample and not at the next was ended in
 * between: its last connected sample is printed with why=kick, at once,
 * because the SDK resets a slot when it ends it and the state is gone by the
 * next read. At the end of the run every connection is printed (why=end,
 * a fresh sample for a live one, the last connected sample for an ended one),
 * then the counters: the named set always, every other counter when nonzero,
 * and on a side with one counter block per connection the named set per
 * connection when nonzero.
 *
 * The run mark is the time RUN was queued (server) or received (client) and,
 * on the server, the first sequence a packet sent after RUN was queued can
 * carry: a largest acknowledged sequence at or past it says the client
 * acknowledged a packet sent after RUN was queued (RUN's own or a later one;
 * reliable_unacked is the total ordered window still outstanding and cannot
 * identify RUN specifically). A client line has
 * no sequence mark and prints acked_past_run=na.
 *
 * A connection that ends less than one interval after it connected is never
 * sampled connected: it prints only end_never, with no kick line.
 *
 * Pure over a source and an emitter the caller supplies: no clock, socket or
 * allocation; time is an argument. load_split.c binds the source to the SDK,
 * test_unit_load_split_capture.cpp to a fake. */

#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_counters.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SPLIT_CAPTURE_INTERVAL_S 1.0
#define SPLIT_CAPTURE_LINE_BYTES 1536

/* The counters every capture names, zero or not: the delivery and refusal
 * evidence the start barrier failure is decided by. */
static const int split_capture_named_counters[] = {
    ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE,
    ROCKETNET_COUNTER_ACKED_BATCH_FULL,
    ROCKETNET_COUNTER_SEND_HISTORY_LOST,
    ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW,
    ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED,
    ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY,
    ROCKETNET_COUNTER_MESSAGE_RESENT,
    ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED,
    ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_PAYLOAD_RECEIVED,
    ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN,
    ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_PAYLOAD_DELIVERED,
    ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT,
    ROCKETNET_COUNTER_SEND_RING_FULL,
    ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS,
    ROCKETNET_COUNTER_SEND_LEASE_FULL,
    ROCKETNET_COUNTER_SEND_FAILED,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_REPLAYED,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_NO_SOCKET,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED,
    ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE,
    ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED,
    ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE,
    ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED,
};

#define SPLIT_CAPTURE_NUM_NAMED ( (int) ( sizeof( split_capture_named_counters ) / sizeof( split_capture_named_counters[0] ) ) )

struct split_capture_sample_t
{
    double time;                                /* when sampled; 0 when never */
    int state;                                  /* the client state, or 1 connected / 0 not on the server */
    uint64_t packets_sent, packets_received;
    uint64_t messages_resent;                   /* the connection's resends; 0 where the side keeps none per connection */
    int resends_known;                          /* 1 when messages_resent is per connection */
    struct rocketnet_delivery_capture_t delivery;
};

struct split_capture_entry_t
{
    struct split_capture_sample_t last;         /* the last sample taken while connected */
    int seen_connected;                         /* some sample found it connected */
    int ended;                                  /* a later sample found it not connected */
    uint64_t named[SPLIT_CAPTURE_NUM_NAMED];     /* the connection's own named counters at the last connected sample (one block per connection) */
    double run_time;                            /* RUN sent or received; 0 when not yet */
    uint64_t run_sequence;                      /* server: the first sequence sent after RUN was queued; 0 on a client */
};

struct split_capture_source_t
{
    void * context;
    /* one connection's sample at now; returns 0 when the connection does not exist */
    int ( *sample )( void * context, int index, double now, struct split_capture_sample_t * out );
    /* a counter's value: index -1 the side's whole block, otherwise that
     * connection's own block (a side with one block per connection) */
    uint64_t ( *counter )( void * context, int index, int counter );
    int counters_per_connection;                /* 1 on a client shard */
};

struct split_capture_emit_t
{
    void * context;
    void ( *line )( void * context, const char * line );
};

struct split_capture_t
{
    const char * role;
    uint64_t run_id;
    int first;                                  /* the global index of entry 0 */
    int count;
    struct split_capture_entry_t * entries;     /* count entries, the caller's memory */
    double next_sample;                         /* 0: sample at the first tick */
    struct split_capture_source_t source;
    struct split_capture_emit_t emit;
    uint64_t kicks_printed;
};

static inline int split_capture_is_named( int counter )
{
    for ( int i = 0; i < SPLIT_CAPTURE_NUM_NAMED; ++i )
        if ( split_capture_named_counters[i] == counter )
            return 1;
    return 0;
}

static inline void split_capture_init( struct split_capture_t * capture, const char * role, uint64_t run_id,
                                       int first, int count, struct split_capture_entry_t * entries,
                                       const struct split_capture_source_t * source,
                                       const struct split_capture_emit_t * emit )
{
    memset( capture, 0, sizeof( *capture ) );
    capture->role = role;
    capture->run_id = run_id;
    capture->first = first;
    capture->count = count;
    capture->entries = entries;
    capture->source = *source;
    capture->emit = *emit;
    if ( entries && count > 0 )
        memset( entries, 0, (size_t) count * sizeof( *entries ) );
}

/* The age of a time at now, or -1 for never (0). */
static inline double split_capture_age( double now, double then )
{
    return then > 0.0 ? now - then : -1.0;
}

/* One connection's line. now is the print time; the sample's own fields are
 * aged at the sample's time, so a kick line says how stale each was then. */
static inline int split_capture_format( char * buffer, size_t bytes, const struct split_capture_t * capture, int index,
                                        const char * why, double now, int state_now,
                                        const struct split_capture_sample_t * sample )
{
    const struct split_capture_entry_t * entry = &capture->entries[index];
    const struct rocketnet_delivery_capture_t * d = &sample->delivery;
    const int run_marked = capture->entries[index].run_sequence > 0;
    char server_slot[24];
    if ( d->server_mapping_known )
        snprintf( server_slot, sizeof( server_slot ), "%d", d->server_slot );
    else
        snprintf( server_slot, sizeof( server_slot ), "na" );
    return snprintf( buffer, bytes,
                     "split_capture: role=%s run_id=%016" PRIx64 " client=%d why=%s state_now=%d sample_age_s=%.3f state=%d connected=%d"
                     " server_slot=%s remote_endpoint=%s"
                     " packets_sent=%" PRIu64 " packets_received=%" PRIu64 " messages_resent=%s%" PRIu64
                     " last_receive_age_s=%.3f received_sequence=%" PRIu64 " sent_sequence=%" PRIu64 " acked_sequence=%" PRIu64
                     " keep_alive_acked=%d run_age_s=%.3f run_sequence=%" PRIu64 " acked_past_run=%s"
                     " reliable_unacked=%" PRIu64 " resend_queued=%" PRIu64 " oldest_unacked_age_s=%.3f"
                     " srtt_ms=%.3f resend_delay_ms=%.3f window_stalled=%d budget_reliable_waiting=%d budget_tokens=%.1f",
                     capture->role, capture->run_id, capture->first + index, why, state_now, split_capture_age( now, sample->time ),
                     sample->state, d->connected, server_slot,
                     d->server_mapping_known ? d->remote_endpoint : "na", sample->packets_sent, sample->packets_received,
                     sample->resends_known ? "" : "na:", sample->messages_resent,
                     d->connected ? d->last_receive_age_seconds : -1.0,
                     d->highest_received_sequence, d->highest_sent_sequence, d->largest_acked_sequence,
                     d->keep_alive_acked, split_capture_age( sample->time, entry->run_time ), entry->run_sequence,
                     !run_marked ? "na" : ( d->largest_acked_sequence >= entry->run_sequence ? "yes" : "no" ),
                     d->reliable_unacked, d->reliable_resend_queued,
                     d->connected ? d->reliable_oldest_unacked_age_seconds : -1.0,
                     d->srtt_seconds * 1000.0, d->resend_delay_seconds * 1000.0, d->window_stalled,
                     d->budget_reliable_waiting, d->budget_tokens );
}

static inline void split_capture_emit_entry( struct split_capture_t * capture, int index, const char * why, double now,
                                             int state_now, const struct split_capture_sample_t * sample )
{
    char line[SPLIT_CAPTURE_LINE_BYTES];
    split_capture_format( line, sizeof( line ), capture, index, why, now, state_now, sample );
    capture->emit.line( capture->emit.context, line );
}

static inline void split_capture_emit_counter( struct split_capture_t * capture, int index, int counter, uint64_t value )
{
    char line[SPLIT_CAPTURE_LINE_BYTES];
    char client[16];
    if ( index < 0 )
        snprintf( client, sizeof( client ), "all" );
    else
        snprintf( client, sizeof( client ), "%d", capture->first + index );
    snprintf( line, sizeof( line ), "split_counter: role=%s run_id=%016" PRIx64 " client=%s name=%s value=%" PRIu64,
              capture->role, capture->run_id, client, rocketnet_counter_name( counter ), value );
    capture->emit.line( capture->emit.context, line );
}

/* One connection's own named counters from its last connected sample, the
 * nonzero ones; a side with one block for all connections has none. */
static inline void split_capture_connection_counters( struct split_capture_t * capture, int index )
{
    if ( !capture->source.counters_per_connection )
        return;
    for ( int i = 0; i < SPLIT_CAPTURE_NUM_NAMED; ++i )
        if ( capture->entries[index].named[i] )
            split_capture_emit_counter( capture, index, split_capture_named_counters[i], capture->entries[index].named[i] );
}

/* Takes a connected sample as the connection's last, with its own named
 * counters on a side that keeps one block per connection. */
static inline void split_capture_keep( struct split_capture_t * capture, int index, const struct split_capture_sample_t * sample )
{
    struct split_capture_entry_t * entry = &capture->entries[index];
    entry->last = *sample;
    entry->seen_connected = 1;
    entry->ended = 0;
    if ( capture->source.counters_per_connection )
        for ( int i = 0; i < SPLIT_CAPTURE_NUM_NAMED; ++i )
            entry->named[i] = capture->source.counter( capture->source.context, index, split_capture_named_counters[i] );
}

/* The side's counters: the named set always, then every other nonzero one;
 * with one block per connection and with_connections, also each
 * connection's nonzero named set (the end only: a kick prints the kicked
 * connection's own, so a tick inside the run stays small). */
static inline void split_capture_counters( struct split_capture_t * capture, int with_connections )
{
    for ( int i = 0; i < SPLIT_CAPTURE_NUM_NAMED; ++i )
        split_capture_emit_counter( capture, -1, split_capture_named_counters[i],
                                    capture->source.counter( capture->source.context, -1, split_capture_named_counters[i] ) );
    const int n = rocketnet_num_counters();
    for ( int counter = 0; counter < n; ++counter )
    {
        if ( split_capture_is_named( counter ) )
            continue;
        const uint64_t value = capture->source.counter( capture->source.context, -1, counter );
        if ( value )
            split_capture_emit_counter( capture, -1, counter, value );
    }
    if ( !with_connections || !capture->source.counters_per_connection )
        return;
    for ( int index = 0; index < capture->count; ++index )
        split_capture_connection_counters( capture, index );
}

/* Samples every connection when the interval is due. A connection that was
 * connected at its last sample and is not now was ended in between: its last
 * connected sample prints at once (why=kick), and the side's counters print
 * once per tick that saw a kick. Returns the kicks this tick found. */
static inline int split_capture_tick( struct split_capture_t * capture, double now )
{
    if ( now < capture->next_sample )
        return 0;
    capture->next_sample = now + SPLIT_CAPTURE_INTERVAL_S;
    int kicks = 0;
    for ( int index = 0; index < capture->count; ++index )
    {
        struct split_capture_entry_t * entry = &capture->entries[index];
        struct split_capture_sample_t sample;
        memset( &sample, 0, sizeof( sample ) );
        const int exists = capture->source.sample( capture->source.context, index, now, &sample );
        if ( exists && sample.delivery.connected )
        {
            split_capture_keep( capture, index, &sample );
            continue;
        }
        if ( entry->seen_connected && !entry->ended )
        {
            entry->ended = 1;
            kicks++;
            capture->kicks_printed++;
            split_capture_emit_entry( capture, index, "kick", now, sample.state, &entry->last );
            split_capture_connection_counters( capture, index );
        }
    }
    if ( kicks )
        split_capture_counters( capture, 0 );
    return kicks;
}

/* RUN's mark for one connection: when it was queued or received and, on the
 * server, the first sequence sent after it was queued (0 on a client). */
static inline void split_capture_mark_run( struct split_capture_t * capture, int index, double time, uint64_t sequence )
{
    if ( index < 0 || index >= capture->count )
        return;
    capture->entries[index].run_time = time;
    capture->entries[index].run_sequence = sequence;
}

/* The end of the run: every connection (a live one sampled now, an ended or
 * never-connected one from its last connected sample) and then the counters. */
static inline void split_capture_end( struct split_capture_t * capture, double now )
{
    for ( int index = 0; index < capture->count; ++index )
    {
        struct split_capture_entry_t * entry = &capture->entries[index];
        struct split_capture_sample_t sample;
        memset( &sample, 0, sizeof( sample ) );
        const int exists = capture->source.sample( capture->source.context, index, now, &sample );
        if ( exists && sample.delivery.connected )
        {
            split_capture_keep( capture, index, &sample );
            split_capture_emit_entry( capture, index, "end", now, sample.state, &sample );
        }
        else
            split_capture_emit_entry( capture, index, entry->seen_connected ? "end_ended" : "end_never", now, sample.state, &entry->last );
    }
    split_capture_counters( capture, 1 );
}

#endif // ROCKETNET_TOOLS_LOAD_SPLIT_CAPTURE_H
