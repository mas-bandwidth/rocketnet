#pragma once

#ifndef ROCKETNET_TOOLS_LOAD_SPLIT_STARTUP_H
#define ROCKETNET_TOOLS_LOAD_SPLIT_STARTUP_H

/* A split client's side of the start barrier: ARM, GO and RUN arrive from the
 * server; ARM is answered with an ACK and GO with a GO ACK. */

enum { SPLIT_STARTUP_ARM = 1, SPLIT_STARTUP_GO = 2, SPLIT_STARTUP_RUN = 3 };
enum { SPLIT_STARTUP_SEND_NONE = 0, SPLIT_STARTUP_SEND_ACK = 1, SPLIT_STARTUP_SEND_GO_ACK = 2 };

struct split_startup_t
{
    int arm_received, armed, go_received, go, running;
};

/* 1 when the control message is the next one expected, 0 when it is a
 * protocol error (a repeat, or one ahead of its turn). */
static inline int split_startup_receive( struct split_startup_t * startup, int kind )
{
    if ( kind == SPLIT_STARTUP_ARM && !startup->arm_received ) { startup->arm_received = 1; return 1; }
    if ( kind == SPLIT_STARTUP_GO && startup->armed && !startup->go_received ) { startup->go_received = 1; return 1; }
    if ( kind == SPLIT_STARTUP_RUN && startup->go && !startup->running ) { startup->running = 1; return 1; }
    return 0;
}

/* The answer owed now. The client transport hands payloads from before it
 * reports CONNECTED, and a send before CONNECTED is dropped at start (never
 * queued), so an answer waits until the client is connected. */
static inline int split_startup_due( const struct split_startup_t * startup, int client_connected )
{
    if ( !client_connected ) return SPLIT_STARTUP_SEND_NONE;
    if ( startup->arm_received && !startup->armed ) return SPLIT_STARTUP_SEND_ACK;
    if ( startup->go_received && !startup->go ) return SPLIT_STARTUP_SEND_GO_ACK;
    return SPLIT_STARTUP_SEND_NONE;
}

static inline void split_startup_sent( struct split_startup_t * startup, int send )
{
    if ( send == SPLIT_STARTUP_SEND_ACK ) startup->armed = 1;
    else if ( send == SPLIT_STARTUP_SEND_GO_ACK ) startup->go = 1;
}

#endif // ROCKETNET_TOOLS_LOAD_SPLIT_STARTUP_H
