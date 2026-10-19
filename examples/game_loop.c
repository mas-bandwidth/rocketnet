/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop: a 60 Hz server and N clients in one process over the loopback
    transport. Every frame each client sends a small unreliable-sequenced
    state packet and a reliable-ordered event now and then; the server echoes
    each client a snapshot. It measures each client's per-frame network cost
    against the 100 microsecond budget and the server's tick time, and checks
    the frame order by measuring end-to-end message latency in frames. With
    the correct order a message queued in frame N leaves in frame N and is
    received the first frame the other side's update sees it (0 extra
    frames); --wrong-order sends early so every message waits one frame.

    Run it from the repository root: ./bin/example_game_loop
    The header at this base still declares start, finish, receive, release and
    send_packets (the lifecycle rename to *_message lands later), so this
    example uses the header's names.
*/

#include "rocketnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK( c ) do { if ( !( c ) ) { printf( "FAIL: %s\n", #c ); goto cleanup; } } while ( 0 )

typedef struct { uint64_t frame; float x, y, z; } state_t;

static int cmp_d( const void * a, const void * b )
{
    double x = *(const double*) a, y = *(const double*) b;
    return ( x > y ) - ( x < y );
}

int main( int argc, char ** argv )
{
    int clients = 32, seconds = 5, wrong = 0;
    for ( int i = 1; i < argc; i++ )
    {
        if ( !strcmp( argv[i], "--wrong-order" ) ) wrong = 1;
        else if ( !strcmp( argv[i], "--clients" ) && i + 1 < argc ) clients = atoi( argv[++i] );
        else if ( !strcmp( argv[i], "--seconds" ) && i + 1 < argc ) seconds = atoi( argv[++i] );
        else { printf( "usage: %s [--clients N] [--seconds S] [--wrong-order]\n", argv[0] ); return 1; }
    }
    if ( clients < 1 || clients > 1000 )
    {
        printf( "usage: %s [--clients N] [--seconds S] [--wrong-order]\n", argv[0] );
        return 1;
    }
    if ( seconds < 1 ) seconds = 1;

    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client[1000];
    struct rocketnet_client_config_t ccfg;
    struct rocketnet_server_config_t scfg;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char addr[64];
    uint64_t events_sent[1000], events_seen[1000];
    int had_connected[1000], lost[1000];
    double * samples = NULL, * stick = NULL;
    unsigned long nsamp = 0, nstick = 0;
    int result = 1;
    for ( int i = 0; i < 1000; i++ ) { client[i] = NULL; events_sent[i] = events_seen[i] = 0; had_connected[i] = lost[i] = 0; }

    const unsigned long nsamp_cap = (unsigned long) clients * (unsigned long) seconds * 60ul + 16ul;
    const unsigned long nstick_cap = (unsigned long) seconds * 60ul + 16ul;
    samples = (double*) malloc( nsamp_cap * sizeof( double ) );
    stick = (double*) malloc( nstick_cap * sizeof( double ) );
    CHECK( samples && stick );

    CHECK( rocketnet_init( NULL ) );
    rocketnet_server_dev_config( &scfg );
    scfg.max_clients = clients;
    scfg.transport = "loopback";
    scfg.num_reassembly_slots = 2;
    scfg.receive_ring_bytes = 4096;
    scfg.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    scfg.send_buffer_bytes = 12288;
    scfg.send_queue_messages = 16;
    scfg.packets_in_flight = 16;
    scfg.receipt_ring = 16;
    scfg.window_messages = 64;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", &scfg, NULL, 0 );
    CHECK( server != NULL );
    snprintf( addr, sizeof( addr ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );

    rocketnet_client_default_config( &ccfg );
    ccfg.transport = "loopback";
    ccfg.num_reassembly_slots = 2;
    ccfg.receive_ring_bytes = 4096;
    ccfg.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    ccfg.send_buffer_bytes = 12288;
    ccfg.send_queue_messages = 16;
    ccfg.packets_in_flight = 16;
    ccfg.receipt_ring = 16;
    ccfg.window_messages = 64;
    for ( int i = 0; i < clients; i++ )
    {
        CHECK( rocketnet_write_dev_connect_token( addr, (uint64_t) i, 4102444800ull, token ) );
        client[i] = rocketnet_client_create( NULL, token, &ccfg, NULL, 0 );
        CHECK( client[i] != NULL );
    }

    const double dt = 1.0 / 60.0;
    const double t0 = rocketnet_time();
    uint64_t frame = 0;
    double min_extra = 1e18, sum_extra = 0.0;
    unsigned long n_extra = 0;

    while ( rocketnet_time() - t0 < (double) seconds )
    {
        const double now = 1000.0 + (double) frame * dt;

        for ( int i = 0; i < clients; i++ )
        {
            const double tc = rocketnet_time();
            if ( rocketnet_client_update( client[i], now ) != ROCKETNET_MESSAGE_OK ) { printf( "FAIL: client update\n" ); goto cleanup; }
            if ( wrong ) rocketnet_client_send_packets( client[i] );   // wrong order: send before queueing
            if ( rocketnet_client_state( client[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                had_connected[i] = 1;
                const uint8_t * data = NULL;
                int bytes = 0;
                while ( rocketnet_client_receive( client[i], 0, &data, &bytes ) )
                {
                    if ( bytes >= (int) sizeof( state_t ) )
                    {
                        state_t s; memcpy( &s, data, sizeof s );
                        const double d = (double)( frame - s.frame );
                        if ( d < min_extra ) min_extra = d;
                        sum_extra += d; n_extra++;
                    }
                    rocketnet_client_release( client[i], data );
                }
                state_t stmsg; stmsg.frame = frame; stmsg.x = 0.01f * (float) i; stmsg.y = stmsg.z = 0.0f;
                uint8_t * b = rocketnet_client_start( client[i], 0, ROCKETNET_UNRELIABLE_SEQUENCED, (int) sizeof( state_t ) );
                if ( b ) { memcpy( b, &stmsg, sizeof stmsg ); rocketnet_client_finish( client[i], (int) sizeof( state_t ) ); }
                if ( frame % 60 == 0 )
                {
                    b = rocketnet_client_start( client[i], 1, ROCKETNET_RELIABLE_ORDERED, 8 );
                    if ( b ) { uint64_t ef = frame; memcpy( b, &ef, 8 ); rocketnet_client_finish( client[i], 8 ); events_sent[i]++; }
                }
                if ( !wrong ) rocketnet_client_send_packets( client[i] );   // last, so this frame's messages leave this frame
            }
            else if ( had_connected[i] ) lost[i] = 1;
            if ( nsamp < nsamp_cap ) samples[nsamp++] = ( rocketnet_time() - tc ) * 1e6;
        }

        {
            const double ts = rocketnet_time();
            if ( rocketnet_server_update( server, now ) != ROCKETNET_MESSAGE_OK ) { printf( "FAIL: server update\n" ); goto cleanup; }
            for ( int i = 0; i < clients; i++ )
            {
                if ( !rocketnet_server_client_connected( server, i ) ) continue;
                const uint8_t * data = NULL;
                int bytes = 0;
                while ( rocketnet_server_receive( server, i, 0, &data, &bytes ) )
                {
                    if ( bytes >= (int) sizeof( state_t ) )
                    {
                        state_t s; memcpy( &s, data, sizeof( state_t ) );
                        const double d = (double)( frame - s.frame );
                        if ( d < min_extra ) min_extra = d;
                        sum_extra += d; n_extra++;
                    }
                    rocketnet_server_release( server, i, data );
                }
                while ( rocketnet_server_receive( server, i, 1, &data, &bytes ) ) { events_seen[i]++; rocketnet_server_release( server, i, data ); }
                state_t snap; snap.frame = frame; snap.x = snap.y = snap.z = 0.0f;
                uint8_t * b = rocketnet_server_start( server, i, 0, ROCKETNET_UNRELIABLE_SEQUENCED, (int) sizeof( state_t ) );
                if ( b ) { memcpy( b, &snap, sizeof snap ); rocketnet_server_finish( server, i, (int) sizeof( state_t ) ); }
            }
            rocketnet_server_send_packets( server );   // last, so this frame's messages leave this frame
            if ( nstick < nstick_cap ) stick[nstick++] = ( rocketnet_time() - ts ) * 1e6;
        }

        if ( frame > 0 && frame % 60 == 0 )
        {
            if ( nsamp > 1 ) qsort( samples, nsamp, sizeof( double ), cmp_d );
            double sm = 0.0; for ( unsigned long k = 0; k < nstick; k++ ) sm += stick[k];
            printf( "t=%llus clients=%d p50=%.0fus p99=%.0fus max=%.0fus server_tick=%.0fus\n",
                    (unsigned long long) frame / 60, clients, samples[(nsamp-1)/2],
                    samples[(unsigned long)(0.99*(nsamp-1))], samples[nsamp-1], nstick ? sm / nstick : 0.0 );
        }

        frame++;
        const double remaining = ( t0 + (double) frame * dt ) - rocketnet_time();
        if ( remaining > 0.0 ) rocketnet_sleep( remaining );
    }

    if ( nsamp > 1 ) qsort( samples, nsamp, sizeof( double ), cmp_d );
    double sm = 0.0; for ( unsigned long k = 0; k < nstick; k++ ) sm += stick[k];
    const double p50 = nsamp ? samples[(nsamp-1)/2] : 0.0;
    const double p99 = nsamp ? samples[(unsigned long)(0.99*(nsamp-1))] : 0.0;
    const double mx = nsamp ? samples[nsamp-1] : 0.0;
    const double stick_mean = nstick ? sm / nstick : 0.0;
    printf( "per-client per-frame p50=%.0fus p99=%.0fus max=%.0fus (budget 100us %s)\n",
            p50, p99, mx, p99 <= 100.0 ? "held" : "exceeded" );
    printf( "server_tick=%.0fus\n", stick_mean );
    const unsigned long extras = n_extra ? (unsigned long) min_extra : 0;
    printf( "latency: %lu extra frame(s) (%s order)\n", extras, wrong ? "wrong" : "correct" );

    int ok = 1;
    for ( int i = 0; i < clients; i++ )
        if ( !had_connected[i] || lost[i] ) { printf( "FAIL: client %d did not stay connected\n", i ); ok = 0; }
    for ( int i = 0; i < clients; i++ )
        if ( events_seen[i] < events_sent[i] ) { printf( "FAIL: client %d lost an event\n", i ); ok = 0; }
    if ( wrong ) { if ( extras < 1 ) { printf( "FAIL: wrong order no extra frame\n" ); ok = 0; } }
    else { if ( extras != 0 ) { printf( "FAIL: correct order extra frames\n" ); ok = 0; } }
    if ( !ok ) goto cleanup;

    printf( "PASS\n" );
    result = 0;

cleanup:
    free( samples );
    free( stick );
    for ( int i = 0; i < clients; i++ ) rocketnet_client_destroy( client[i] );
    rocketnet_server_destroy( server );
    rocketnet_term();
    return result;
}
