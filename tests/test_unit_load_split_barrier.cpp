/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The load_split start barrier over a fake transport with caller time. Two
// shards of two clients start at different times, as shards on different
// hosts finish setup at different times. A lost control message is modelled
// as the SDK's reliable resend: it arrives one resend delay later for every
// loss. A dark client loses everything. Time starts at 1000.0.

#include "test_unit_registry.h"

#include "../tools/load_split_barrier.h"

#include <stdint.h>

#define check rocketnet_unit_check

#define BARRIER_SHARDS 2
#define BARRIER_SHARD_CLIENTS 2
#define BARRIER_CLIENTS ( BARRIER_SHARDS * BARRIER_SHARD_CLIENTS )
#define BARRIER_MESSAGES 256
#define BARRIER_TICK 0.01
#define BARRIER_LATENCY 0.05
#define BARRIER_RESEND_DELAY 2.0
#define BARRIER_CONNECT_WINDOW 10.0
#define BARRIER_RELIABLE_TIMEOUT 10.0
#define BARRIER_ACK 10
#define BARRIER_GO_ACK 11
#define BARRIER_END 12

struct barrier_message_t
{
    double arrive;
    int client, to_server, delivered, kind;
};

struct barrier_world_t
{
    double connected[BARRIER_SHARDS];
    int losses[BARRIER_CLIENTS][2];          // [client][to_server]: times each message is lost
    int dark_client;                         // every message to and from it is lost
    int server_silent;                       // the server never sends after ARM

    struct barrier_message_t messages[BARRIER_MESSAGES];
    int num_messages;

    struct split_startup_t clients[BARRIER_CLIENTS];
    int disconnected[BARRIER_CLIENTS];
    double disconnect_at[BARRIER_CLIENTS];

    int arm_acked[BARRIER_CLIENTS], go_acked[BARRIER_CLIENTS];
    enum split_start_step_t server_phase;
    double server_phase_start;
    enum split_start_verdict_t server_verdict;
    double server_verdict_time;

    struct split_shard_start_t shard_start[BARRIER_SHARDS];
    enum split_start_verdict_t shard_verdict[BARRIER_SHARDS];
    double shard_verdict_time[BARRIER_SHARDS];
    enum split_start_step_t shard_step[BARRIER_SHARDS];
};

static void barrier_world_init( struct barrier_world_t * world, double connected0, double connected1 )
{
    struct barrier_world_t zero = {};
    *world = zero;
    world->connected[0] = connected0;
    world->connected[1] = connected1;
    world->dark_client = -1;
    world->server_phase = SPLIT_START_STEP_ARM;
    world->server_phase_start = -1.0;
    for ( int client = 0; client < BARRIER_CLIENTS; ++client )
        world->disconnect_at[client] = -1.0;
}

static void barrier_send( struct barrier_world_t * world, double now, int client, int to_server, int kind )
{
    check( world->num_messages < BARRIER_MESSAGES );
    struct barrier_message_t * message = &world->messages[world->num_messages++];
    message->client = client;
    message->to_server = to_server;
    message->kind = kind;
    message->delivered = 0;
    if ( client == world->dark_client )
        message->arrive = 1.0e9;
    else
        message->arrive = now + BARRIER_LATENCY + BARRIER_RESEND_DELAY * (double) world->losses[client][to_server];
}

static void barrier_server_send_all( struct barrier_world_t * world, double now, int kind )
{
    for ( int client = 0; client < BARRIER_CLIENTS; ++client )
        barrier_send( world, now, client, 0, kind );
}

// The server gave up: it destroys every connection. A client the disconnect
// cannot reach times itself out one reliable timeout later.
static void barrier_server_abort( struct barrier_world_t * world, double now )
{
    for ( int client = 0; client < BARRIER_CLIENTS; ++client )
    {
        if ( client == world->dark_client )
            world->disconnect_at[client] = now + BARRIER_RELIABLE_TIMEOUT;
        else
            barrier_send( world, now, client, 0, BARRIER_END );
    }
}

static void barrier_deliver( struct barrier_world_t * world, double now )
{
    for ( int i = 0; i < world->num_messages; ++i )
    {
        struct barrier_message_t * message = &world->messages[i];
        if ( message->delivered || message->arrive > now )
            continue;
        message->delivered = 1;
        const int client = message->client;
        if ( message->to_server )
        {
            if ( message->kind == BARRIER_ACK ) { check( !world->arm_acked[client] ); world->arm_acked[client] = 1; }
            if ( message->kind == BARRIER_GO_ACK ) { check( !world->go_acked[client] ); world->go_acked[client] = 1; }
            continue;
        }
        if ( message->kind == BARRIER_END )
        {
            world->disconnected[client] = 1;
            continue;
        }
        // every client here is connected, so an answer is due at once
        check( split_startup_receive( &world->clients[client], message->kind ) );
        const int send = split_startup_due( &world->clients[client], 1 );
        if ( send == SPLIT_STARTUP_SEND_ACK )
            barrier_send( world, now, client, 1, BARRIER_ACK );
        if ( send == SPLIT_STARTUP_SEND_GO_ACK )
            barrier_send( world, now, client, 1, BARRIER_GO_ACK );
        split_startup_sent( &world->clients[client], send );
    }
    for ( int client = 0; client < BARRIER_CLIENTS; ++client )
        if ( world->disconnect_at[client] >= 0.0 && now >= world->disconnect_at[client] )
            world->disconnected[client] = 1;
}

static void barrier_server_update( struct barrier_world_t * world, double now )
{
    if ( world->server_verdict != SPLIT_START_WAITING )
        return;
    if ( world->server_phase_start < 0.0 )
    {
        // the population: ARM goes out once the last shard is connected
        const double last = world->connected[0] > world->connected[1] ? world->connected[0] : world->connected[1];
        if ( now < last )
            return;
        world->server_phase_start = now;
        barrier_server_send_all( world, now, SPLIT_STARTUP_ARM );
        return;
    }
    if ( world->server_silent )
        return;
    int acked = 0;
    for ( int client = 0; client < BARRIER_CLIENTS; ++client )
        acked += world->server_phase == SPLIT_START_STEP_ARM ? world->arm_acked[client] : world->go_acked[client];
    const enum split_start_verdict_t verdict = split_server_start_update( now, world->server_phase_start, BARRIER_CLIENTS, acked, 0 );
    if ( verdict == SPLIT_START_WAITING )
        return;
    if ( verdict != SPLIT_START_COMPLETE )
    {
        world->server_verdict = verdict;
        world->server_verdict_time = now;
        barrier_server_abort( world, now );
        return;
    }
    if ( world->server_phase == SPLIT_START_STEP_ARM )
    {
        world->server_phase = SPLIT_START_STEP_GO;
        world->server_phase_start = now;
        barrier_server_send_all( world, now, SPLIT_STARTUP_GO );
        return;
    }
    barrier_server_send_all( world, now, SPLIT_STARTUP_RUN );
    world->server_verdict = SPLIT_START_COMPLETE;
    world->server_verdict_time = now;
}

static void barrier_shard_update( struct barrier_world_t * world, int shard, double now )
{
    if ( now < world->connected[shard] || world->shard_verdict[shard] != SPLIT_START_WAITING )
        return;
    int running = 0, disconnected = 0;
    enum split_start_step_t step = SPLIT_START_STEP_RUNNING;
    for ( int local = 0; local < BARRIER_SHARD_CLIENTS; ++local )
    {
        const int client = shard * BARRIER_SHARD_CLIENTS + local;
        running += world->clients[client].running;
        disconnected += world->disconnected[client];
        const enum split_start_step_t client_step = split_start_client_step( &world->clients[client] );
        if ( client_step < step )
            step = client_step;
    }
    const enum split_start_verdict_t verdict =
        split_shard_start_update( &world->shard_start[shard], now, BARRIER_SHARD_CLIENTS, running, disconnected );
    if ( verdict == SPLIT_START_WAITING )
        return;
    world->shard_verdict[shard] = verdict;
    world->shard_verdict_time[shard] = now;
    world->shard_step[shard] = step;
}

static void barrier_run( struct barrier_world_t * world )
{
    for ( int shard = 0; shard < BARRIER_SHARDS; ++shard )
        split_shard_start_begin( &world->shard_start[shard], world->connected[shard],
                                 BARRIER_CONNECT_WINDOW, BARRIER_RELIABLE_TIMEOUT );
    for ( int64_t tick = 0; tick <= 10000; ++tick )
    {
        const double now = 1000.0 + (double) tick * BARRIER_TICK;
        barrier_deliver( world, now );
        barrier_server_update( world, now );
        for ( int shard = 0; shard < BARRIER_SHARDS; ++shard )
            barrier_shard_update( world, shard, now );
        if ( world->shard_verdict[0] != SPLIT_START_WAITING && world->shard_verdict[1] != SPLIT_START_WAITING )
            return;
    }
}

static void test_load_split_start_slow_shard_inside_server_window()
{
    // the second shard connects 9.99 s after the first, inside the server's
    // 10 s population window: ARM reaches the first shard more than 10 s
    // after its own connection, and it waits for it
    struct barrier_world_t world;
    barrier_world_init( &world, 1000.0, 1009.99 );
    barrier_run( &world );
    check( world.shard_verdict[0] == SPLIT_START_COMPLETE );
    check( world.shard_verdict[1] == SPLIT_START_COMPLETE );
    check( world.server_verdict == SPLIT_START_COMPLETE );
}

static void test_load_split_start_lost_packets_are_delay()
{
    // every ARM to the first shard is lost three times and every GO ACK from
    // the second shard twice; the resends land inside the server windows
    struct barrier_world_t world;
    barrier_world_init( &world, 1000.0, 1004.0 );
    world.losses[0][0] = 3;
    world.losses[1][0] = 3;
    world.losses[2][1] = 2;
    world.losses[3][1] = 2;
    barrier_run( &world );
    check( world.shard_verdict[0] == SPLIT_START_COMPLETE );
    check( world.shard_verdict[1] == SPLIT_START_COMPLETE );
    check( world.server_verdict == SPLIT_START_COMPLETE );
}

static void test_load_split_start_dark_client_fails_at_server_window()
{
    // a client whose path loses everything: the server fails ARM at its
    // window and ends every connection; both shards stop on the disconnect,
    // and the dark client's shard still lags at arm
    struct barrier_world_t world;
    barrier_world_init( &world, 1000.0, 1001.0 );
    world.dark_client = 3;
    barrier_run( &world );
    check( world.server_verdict == SPLIT_START_TIMED_OUT );
    check( world.server_verdict_time >= 1011.0 && world.server_verdict_time < 1011.0 + 2.0 * BARRIER_TICK );
    check( world.arm_acked[0] && world.arm_acked[1] && world.arm_acked[2] && !world.arm_acked[3] );
    check( world.shard_verdict[0] == SPLIT_START_DISCONNECTED );
    check( world.shard_verdict[1] == SPLIT_START_DISCONNECTED );
    check( world.shard_step[1] == SPLIT_START_STEP_ARM );
    check( world.shard_verdict_time[1] < world.shard_start[1].deadline );
}

static void test_load_split_start_silent_server_fails_at_shard_deadline()
{
    // the server never answers after ARM: each shard fails on its own clock,
    // at its one bounded deadline
    struct barrier_world_t world;
    barrier_world_init( &world, 1000.0, 1001.0 );
    world.server_silent = 1;
    barrier_run( &world );
    const double deadline = 1000.0 + BARRIER_CONNECT_WINDOW + 2.0 * SPLIT_START_DEADLINE_S + BARRIER_RELIABLE_TIMEOUT;
    check( world.shard_verdict[0] == SPLIT_START_TIMED_OUT );
    check( world.shard_step[0] == SPLIT_START_STEP_GO );
    check( world.shard_verdict_time[0] >= deadline && world.shard_verdict_time[0] < deadline + 2.0 * BARRIER_TICK );
    check( world.shard_verdict[1] == SPLIT_START_TIMED_OUT );
}

REGISTER_UNIT_TEST( test_load_split_start_slow_shard_inside_server_window );
REGISTER_UNIT_TEST( test_load_split_start_lost_packets_are_delay );
REGISTER_UNIT_TEST( test_load_split_start_dark_client_fails_at_server_window );
REGISTER_UNIT_TEST( test_load_split_start_silent_server_fails_at_shard_deadline );
