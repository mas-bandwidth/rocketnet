#pragma once

#ifndef ROCKETNET_TOOLS_LOAD_SPLIT_BARRIER_H
#define ROCKETNET_TOOLS_LOAD_SPLIT_BARRIER_H

#include "load_split_startup.h"

/* The split start barrier with caller time, so the unit suite drives both
 * roles over a fake transport. The server sends ARM once every client is
 * connected, GO once every ARM is acknowledged, and RUN once every GO is
 * acknowledged; each control message is reliable ordered, so a lost packet
 * is the SDK's resend and costs delay, never the message. A client answers
 * ARM and GO once it is connected (tools/load_split_startup.h).
 * proofs/tla/SplitStart.tla models the windows and checks that a shard gives up on
 * its own clock only after the server has failed the start. */

/* The server waits this long for every ARM ACK, then again for every GO ACK. */
#define SPLIT_START_DEADLINE_S 10.0

/* Names cap the clients a start failure lists; the count is always whole. */
#define SPLIT_START_NAMED_CLIENTS 32

enum split_start_step_t
{
    SPLIT_START_STEP_ARM,
    SPLIT_START_STEP_GO,
    SPLIT_START_STEP_RUN,
    SPLIT_START_STEP_RUNNING
};

enum split_start_verdict_t
{
    SPLIT_START_WAITING,
    SPLIT_START_COMPLETE,
    SPLIT_START_TIMED_OUT,
    SPLIT_START_DISCONNECTED
};

struct split_shard_start_t
{
    double deadline;
};

static inline const char * split_start_step_name( enum split_start_step_t step )
{
    switch ( step )
    {
        case SPLIT_START_STEP_ARM: return "arm";
        case SPLIT_START_STEP_GO:  return "go";
        case SPLIT_START_STEP_RUN: return "run";
        default:                   return "running";
    }
}

static inline const char * split_start_verdict_name( enum split_start_verdict_t verdict )
{
    switch ( verdict )
    {
        case SPLIT_START_WAITING:   return "waiting";
        case SPLIT_START_COMPLETE:  return "complete";
        case SPLIT_START_TIMED_OUT: return "timed_out";
        default:                    return "disconnected";
    }
}

/* The earliest start step a client has not passed: its ACK or GO ACK counts
 * once it is sent, not when ARM or GO arrives. */
static inline enum split_start_step_t split_start_client_step( const struct split_startup_t * startup )
{
    if ( startup->running )
        return SPLIT_START_STEP_RUNNING;
    if ( startup->go )
        return SPLIT_START_STEP_RUN;
    if ( startup->armed )
        return SPLIT_START_STEP_GO;

    return SPLIT_START_STEP_ARM;
}

/* A shard waits for RUN against one deadline from its own connection: the
 * server's population window, both server start windows, and one reliable
 * message timeout for RUN itself. Every server window closes first, so the
 * server is the one authority that fails a start, and the shard's clock never
 * cuts short a start the server would complete. Shards finish setup at
 * different times, and the first to connect waits for the last. */
static inline void split_shard_start_begin( struct split_shard_start_t * start, double connected_time,
                                            double connect_window_s, double reliable_timeout_s )
{
    start->deadline = connected_time + connect_window_s + 2.0 * SPLIT_START_DEADLINE_S + reliable_timeout_s;
}

/* A client that leaves the connected state ends the wait at once: the server
 * gave up on it, or the path did. */
static inline enum split_start_verdict_t split_shard_start_update( const struct split_shard_start_t * start, double now,
                                                                   int count, int running, int disconnected )
{
    if ( disconnected )
        return SPLIT_START_DISCONNECTED;
    if ( running == count )
        return SPLIT_START_COMPLETE;
    if ( now >= start->deadline )
        return SPLIT_START_TIMED_OUT;

    return SPLIT_START_WAITING;
}

/* The server waits one window per phase from the send of ARM or GO. */
static inline enum split_start_verdict_t split_server_start_update( double now, double phase_start,
                                                                    int total, int acked, int disconnected )
{
    if ( disconnected )
        return SPLIT_START_DISCONNECTED;
    if ( acked == total )
        return SPLIT_START_COMPLETE;
    if ( now - phase_start >= SPLIT_START_DEADLINE_S )
        return SPLIT_START_TIMED_OUT;

    return SPLIT_START_WAITING;
}

#endif // ROCKETNET_TOOLS_LOAD_SPLIT_BARRIER_H
