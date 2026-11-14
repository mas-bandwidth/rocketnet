/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The wide fill: the game's own threads fill and seal each client's sends
// between update and send_packets, one thread per client at a time
// (sdk/rocketnet_fill_phase.h, proofs/tla/RNFillPhase.tla), each holding one
// open message on its own client (no model yet: roadmap message-open-model).
//
//   - the concurrent fill: N threads, each owning a disjoint set of clients
//     that moves every frame, start and finish unreliable, unreliable
//     sequenced and reliable ordered messages through the public API against
//     a server on the in-process loopback transport; every client checks
//     every byte and the order of what it receives, and every reliable
//     message arrives exactly once in order. Run under TSan and ASan+UBSan.
//   - the check itself, driven step by step: an update inside a fill call,
//     a fill call inside a main-thread operation, two fillers on one client,
//     a client that moves to another thread in the next fill phase (no false
//     alarm), each refused and counted (and asserted in a debug build).
//   - one open message per client: two opens on one client refused, opens on
//     different clients independent, the one-thread finish refusing while
//     two are open, and a send_packets while a message is open closing it.
//
// No socket, no real time: the server and clients run on a loopback hub and
// the clock is the test's. Threads are joined before every main-thread call,
// which is the contract's handoff.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "token_string.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_counters.h"
#include "rocketnet_fill_phase.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#define check rocketnet_unit_check

#define WIDE_FILL_PROTOCOL 0x57494445F1110001ULL
#define WIDE_FILL_MAX_CLIENTS 16
#define WIDE_FILL_FULL_SERVER_CLIENTS 1000         // the full-server case: every slot of a 1000-client server connected

static double g_wide_fill_now = 1000.0;
static double wide_fill_clock( void * ) { return g_wide_fill_now; }

static int g_wide_fill_asserts = 0;
static void wide_fill_assert_hook( const char *, const char *, const char *, int )
{
    g_wide_fill_asserts++;
}

// the asserts a check fires in a debug build, none in a release build
static int wide_fill_expected_asserts( int fired_in_debug )
{
#if ROCKETNET_ASSERTS
    return fired_in_debug;
#else // #if ROCKETNET_ASSERTS
    (void) fired_in_debug;
    return 0;
#endif // #if ROCKETNET_ASSERTS
}

struct wide_fill_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_loopback_hub_t * hub;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[WIDE_FILL_FULL_SERVER_CLIENTS];
    int num_clients;
    double time;
};

static void wide_fill_world_create( wide_fill_world_t * world, int num_clients, int packets_per_update = 0, int max_clients = WIDE_FILL_MAX_CLIENTS )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    g_wide_fill_now = world->time;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3D ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );

    world->hub = rocketnet_loopback_hub_create( NULL, max_clients, NULL );
    check( world->hub != NULL );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = WIDE_FILL_PROTOCOL;
    server_config.max_clients = max_clients;
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = world->hub;
    if ( max_clients > WIDE_FILL_MAX_CLIENTS )
    {
        server_config.handshakes_per_update = 1024;     // the full server connects in a few steps, not two clients a step
    }
    if ( packets_per_update > 0 )
    {
        server_config.packets_per_update = packets_per_update;
    }

    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world->server != NULL );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    world->num_clients = num_clients;
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_config_t client_config;
        rocketnet_client_default_config( &client_config );
        client_config.transport = "loopback";
        client_config.loopback_hub = world->hub;
        client_config.step_driven = true;
        client_config.time_callback = wide_fill_clock;
        world->clients[i] = rocketnet_client_create( NULL, &client_config, NULL );
        check( world->clients[i] != NULL );
    }
}

static void wide_fill_world_destroy( wide_fill_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rn_test_client_stop_and_destroy( world->clients[i] );
    }
    rn_test_server_stop_and_destroy( world->server );
    check( rocketnet_loopback_hub_destroy( world->hub ) == ROCKETNET_STATUS_OK );
}

// the server's counter, read from its transport
static uint64_t wide_fill_counter( rocketnet_server_t * server, int counter )
{
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( server ) );
    const rocketnet_counters_t * counters = rocketnet_server_transport_counters( transport );
    return counters ? counters->values[counter] : 0;
}

static void wide_fill_clients_step( wide_fill_world_t * world )
{
    g_wide_fill_now = world->time;
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rocketnet_client_update( world->clients[i], world->time );
        rocketnet_client_send_packets( world->clients[i] );
    }
}

// a whole frame with no fill: clients, then the server's send_packets and update
static void wide_fill_pump( wide_fill_world_t * world )
{
    wide_fill_clients_step( world );
    rocketnet_server_send_packets( world->server );
    rocketnet_server_update( world->server, world->time );
    while ( rocketnet_server_receive_messages( world->server ) != NULL )
    {
    }
    for ( int i = 0; i < world->num_clients; i++ )
    {
        while ( rocketnet_client_receive_messages( world->clients[i] ) != NULL )
        {
        }
    }
    world->time += 1.0 / 60.0;
}

// the game names a client: the thread rules are checked from here (the server
// arms its check at the first rocketnet_server_abort_client_message or
// rocketnet_server_finish_client_message; no message is open, so this one
// does nothing else)
static void wide_fill_opt_in( wide_fill_world_t * world )
{
    rocketnet_server_abort_client_message( world->server, 0 );
}

static void wide_fill_connect_all( wide_fill_world_t * world )
{
    for ( int c = 0; c < world->num_clients; c++ )
    {
        char address[64];
        snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
        const char * addresses[] = { address };
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        check( rn_test_server_token_string( world->backend_secret, world->server, WIDE_FILL_PROTOCOL, addresses, 1,
                                            (uint64_t) ( 0x5100 + c ), 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
        check( rn_test_client_connect_after_completion( world->clients[c], token ) == ROCKETNET_STATUS_OK );
    }

    for ( int step = 0; step < 600 + world->num_clients; step++ )   // at the default handshake rate a server connects about two clients a step
    {
        int connected = 0;
        for ( int c = 0; c < world->num_clients; c++ )
        {
            const int index = rocketnet_client_index( world->clients[c] );
            if ( rocketnet_client_state( world->clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED && index >= 0 &&
                 rocketnet_server_client_connected( world->server, index ) )
            {
                connected++;
            }
        }
        if ( connected == world->num_clients )
        {
            break;
        }
        wide_fill_pump( world );
    }

    for ( int c = 0; c < world->num_clients; c++ )
    {
        check( rocketnet_client_state( world->clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( world->server, rocketnet_client_index( world->clients[c] ) ) );
    }
}

// ---------------------------------------------------------------------------
// the concurrent fill
// ---------------------------------------------------------------------------

// a message: [kind][slot][seq:4][frame:4] then bytes derived from all three
#define WIDE_FILL_HEADER_BYTES 10

static uint8_t wide_fill_byte( int kind, int slot, uint32_t seq, int i )
{
    return (uint8_t) ( ( kind * 131 ) ^ ( slot * 29 ) ^ (int) ( seq * 7u ) ^ ( i * 13 ) );
}

static void wide_fill_write( uint8_t * p, int bytes, int kind, int slot, uint32_t seq, uint32_t frame )
{
    p[0] = (uint8_t) kind;
    p[1] = (uint8_t) slot;
    memcpy( p + 2, &seq, 4 );
    memcpy( p + 6, &frame, 4 );
    for ( int i = WIDE_FILL_HEADER_BYTES; i < bytes; i++ )
    {
        p[i] = wide_fill_byte( kind, slot, seq, i );
    }
}

static const int wide_fill_kinds[3] = { ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_RELIABLE_ORDERED };

struct wide_fill_slot_t
{
    uint32_t sent[3];           // per kind, the next sequence the filler gives
    uint32_t refused[3];        // starts the server refused (no room, budget): never written
    uint32_t received[3];       // per kind, messages the client took
    uint32_t next_reliable;     // the reliable sequence the client expects next
    int64_t last_sequenced;     // the last unreliable sequenced seq the client took
    int64_t last_unreliable;    // the last unreliable seq the client took
    uint32_t corrupt;
};

struct wide_fill_frame_t
{
    rocketnet_server_t * server;
    wide_fill_slot_t * slots;
    const int * slot_of_client;     // the server slot of each world client
    int num_clients;
    int num_threads;
    int frame;
    int unreliable_per_frame;
    int bytes;
    bool use_one_thread_finish;     // the finish that names no client: only with one thread
};

static void wide_fill_job( const wide_fill_frame_t * f, int thread )
{
    for ( int c = 0; c < f->num_clients; c++ )
    {
        if ( ( c + f->frame ) % f->num_threads != thread )
        {
            continue;                                   // another thread's client this frame
        }

        const int slot = f->slot_of_client[c];
        wide_fill_slot_t * s = &f->slots[slot];
        for ( int m = 0; m < f->unreliable_per_frame + 2; m++ )
        {
            const int k = m < f->unreliable_per_frame ? 0 : ( m == f->unreliable_per_frame ? 1 : 2 );
            const int bytes = k == 2 ? WIDE_FILL_HEADER_BYTES + 6 : f->bytes - ( m % 3 );
            uint8_t * p = rocketnet_server_start_message( f->server, (uint64_t) slot, 0,
                                                          (enum rocketnet_reliability_t) wide_fill_kinds[k], bytes );
            if ( !p )
            {
                s->refused[k]++;
                continue;
            }
            wide_fill_write( p, bytes, k, slot, s->sent[k], (uint32_t) f->frame );
            const uint32_t serial = f->use_one_thread_finish ? rocketnet_server_finish_message( f->server, bytes )
                                                             : rocketnet_server_finish_client_message( f->server, slot, bytes );
            if ( serial == 0 )
            {
                s->refused[k]++;
                continue;
            }
            s->sent[k]++;
        }
    }
}

static void wide_fill_client_receive( wide_fill_world_t * world, int c, wide_fill_slot_t * s, int slot )
{
    const rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_client_receive_messages( world->clients[c] ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            const uint8_t * d = batch->data[i];
            const int bytes = batch->bytes[i];
            if ( bytes < WIDE_FILL_HEADER_BYTES || d[0] > 2 || d[1] != (uint8_t) slot )
            {
                s->corrupt++;
                continue;
            }
            const int k = d[0];
            uint32_t seq = 0;
            memcpy( &seq, d + 2, 4 );
            for ( int b = WIDE_FILL_HEADER_BYTES; b < bytes; b++ )
            {
                if ( d[b] != wide_fill_byte( k, slot, seq, b ) )
                {
                    s->corrupt++;
                    break;
                }
            }
            s->received[k]++;
            if ( k == 2 )
            {
                if ( seq != s->next_reliable )
                {
                    s->corrupt++;
                }
                s->next_reliable = seq + 1;
            }
            else if ( k == 1 )
            {
                if ( (int64_t) seq <= s->last_sequenced )
                {
                    s->corrupt++;
                }
                s->last_sequenced = seq;
            }
            else
            {
                if ( (int64_t) seq <= s->last_unreliable )
                {
                    s->corrupt++;              // one sender thread per client per frame: its own order holds
                }
                s->last_unreliable = seq;
            }
        }
    }
}

// the server's update: one call, or the three calls with the game threads
// each taking the clients whose index falls to them (every index asked for
// by exactly one thread; a client nobody took would be left to the finish)
static void wide_fill_server_update( wide_fill_world_t * world, bool three_calls, int num_threads, uint64_t * applied )
{
    if ( !three_calls )
    {
        rocketnet_server_update( world->server, world->time );
        return;
    }

    rocketnet_server_update_receive( world->server, world->time );
    uint64_t taken[8] = {};
    std::vector<std::thread> threads;
    for ( int t = 0; t < num_threads; t++ )
    {
        threads.emplace_back( [world, num_threads, t, &taken]()
                              {
                                  for ( int c = t; c < WIDE_FILL_MAX_CLIENTS; c += num_threads )
                                  {
                                      taken[t] += (uint64_t) rocketnet_server_update_client( world->server, c );
                                  } } );
    }
    for ( std::thread & thread : threads )
    {
        thread.join();
    }
    for ( int t = 0; t < num_threads; t++ )
    {
        *applied += taken[t];
    }
    rocketnet_server_update_finish( world->server );
}

static void wide_fill_run( int num_threads, int frames, int unreliable_per_frame, int bytes, bool one_thread_finish, bool three_calls = false )
{
    uint64_t applied = 0;
    const uint64_t misuse = rocketnet_api_misuse_count();
    wide_fill_world_t world;
    wide_fill_world_create( &world, WIDE_FILL_MAX_CLIENTS );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );

    wide_fill_slot_t slots[WIDE_FILL_MAX_CLIENTS];
    memset( slots, 0, sizeof( slots ) );
    int slot_of_client[WIDE_FILL_MAX_CLIENTS];
    for ( int c = 0; c < world.num_clients; c++ )
    {
        slot_of_client[c] = rocketnet_client_index( world.clients[c] );
        slots[slot_of_client[c]].last_sequenced = -1;
        slots[slot_of_client[c]].last_unreliable = -1;
    }

    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    const uint64_t two = wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS );

    for ( int frame = 0; frame < frames; frame++ )
    {
        // main: the frame just filled goes out, then update and receive

        rocketnet_server_send_packets( world.server );
        wide_fill_clients_step( &world );
        wide_fill_server_update( &world, three_calls, num_threads, &applied );
        while ( rocketnet_server_receive_messages( world.server ) != NULL )
        {
        }
        for ( int c = 0; c < world.num_clients; c++ )
        {
            wide_fill_client_receive( &world, c, &slots[slot_of_client[c]], slot_of_client[c] );
        }

        // the fill: the game's threads, joined before the next main-thread call

        wide_fill_frame_t f;
        f.server = world.server;
        f.slots = slots;
        f.slot_of_client = slot_of_client;
        f.num_clients = world.num_clients;
        f.num_threads = num_threads;
        f.frame = frame;
        f.unreliable_per_frame = unreliable_per_frame;
        f.bytes = bytes;
        f.use_one_thread_finish = one_thread_finish;

        std::vector<std::thread> threads;
        for ( int t = 0; t < num_threads; t++ )
        {
            threads.emplace_back( wide_fill_job, &f, t );
        }
        for ( std::thread & thread : threads )
        {
            thread.join();
        }

        world.time += 1.0 / 60.0;
    }

    // drain: the last fill goes out and every reliable message arrives

    for ( int step = 0; step < 120; step++ )
    {
        rocketnet_server_send_packets( world.server );
        wide_fill_clients_step( &world );
        wide_fill_server_update( &world, three_calls, num_threads, &applied );
        while ( rocketnet_server_receive_messages( world.server ) != NULL )
        {
        }
        for ( int c = 0; c < world.num_clients; c++ )
        {
            wide_fill_client_receive( &world, c, &slots[slot_of_client[c]], slot_of_client[c] );
        }
        world.time += 1.0 / 60.0;
    }

    uint64_t sent_unreliable = 0;
    uint64_t received_unreliable = 0;
    for ( int c = 0; c < world.num_clients; c++ )
    {
        const wide_fill_slot_t * s = &slots[slot_of_client[c]];
        check( s->corrupt == 0 );
        check( s->sent[2] > 0 );
        check( s->received[2] == s->sent[2] );         // every reliable message, once, in order
        check( s->next_reliable == s->sent[2] );
        check( s->received[0] <= s->sent[0] );
        check( s->received[1] <= s->sent[1] );
        sent_unreliable += s->sent[0];
        received_unreliable += s->received[0];
    }
    check( sent_unreliable > 0 );
    check( received_unreliable * 10 >= sent_unreliable * 9 );     // a lossless hub: the budget may drop a few

    // no break of the contract happened, so nothing was refused or counted
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) == two );

    // the three calls: the game threads applied ack blocks, and no call was misuse
    if ( three_calls )
    {
        check( applied > 0 );
        check( rocketnet_api_misuse_count() == misuse );
    }

    wide_fill_world_destroy( &world );
}

static void test_wide_fill_one_thread()
{
    wide_fill_run( 1, 60, 4, 120, true );
}

static void test_wide_fill_four_threads()
{
    wide_fill_run( 4, 120, 4, 120, false );
}

static void test_wide_fill_eight_threads_large()
{
    wide_fill_run( 8, 90, 3, 1000, false );
}

// the same frame with the server updated by the three calls: receive on the
// main thread, each client's received acks applied on the game threads,
// finish on the main thread. Every reliable message still arrives once, in
// order, which needs every ack applied
static void test_wide_fill_update_in_three_calls()
{
    wide_fill_run( 4, 120, 4, 120, false, true );
    wide_fill_run( 8, 60, 3, 1000, false, true );
}

// ---------------------------------------------------------------------------
// the check, driven step by step
// ---------------------------------------------------------------------------

static void test_wide_fill_check_steps()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    rocketnet_quiet( 1 );

    uint64_t overlap = 0;
    uint64_t two = 0;
    static rocketnet_fill_phase_t phase;
    rocketnet_fill_phase_init( &phase, 4, &overlap, &two );

    // a fill phase with two threads on two clients: nothing flagged. Client
    // 0's filler is this thread, which stays alive: the check names a thread
    // by its id, and an id is reused once its thread exits, so a joined
    // filler and a new thread can be one filler to it

    g_wide_fill_asserts = 0;
    check( rocketnet_fill_phase_main_begin( &phase ) );
    rocketnet_fill_phase_main_end( &phase );
    {
        const bool a = rocketnet_fill_phase_fill_begin( &phase, 0 );
        rocketnet_fill_phase_fill_end( &phase, 0 );
        bool b = false;
        std::thread tb( [&]
                        { b = rocketnet_fill_phase_fill_begin( &phase, 1 ); rocketnet_fill_phase_fill_end( &phase, 1 ); } );
        tb.join();
        check( a && b );
    }
    check( overlap == 0 && two == 0 && g_wide_fill_asserts == 0 );

    // OneFiller: a second thread on client 0 in the same fill phase

    {
        bool second = true;
        std::thread tc( [&]
                        { second = rocketnet_fill_phase_fill_begin( &phase, 0 ); } );
        tc.join();
        check( !second );
        check( two == 1 );
        check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    }

    // another client in the same phase is still free for this thread

    check( rocketnet_fill_phase_fill_begin( &phase, 2 ) );
    rocketnet_fill_phase_fill_end( &phase, 2 );
    check( two == 1 );

    // NoFalseAlarm: after a main-thread operation, client 0 moves to another thread

    check( rocketnet_fill_phase_main_begin( &phase ) );
    rocketnet_fill_phase_main_end( &phase );
    {
        bool moved = false;
        std::thread td( [&]
                        { moved = rocketnet_fill_phase_fill_begin( &phase, 0 ); rocketnet_fill_phase_fill_end( &phase, 0 ); } );
        td.join();
        check( moved );
        check( two == 1 && overlap == 0 );
    }

    // Sound, the main side: a main-thread operation while a fill call is in progress

    g_wide_fill_asserts = 0;
    check( rocketnet_fill_phase_fill_begin( &phase, 3 ) );          // this thread is inside a fill call for client 3
    {
        bool entered = true;
        std::thread tm( [&]
                        { entered = rocketnet_fill_phase_main_begin( &phase ); } );
        tm.join();
        check( !entered );
        check( overlap == 1 );
    }

    // a main-thread operation from inside a fill call on the same thread

    check( !rocketnet_fill_phase_main_begin( &phase ) );
    check( overlap == 2 );
    rocketnet_fill_phase_fill_end( &phase, 3 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );

    // Sound, the fill side: a fill call while a main-thread operation runs

    g_wide_fill_asserts = 0;
    check( rocketnet_fill_phase_main_begin( &phase ) );
    {
        bool entered = true;
        std::thread tf( [&]
                        { entered = rocketnet_fill_phase_fill_begin( &phase, 1 ); } );
        tf.join();
        check( !entered );
        check( overlap == 3 );
    }

    // two main-thread operations at once

    {
        bool entered = true;
        std::thread tn( [&]
                        { entered = rocketnet_fill_phase_main_begin( &phase ); } );
        tn.join();
        check( !entered );
        check( overlap == 4 );
    }
    rocketnet_fill_phase_main_end( &phase );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );

    // nesting: an inner call of either kind is the outer call's own work

    check( rocketnet_fill_phase_fill_begin( &phase, 2 ) );
    check( rocketnet_fill_phase_fill_begin( &phase, 2 ) );
    rocketnet_fill_phase_fill_end( &phase, 2 );
    rocketnet_fill_phase_fill_end( &phase, 2 );
    check( rocketnet_fill_phase_main_begin( &phase ) );
    check( rocketnet_fill_phase_main_begin( &phase ) );
    check( rocketnet_fill_phase_fill_begin( &phase, 1 ) );           // a send a main-thread operation makes
    rocketnet_fill_phase_fill_end( &phase, 1 );
    rocketnet_fill_phase_main_end( &phase );
    rocketnet_fill_phase_main_end( &phase );
    check( overlap == 4 && two == 1 );

    rocketnet_assert_function( NULL );
}

// the nesting depth has 7 bits: a fill call nested
// ROCKETNET_FILL_PHASE_DEPTH_MAX deep on one client is refused, counted as
// API misuse, and leaves the word as it was; one more would have carried
// into the epoch. The calls below it end as nested calls do, and the check
// is whole after them

static void test_wide_fill_check_depth_bound()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    rocketnet_quiet( 1 );
    g_wide_fill_asserts = 0;

    uint64_t overlap = 0;
    uint64_t two = 0;
    static rocketnet_fill_phase_t phase;
    rocketnet_fill_phase_init( &phase, 1, &overlap, &two );
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t epoch = phase.epoch.load();

    int entered = 0;
    for ( int i = 0; i < ROCKETNET_FILL_PHASE_DEPTH_MAX; i++ )
    {
        entered += rocketnet_fill_phase_fill_begin( &phase, 0 ) ? 1 : 0;
    }
    check( entered == ROCKETNET_FILL_PHASE_DEPTH_MAX );
    const uint64_t held = phase.clients[0].claim.load();
    check( ( ( held & ROCKETNET_FILL_PHASE_DEPTH_MASK ) >> ROCKETNET_FILL_PHASE_DEPTH_SHIFT ) == ROCKETNET_FILL_PHASE_DEPTH_MAX );
    check( g_wide_fill_asserts == 0 );

    const bool deeper = rocketnet_fill_phase_fill_begin( &phase, 0 );
    check( !deeper );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( phase.clients[0].claim.load() == held );
    check( ( held >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT ) == epoch );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    check( overlap == 0 && two == 0 );
    if ( deeper )
    {
        rocketnet_fill_phase_fill_end( &phase, 0 );
    }

    for ( int i = 0; i < entered; i++ )
    {
        rocketnet_fill_phase_fill_end( &phase, 0 );
    }
    const uint64_t left = phase.clients[0].claim.load();
    check( ( left & ROCKETNET_FILL_PHASE_IN ) == 0 );
    check( ( left >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT ) == epoch );
    uint64_t lines = 0;
    for ( int l = 0; l < ROCKETNET_FILL_PHASE_FILL_LINES; l++ )
    {
        lines += phase.lines[l].fills_in.load();
    }
    check( lines == 0 );
    check( rocketnet_fill_phase_main_begin( &phase ) );
    rocketnet_fill_phase_main_end( &phase );
    check( overlap == 0 );

    rocketnet_assert_function( NULL );
}

// the check runs from the server's first call that opts in (rocketnet.h,
// rocketnet_server_t): a stop and a start make a new transport, whose check
// the start arms again, so a breach after the restart is refused and counted
// as one before it is

static void test_wide_fill_check_armed_across_restart()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 0 );

    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( phase != NULL );
    check( !rocketnet_fill_phase_armed( phase ) );

    // a run that never opted in leaves the next one not armed

    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( phase != NULL );
    check( !rocketnet_fill_phase_armed( phase ) );

    rocketnet_server_update_receive( world.server, world.time );
    rocketnet_server_update_finish( world.server );
    check( rocketnet_fill_phase_armed( phase ) );

    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );

    phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( phase != NULL );
    check( rocketnet_fill_phase_armed( phase ) );

    // a main operation's scope as the call sites take it (held by hand) and
    // a fill call from another thread inside it: refused, counted on the
    // new transport

    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    {
        rocketnet_fill_phase_main_scope_t held( phase );
        check( held.fill_phase == phase && held.entered );
        bool entered = true;
        std::thread tf( [&]
                        {
            entered = rocketnet_fill_phase_fill_begin( phase, 0 );
            if ( entered )
            {
                rocketnet_fill_phase_fill_end( phase, 0 );
            } } );
        tf.join();
        check( !entered );
    }
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 1 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

#if ROCKETNET_DEV

// Two rule breaches at once must leave the check whole (the header's
// promise: a refused call does nothing). The interleaving, driven by the
// check's own trace hook: thread B fills the client in one phase and stays
// alive (the owner it left is B); after a main operation thread A claims the
// client in the next and is held after its count; the window opens, so A's
// look will refuse; B's next call finds the client in and is held after its
// first load; A's look refuses; B goes on. Had A stored the owner before its
// look and put B back at its refusal, B would read the word A's claim left
// and the owner A put back, take itself as nested in a call it never made,
// count nothing, and its end would take one off a fills_in line it never
// counted on: every main operation refused from then on. Taken back to that
// order (with the FillPeek hook between its two loads) this test fails at
// !b_second; as the code is, A holds the client's word in its lock form
// until its look, so B is refused at its lock and never parks at FillPeek.

struct wide_fill_park_t
{
    std::mutex mutex;
    std::condition_variable changed;
    const char * park_at[2];                // the step each role parks at once, NULL: none
    bool parked[2];
    bool release[2];
    bool go[2];
    bool waiting[2];
    bool done[2];
};

static thread_local int g_wide_fill_role = -1;

static void wide_fill_park_hook( void * context, const char * action, int, uint64_t, bool )
{
    wide_fill_park_t * park = (wide_fill_park_t *) context;
    const int role = g_wide_fill_role;
    if ( role < 0 )
    {
        return;
    }
    std::unique_lock<std::mutex> lock( park->mutex );
    if ( !park->park_at[role] || strcmp( action, park->park_at[role] ) != 0 )
    {
        return;
    }
    park->park_at[role] = NULL;
    park->parked[role] = true;
    park->changed.notify_all();
    park->changed.wait( lock, [&]
                        { return park->release[role]; } );
}

// role waits for the test's go
static void wide_fill_park_gate( wide_fill_park_t * park, int role )
{
    std::unique_lock<std::mutex> lock( park->mutex );
    park->waiting[role] = true;
    park->changed.notify_all();
    park->changed.wait( lock, [&]
                        { return park->go[role]; } );
}

static void wide_fill_park_set( wide_fill_park_t * park, bool * flag )
{
    std::unique_lock<std::mutex> lock( park->mutex );
    *flag = true;
    park->changed.notify_all();
}

static void wide_fill_park_wait( wide_fill_park_t * park, const bool * a, const bool * b )
{
    std::unique_lock<std::mutex> lock( park->mutex );
    park->changed.wait( lock, [&]
                        { return *a || *b; } );
}

static void test_wide_fill_check_nested_never_torn()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    rocketnet_quiet( 1 );
    g_wide_fill_asserts = 0;

    uint64_t overlap = 0;
    uint64_t two = 0;
    static rocketnet_fill_phase_t phase;
    rocketnet_fill_phase_init( &phase, 1, &overlap, &two );
    const uint64_t misuse = rocketnet_api_misuse_count();

    static wide_fill_park_t park;
    park.park_at[0] = park.park_at[1] = NULL;
    for ( int r = 0; r < 2; r++ )
    {
        park.parked[r] = park.release[r] = park.go[r] = park.waiting[r] = park.done[r] = false;
    }
    rocketnet_fill_phase_set_trace( wide_fill_park_hook, &park );

    // B: one whole call, then the held one

    bool b_first = false;
    bool b_second = false;
    std::thread tb( [&]
                    {
        g_wide_fill_role = 1;
        b_first = rocketnet_fill_phase_fill_begin( &phase, 0 );
        if ( b_first )
        {
            rocketnet_fill_phase_fill_end( &phase, 0 );
        }
        wide_fill_park_gate( &park, 1 );
        b_second = rocketnet_fill_phase_fill_begin( &phase, 0 );
        if ( b_second )
        {
            rocketnet_fill_phase_fill_end( &phase, 0 );
        }
        wide_fill_park_set( &park, &park.done[1] ); } );
    wide_fill_park_wait( &park, &park.waiting[1], &park.done[1] );
    check( b_first );

    // a main operation: the next phase

    check( rocketnet_fill_phase_main_begin( &phase ) );
    rocketnet_fill_phase_main_end( &phase );

    // A: claims the client, held after its count

    {
        std::unique_lock<std::mutex> lock( park.mutex );
        park.park_at[0] = "FillCount";
    }
    bool a_entered = true;
    std::thread ta( [&]
                    {
        g_wide_fill_role = 0;
        a_entered = rocketnet_fill_phase_fill_begin( &phase, 0 );
        if ( a_entered )
        {
            rocketnet_fill_phase_fill_end( &phase, 0 );
        }
        wide_fill_park_set( &park, &park.done[0] ); } );
    wide_fill_park_wait( &park, &park.parked[0], &park.done[0] );
    check( park.parked[0] );

    // the window opens (an update_receive's work): A's look will refuse

    rocketnet_fill_phase_window( &phase, true );

    // B's second call, held after it loads the word, if it gets that far

    {
        std::unique_lock<std::mutex> lock( park.mutex );
        park.park_at[1] = "FillPeek";
        park.go[1] = true;
        park.changed.notify_all();
    }
    wide_fill_park_wait( &park, &park.parked[1], &park.done[1] );

    // A's look refuses; then B goes on

    wide_fill_park_set( &park, &park.release[0] );
    wide_fill_park_wait( &park, &park.done[0], &park.done[0] );
    wide_fill_park_set( &park, &park.release[1] );
    wide_fill_park_wait( &park, &park.done[1], &park.done[1] );
    ta.join();
    tb.join();
    rocketnet_fill_phase_set_trace( NULL, NULL );
    rocketnet_fill_phase_window( &phase, false );

    // both refused, each counted once, and the check whole

    check( !a_entered );
    check( !b_second );
    check( overlap == 1 );
    check( two == 1 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );
    check( rocketnet_api_misuse_count() == misuse );
    uint64_t lines = 0;
    for ( int l = 0; l < ROCKETNET_FILL_PHASE_FILL_LINES; l++ )
    {
        lines += phase.lines[l].fills_in.load();
    }
    check( lines == 0 );
    check( ( phase.clients[0].claim.load() & ROCKETNET_FILL_PHASE_IN ) == 0 );
    check( phase.main_calls.load() == 0 );
    check( rocketnet_fill_phase_main_begin( &phase ) );
    rocketnet_fill_phase_main_end( &phase );
    check( rocketnet_fill_phase_fill_begin( &phase, 0 ) );
    rocketnet_fill_phase_fill_end( &phase, 0 );

    rocketnet_assert_function( NULL );
}

// A call a game thread may make reads the running server once, before its
// check, and keeps that copy: a stop that runs whole between that read and
// the rest of the call (here between start_message's read and its fill
// check's first step, held by the trace hook) leaves the copy alive,
// retiring, and the call ends as it would after the stop: its check finds
// the stop's main operation ran since it began, refuses it and counts it
// once. Read twice, the second read found the server stopped and the call
// counted API misuse for a NULL it never passed.

static void test_wide_fill_check_running_read_once()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 1 );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );
    rocketnet_server_send_packets( world.server );      // a main thread call: the opt-in's claim ends
    const int slot = rocketnet_client_index( world.clients[0] );
    check( slot >= 0 );

    static wide_fill_park_t park;
    park.park_at[0] = park.park_at[1] = NULL;
    for ( int r = 0; r < 2; r++ )
    {
        park.parked[r] = park.release[r] = park.go[r] = park.waiting[r] = park.done[r] = false;
    }
    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( phase != NULL );
    rocketnet_fill_phase_set_trace( wide_fill_park_hook, &park );

    // A, a game thread, starts a message on the client and is held at its
    // fill check's first step, after its one read of the running server

    {
        std::unique_lock<std::mutex> lock( park.mutex );
        park.park_at[0] = "FillBegin";
    }
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    const int asserts = g_wide_fill_asserts;
    uint8_t * started = (uint8_t *) 1;
    std::thread ta( [&]
                    {
        g_wide_fill_role = 0;
        started = rocketnet_server_start_message( world.server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        wide_fill_park_set( &park, &park.done[0] ); } );
    wide_fill_park_wait( &park, &park.parked[0], &park.done[0] );
    check( park.parked[0] );

    // the stop runs whole on this thread, then A goes on

    rocketnet_server_stop( world.server );
    check( !rocketnet_server_running( world.server ) );
    wide_fill_park_set( &park, &park.release[0] );
    ta.join();
    rocketnet_fill_phase_set_trace( NULL, NULL );

    // the call ran to its end on the copy, as it would after the stop: its
    // check refused it (one overlap, its one debug assert), it opened
    // nothing, and no API misuse was counted

    check( started == NULL );
    check( rocketnet_api_misuse_count() == misuse );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 1 );
    check( g_wide_fill_asserts == asserts + wide_fill_expected_asserts( 1 ) );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// A checked main-thread call reads the running server once, then enters its
// check: a stop that runs whole between that read and the call's work inside
// the check leaves the call nothing to do. Here the outer stop is held at its
// check's entry by the trace hook, which makes a whole stop (nested, so its
// own work) before the outer one goes on. The outer stop then finds the run
// it read already retired and returns: the retiring run is kept until the
// destroy. Before, the outer stop read the server again inside the check,
// found it stopped and stored that NULL over the retiring run, so the server
// reported stopped at once and the run's transport was never polled or
// destroyed.

struct wide_fill_stop_inside_t
{
    rocketnet_server_t * server;
    bool armed;
    int stops;
};

static void wide_fill_stop_inside_hook( void * context, const char * action, int, uint64_t, bool refused )
{
    wide_fill_stop_inside_t * inside = (wide_fill_stop_inside_t *) context;
    if ( !inside->armed || refused || strcmp( action, "MainIn" ) != 0 )
    {
        return;
    }
    inside->armed = false;
    inside->stops++;
    rocketnet_server_stop( inside->server );
}

static void test_wide_fill_check_stop_read_once()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 1 );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );
    rocketnet_server_send_packets( world.server );      // a main thread call: the opt-in's claim ends

    struct rocketnet_message_server_t * running = rocketnet_server_message_server( world.server );
    check( running != NULL );
    const uint64_t misuse = rocketnet_api_misuse_count();

    wide_fill_stop_inside_t inside;
    inside.server = world.server;
    inside.armed = true;
    inside.stops = 0;
    rocketnet_fill_phase_set_trace( wide_fill_stop_inside_hook, &inside );
    rocketnet_server_stop( world.server );
    rocketnet_fill_phase_set_trace( NULL, NULL );
    check( inside.stops == 1 );

    // stopped once: the run that stop retired is still the retiring one,
    // nothing refused and no misuse counted

    check( !rocketnet_server_running( world.server ) );
    check( rocketnet_server_message_server( world.server ) == running );
    check( rocketnet_api_misuse_count() == misuse );
    check( g_wide_fill_asserts == 0 );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// The same for the checked calls that are not made every frame: a
// disconnect of one client, a disconnect of all and a client's send rate.
// Each is held at its check's entry while a whole stop runs (nested, as
// above), then finds the server stopped and does nothing: no API misuse, no
// assert, the retiring run kept. Before, each read the running server again
// inside its check and found NULL. Each disconnect took the address of a
// member through it for its kick, which is undefined behaviour that only a
// build with -fsanitize=null reports (the kick then returned on NULL); the
// disconnect of one client and the send rate also passed that NULL on and
// counted API misuse for a NULL the game never passed (asserting in a debug
// build), so the disconnect of all was red only under the sanitizer.

enum wide_fill_stopped_call_t
{
    WIDE_FILL_STOPPED_DISCONNECT_CLIENT,
    WIDE_FILL_STOPPED_DISCONNECT_ALL,
    WIDE_FILL_STOPPED_SET_SEND_KBPS,
};

static void wide_fill_check_call_after_inside_stop( wide_fill_stopped_call_t call )
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 1 );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );
    rocketnet_server_send_packets( world.server );      // a main thread call: the opt-in's claim ends
    const int slot = rocketnet_client_index( world.clients[0] );
    check( slot >= 0 );

    struct rocketnet_message_server_t * running = rocketnet_server_message_server( world.server );
    check( running != NULL );
    const uint64_t misuse = rocketnet_api_misuse_count();

    wide_fill_stop_inside_t inside;
    inside.server = world.server;
    inside.armed = true;
    inside.stops = 0;
    rocketnet_fill_phase_set_trace( wide_fill_stop_inside_hook, &inside );
    if ( call == WIDE_FILL_STOPPED_DISCONNECT_CLIENT )
    {
        rocketnet_server_disconnect_client( world.server, slot );
    }
    else if ( call == WIDE_FILL_STOPPED_DISCONNECT_ALL )
    {
        rocketnet_server_disconnect_all_clients( world.server );
    }
    else
    {
        check( rocketnet_server_set_send_kbps( world.server, slot, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS ) == ROCKETNET_STATUS_OK );
    }
    rocketnet_fill_phase_set_trace( NULL, NULL );
    check( inside.stops == 1 );

    check( !rocketnet_server_running( world.server ) );
    check( rocketnet_server_message_server( world.server ) == running );
    check( rocketnet_api_misuse_count() == misuse );
    check( g_wide_fill_asserts == 0 );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

static void test_wide_fill_check_disconnect_client_read_once()
{
    wide_fill_check_call_after_inside_stop( WIDE_FILL_STOPPED_DISCONNECT_CLIENT );
}

static void test_wide_fill_check_disconnect_all_read_once()
{
    wide_fill_check_call_after_inside_stop( WIDE_FILL_STOPPED_DISCONNECT_ALL );
}

static void test_wide_fill_check_set_send_kbps_read_once()
{
    wide_fill_check_call_after_inside_stop( WIDE_FILL_STOPPED_SET_SEND_KBPS );
}

#endif // #if ROCKETNET_DEV

// the public server: refusals reach the server's counters, and a refused
// fill call does nothing

static void test_wide_fill_check_public()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );
    const int slot0 = rocketnet_client_index( world.clients[0] );
    const int slot1 = rocketnet_client_index( world.clients[1] );

    const uint64_t two = wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS );

    // this thread (A, alive throughout: an exited thread's id may be reused
    // by B, and the check names a thread by its id) opens and finishes a
    // message on slot0; thread B, in the same fill phase, is refused on slot0
    // and accepted on slot1

    {
        uint32_t serial = 0;
        uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x41, 8 );
            serial = rocketnet_server_finish_client_message( world.server, slot0, 8 );
        }
        check( serial != 0 );

        uint8_t * refused = (uint8_t *) 1;
        uint8_t * accepted = NULL;
        std::thread tb( [&]
                        {
            refused = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
            accepted = rocketnet_server_start_message( world.server, (uint64_t) slot1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
            if ( accepted )
            {
                memset( accepted, 0x42, 8 );
                check( rocketnet_server_finish_client_message( world.server, slot1, 8 ) != 0 );
            } } );
        tb.join();
        check( refused == NULL );
        check( accepted != NULL );
        check( wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) == two + 1 );
        check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    }

    // after the next main-thread operation thread B may take slot0

    wide_fill_pump( &world );
    {
        uint8_t * p = NULL;
        std::thread tb( [&]
                        {
            p = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
            if ( p )
            {
                memset( p, 0x43, 8 );
                check( rocketnet_server_finish_client_message( world.server, slot0, 8 ) != 0 );
            } } );
        tb.join();
        check( p != NULL );
        check( wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) == two + 1 );
    }

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// ---------------------------------------------------------------------------
// one open message per client (RNMessageOpen)
// ---------------------------------------------------------------------------

static void test_wide_fill_open_per_client()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    const int slot0 = rocketnet_client_index( world.clients[0] );
    const int slot1 = rocketnet_client_index( world.clients[1] );
    rn_message_object_t * object = &rocketnet_server_message_server( world.server )->object;

    // OpenTwice (reversed witness): a second open on one client is refused,
    // counted, and leaves the first open

    uint8_t * a = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( a != NULL );
    check( rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 ) == NULL );
    check( rn_message_open_count( object ) == 1 );
    check( object->connections[slot0].open.pending == 1 );

    // an open on another client is independent

    uint8_t * b = rocketnet_server_start_message( world.server, (uint64_t) slot1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( b != NULL );
    check( rn_message_open_count( object ) == 2 );

    // with two open the finish that names no client cannot tell which is meant

    check( g_wide_fill_asserts == 0 );
    check( rocketnet_server_finish_message( world.server, 8 ) == 0 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    check( rn_message_open_count( object ) == 2 );
    rocketnet_server_abort_message( world.server );                  // does nothing with two open
    check( rn_message_open_count( object ) == 2 );

    // each closes by its own client

    if ( a && b )
    {
        memset( a, 0x51, 8 );
        memset( b, 0x52, 8 );
        check( rocketnet_server_finish_client_message( world.server, slot0, 8 ) != 0 );
        check( rn_message_open_count( object ) == 1 );
        rocketnet_server_abort_client_message( world.server, slot1 );
        check( rn_message_open_count( object ) == 0 );
    }

    // with one open the finish that names no client finishes it

    a = rocketnet_server_start_message( world.server, (uint64_t) slot1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( a != NULL );
    if ( a )
    {
        memset( a, 0x53, 8 );
        check( rocketnet_server_finish_message( world.server, 8 ) != 0 );
    }
    check( rn_message_open_count( object ) == 0 );

    // FlushWhileOpen (reversed witness): a send_packets while an in-place
    // message is open closes it, counted; its finish returns 0

    g_wide_fill_asserts = 0;
    wide_fill_pump( &world );
    a = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 );
    b = rocketnet_server_start_message( world.server, (uint64_t) slot1, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 );
    check( a != NULL && b != NULL );
    check( object->connections[slot0].open.in_place > 0 && object->connections[slot1].open.in_place > 0 );
    rocketnet_server_send_packets( world.server );
    check( rn_message_open_count( object ) == 0 );
    check( object->connections[slot0].open.in_place == -1 && object->connections[slot1].open.in_place == -1 );
    check( rocketnet_server_finish_client_message( world.server, slot0, 16 ) == 0 );
    check( object->connections[slot0].open.in_place == 0 );
    check( object->connections[slot1].open.in_place == -1 );
    rocketnet_server_abort_client_message( world.server, slot1 );
    check( object->connections[slot1].open.in_place == 0 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );

    // out of range is misuse

    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_finish_client_message( world.server, WIDE_FILL_MAX_CLIENTS, 8 ) == 0 );
    rocketnet_server_abort_client_message( world.server, -1 );
    check( rocketnet_api_misuse_count() == misuse + 2 );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// a main-thread operation made while a fill is inside the check is refused
// whole: stop, disconnect_all_clients and disconnect_client do nothing and
// the server keeps running with every client connected. The fill is held
// open by hand on this thread (the check's own entry), so the case needs no
// second thread.

static void test_wide_fill_main_refused_during_fill()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );
    const int slot0 = rocketnet_client_index( world.clients[0] );
    const int slot1 = rocketnet_client_index( world.clients[1] );

    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) );
    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( transport );
    check( phase != NULL );
    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );

    check( rocketnet_fill_phase_fill_begin( phase, slot0 ) );

    rocketnet_server_stop( world.server );
    check( rocketnet_server_running( world.server ) );
    rocketnet_server_disconnect_all_clients( world.server );
    rocketnet_server_disconnect_client( world.server, slot1 );
    check( rocketnet_server_client_connected( world.server, slot0 ) );
    check( rocketnet_server_client_connected( world.server, slot1 ) );

    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 3 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 3 ) );

    rocketnet_fill_phase_fill_end( phase, slot0 );

    // with the fill ended the same operation runs

    rocketnet_server_disconnect_client( world.server, slot1 );
    check( !rocketnet_server_client_connected( world.server, slot1 ) );
    check( rocketnet_server_client_connected( world.server, slot0 ) );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 3 );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// receive_messages made while another main-thread operation runs (held open
// by hand on this thread; the receive is a real second thread) is refused,
// counted MAIN_THREAD_CALL_OVERLAP, and does nothing: the batch the game
// holds keeps its messages and the queue keeps the rest. Before, the call
// cleared the batch's count ahead of its check, so the refused call emptied
// the batch the game was reading, and made at the same moment as an accepted
// receive it raced that receive's fill of the batch.

static void test_wide_fill_refused_receive_keeps_batch()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 1 );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );
    const int slot = rocketnet_client_index( world.clients[0] );

    for ( int m = 0; m < 2; m++ )
    {
        uint8_t * p = rocketnet_client_start_message( world.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x81 + m, 8 );
            check( rocketnet_client_finish_message( world.clients[0], 8 ) != 0 );
        }
    }
    rocketnet_server_send_packets( world.server );
    wide_fill_clients_step( &world );
    rocketnet_server_update( world.server, world.time );

    const struct rocketnet_message_batch_t * batch = rocketnet_server_receive_messages( world.server );
    check( batch != NULL );
    if ( !batch )
    {
        wide_fill_world_destroy( &world );
        rocketnet_assert_function( NULL );
        return;
    }
    const int held = batch->num_messages;
    check( held == 2 );
    const uint8_t * first = batch->data[0];

    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( phase != NULL );
    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    const uint64_t misuse = rocketnet_api_misuse_count();

    check( rocketnet_fill_phase_main_begin( phase ) );
    const struct rocketnet_message_batch_t * second = batch;
    std::thread tr( [&]
                    { second = rocketnet_server_receive_messages( world.server ); } );
    tr.join();
    rocketnet_fill_phase_main_end( phase );

    // refused and counted; the held batch as it was

    check( second == NULL );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 1 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    check( rocketnet_api_misuse_count() == misuse );
    check( batch->num_messages == held );
    check( batch->data[0] == first );
    check( batch->client_index[0] == slot && batch->client_index[1] == slot );
    check( batch->data[0][0] == 0x81 && batch->data[1][0] == 0x82 );

    // the next accepted receive releases the batch and finds nothing more:
    // the refused call took nothing from the queue

    check( rocketnet_server_receive_messages( world.server ) == NULL );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// the update in three calls. update_receive arms the check, as the first
// call that names a client does. From update_receive's return to
// update_finish only update_client (from any thread) and update_finish are
// allowed: a send from any thread and every other checked main-thread call
// is refused, counted MAIN_THREAD_CALL_OVERLAP, and does nothing. And
// update_finish is a main-thread call: made while a fill call is in progress
// it is refused and counted, not run over the fill. One thread holds the
// fill by hand (the check's own entry); the game thread's send is a real
// second thread.

static void test_wide_fill_split_update_window()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    const int slot0 = rocketnet_client_index( world.clients[0] );
    const int slot1 = rocketnet_client_index( world.clients[1] );

    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) );
    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( transport );
    check( phase != NULL );
    const uint64_t misuse = rocketnet_api_misuse_count();

    // the split update arms the check

    check( !rocketnet_fill_phase_armed( phase ) );
    rocketnet_server_send_packets( world.server );
    wide_fill_clients_step( &world );
    rocketnet_server_update_receive( world.server, world.time );
    check( rocketnet_fill_phase_armed( phase ) );

    // inside the window: refused, counted, nothing done

    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    check( rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 ) == NULL );
    uint8_t * game = (uint8_t *) 1;
    std::thread tg( [&]
                    { game = rocketnet_server_start_message( world.server, (uint64_t) slot1, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8 ); } );
    tg.join();
    check( game == NULL );
    rocketnet_server_send_packets( world.server );
    check( rocketnet_server_receive_messages( world.server ) == NULL );
    struct rocketnet_receipt_t receipts[4];
    check( rocketnet_server_receipts( world.server, slot0, receipts, 4 ) == 0 );
    check( rocketnet_server_set_send_kbps( world.server, slot0, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_server_disconnect_client( world.server, slot1 );
    rocketnet_server_disconnect_all_clients( world.server );
    rocketnet_server_stop( world.server );
    check( rocketnet_server_running( world.server ) );
    check( rocketnet_server_client_connected( world.server, slot0 ) );
    check( rocketnet_server_client_connected( world.server, slot1 ) );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 9 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 9 ) );

    // update_client from this thread and from a game thread is the window's
    // own work: nothing refused; then update_finish closes the window

    (void) rocketnet_server_update_client( world.server, slot0 );
    std::thread tu( [&]
                    { (void) rocketnet_server_update_client( world.server, slot1 ); } );
    tu.join();
    rocketnet_server_update_finish( world.server );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 9 );

    uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    if ( p )
    {
        memset( p, 0x61, 8 );
        check( rocketnet_server_finish_client_message( world.server, slot0, 8 ) != 0 );
    }
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 9 );

    // update_finish while a fill call is in progress: refused and counted,
    // not run as an end with no begin (which would be misuse)

    g_wide_fill_asserts = 0;
    check( rocketnet_fill_phase_fill_begin( phase, slot1 ) );
    rocketnet_server_update_finish( world.server );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 10 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    rocketnet_fill_phase_fill_end( phase, slot1 );
    check( rocketnet_api_misuse_count() == misuse );

    // update_finish while a game thread is inside update_client (a take held
    // by hand): refused and counted, the update stays open, and a send is
    // still refused in it; with the take ended the finish closes it

    g_wide_fill_asserts = 0;
    rocketnet_server_send_packets( world.server );
    wide_fill_clients_step( &world );
    rocketnet_server_update_receive( world.server, world.time );
    check( rocketnet_fill_phase_take_begin( phase ) );
    rocketnet_server_update_finish( world.server );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 11 );
    rocketnet_fill_phase_take_end( phase );
    check( rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 ) == NULL );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 12 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );
    rocketnet_server_update_finish( world.server );
    p = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    if ( p )
    {
        memset( p, 0x62, 8 );
        check( rocketnet_server_finish_client_message( world.server, slot0, 8 ) != 0 );
    }

    // an update_client made inside a main-thread operation: refused at its
    // look and counted (the operation is held open by hand)

    rocketnet_server_update_receive( world.server, world.time );
    check( rocketnet_fill_phase_main_begin( phase, true ) );
    int taken = -1;
    std::thread tt( [&]
                    { taken = rocketnet_server_update_client( world.server, slot0 ); } );
    tt.join();
    check( taken == 0 );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 13 );
    rocketnet_fill_phase_main_end( phase );
    rocketnet_server_update_finish( world.server );
    check( rocketnet_api_misuse_count() == misuse );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// the client sends one reliable message, the server's update_receive takes
// it into the open update, and the update is left open (update_finish not
// called, or refused)
static void wide_fill_open_update_with_delivery( wide_fill_world_t * world, uint8_t fill )
{
    uint8_t * m = rocketnet_client_start_message( world->clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( m != NULL );
    if ( m )
    {
        memset( m, fill, 8 );
        check( rocketnet_client_finish_message( world->clients[0], 8 ) != 0 );
    }
    rocketnet_server_send_packets( world->server );
    wide_fill_clients_step( world );
    rocketnet_server_update_receive( world->server, world->time );
    world->time += 1.0 / 60.0;
}

// the message the open update took is handed to the game: one message, the
// client's bytes
static void wide_fill_check_delivered( wide_fill_world_t * world, int slot, uint8_t fill )
{
    int found = 0;
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            check( batch->client_index[i] == slot );
            check( batch->bytes[i] == 8 );
            check( batch->data[i][0] == fill && batch->data[i][7] == fill );
            found++;
        }
    }
    check( found == 1 );
}

// An update call made while the update in three calls is open (update or
// update_receive with no update_finish, or after a refused one) is API
// misuse: it finishes the open update and begins none. The deliveries that
// finish hands over wait for receive_messages, as after any finish: nobody
// is disconnected for messages the game never had the chance to receive,
// and the misuse is counted once
static void test_wide_fill_split_update_open_update_call()
{
    rocketnet_assert_function( wide_fill_assert_hook );

    wide_fill_world_t world;
    wide_fill_world_create( &world, 1 );
    wide_fill_connect_all( &world );
    const int slot = rocketnet_client_index( world.clients[0] );
    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );

    // update_receive again: the open update finished, none begun, so
    // update_client finds nothing (not misuse) and update_finish only returns

    wide_fill_open_update_with_delivery( &world, 0x71 );
    g_wide_fill_asserts = 0;
    uint64_t misuse = rocketnet_api_misuse_count();
    rocketnet_server_update_receive( world.server, world.time );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    check( rocketnet_server_update_client( world.server, slot ) == 0 );
    rocketnet_server_update_finish( world.server );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_server_client_connected( world.server, slot ) );
    check( rocketnet_server_disconnect_reason( world.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    wide_fill_check_delivered( &world, slot, 0x71 );

    // the one-call update inside the open update: the same

    wide_fill_open_update_with_delivery( &world, 0x72 );
    g_wide_fill_asserts = 0;
    misuse = rocketnet_api_misuse_count();
    rocketnet_server_update( world.server, world.time );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );
    check( rocketnet_server_client_connected( world.server, slot ) );
    wide_fill_check_delivered( &world, slot, 0x72 );

    // a late join: update_finish refused while a game thread is still in
    // update_client (a take held by hand), so the next frame's update_receive
    // finds the update open. One refused finish disconnects nobody

    wide_fill_open_update_with_delivery( &world, 0x73 );
    g_wide_fill_asserts = 0;
    check( rocketnet_fill_phase_take_begin( phase ) );
    rocketnet_server_update_finish( world.server );
    rocketnet_fill_phase_take_end( phase );
    misuse = rocketnet_api_misuse_count();
    rocketnet_server_update_receive( world.server, world.time );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );       // the refused finish, the misuse
    rocketnet_server_update_finish( world.server );
    check( rocketnet_server_client_connected( world.server, slot ) );
    check( rocketnet_server_disconnect_reason( world.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    wide_fill_check_delivered( &world, slot, 0x73 );

    // and the next frame is an ordinary one: the client still sends and is
    // received

    wide_fill_open_update_with_delivery( &world, 0x74 );
    misuse = rocketnet_api_misuse_count();
    rocketnet_server_update_finish( world.server );
    check( rocketnet_api_misuse_count() == misuse );
    wide_fill_check_delivered( &world, slot, 0x74 );
    check( rocketnet_server_client_connected( world.server, slot ) );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// finish_message and abort_message, the forms of a server used one thread at
// a time, are main-thread calls once the check has begun, wherever the
// message was started: checked before they look for the open message, so in
// the window, or while a fill call runs, they are refused and counted
// MAIN_THREAD_CALL_OVERLAP and change nothing. Before, they scanned the
// connections first and checked only when another thread held the client:
// with nothing open in the window, or their own message open beside
// another thread's fill call, they ran unrefused and uncounted
static void test_wide_fill_plain_finish_checked()
{
    rocketnet_assert_function( wide_fill_assert_hook );

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    const int slot0 = rocketnet_client_index( world.clients[0] );
    const int slot1 = rocketnet_client_index( world.clients[1] );
    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    const uint64_t misuse = rocketnet_api_misuse_count();

    // in the window with nothing open: refused and counted, both forms

    rocketnet_server_send_packets( world.server );
    wide_fill_clients_step( &world );
    rocketnet_server_update_receive( world.server, world.time );
    g_wide_fill_asserts = 0;
    uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    check( rocketnet_server_finish_message( world.server, 8 ) == 0 );
    rocketnet_server_abort_message( world.server );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 2 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );
    rocketnet_server_update_finish( world.server );

    // this thread's own message open, and a fill call on another client in
    // progress (held by hand): the finish is refused and counted, and the
    // message stays open; with the fill call ended it finishes

    uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    if ( p )
    {
        memset( p, 0x75, 8 );
    }
    g_wide_fill_asserts = 0;
    overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    check( rocketnet_fill_phase_fill_begin( phase, slot1 ) );
    check( rocketnet_server_finish_message( world.server, 8 ) == 0 );
    rocketnet_server_abort_message( world.server );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 2 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );
    rocketnet_fill_phase_fill_end( phase, slot1 );
    check( rocketnet_server_finish_message( world.server, 8 ) != 0 );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 2 );
    check( rocketnet_api_misuse_count() == misuse );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// The window follows the update's stage: an update_receive that returns
// before the transport opens its update (the first update's time not
// finite, so the clock does not start; or the transport not running) still
// leaves the update open until update_finish, and every call the window
// refuses is refused and counted in it. Before, the window was stored only
// by the transport's receive, so on those paths it stayed shut
static void test_wide_fill_split_update_window_on_early_return()
{
    rocketnet_assert_function( wide_fill_assert_hook );

    wide_fill_world_t world;
    wide_fill_world_create( &world, 0 );
    rocketnet_fill_phase_t * phase = rocketnet_server_transport_fill_phase( rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) ) );
    check( phase != NULL );

    // the server's first update, with a time that is not finite: clamped,
    // the clock not started, nothing received; the update is open

    rocketnet_server_update_receive( world.server, (double) NAN );
    check( rocketnet_fill_phase_armed( phase ) );
    g_wide_fill_asserts = 0;
    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    rocketnet_server_send_packets( world.server );
    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 2 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 2 ) );

    // update_finish closes it: the same calls run

    check( rocketnet_server_update_client( world.server, 0 ) == 0 );
    rocketnet_server_update_finish( world.server );
    rocketnet_server_send_packets( world.server );
    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 2 );

    // and a whole update leaves it shut

    rocketnet_server_update( world.server, world.time );
    rocketnet_server_send_packets( world.server );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap + 2 );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// a finish the check refuses leaves its start open; the next main-thread
// operation closes it, counted MESSAGE_SEND_REFUSED; and a refused transport
// start names no client, so its finish and abort claim none

static void test_wide_fill_refused_finish_and_start()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    wide_fill_opt_in( &world );
    const int slot0 = rocketnet_client_index( world.clients[0] );
    rn_message_object_t * object = &rocketnet_server_message_server( world.server )->object;
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) );

    const uint64_t two = wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS );
    const uint64_t refused = object->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED];

    // this thread (alive throughout) opens on slot0; thread B's finish of it
    // is refused, and B's transport start on slot0 is refused with a context
    // that names no client

    uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    if ( p )
    {
        memset( p, 0x61, 8 );
    }

    uint32_t finished = 1;
    uint8_t * started = (uint8_t *) 1;
    int context_client = 7;
    std::thread tb( [&]
                    {
        finished = rocketnet_server_finish_client_message( world.server, slot0, 8 );
        rocketnet_packet_context_t context;
        memset( &context, 0, sizeof( context ) );
        context.client_index = 7;
        started = rocketnet_server_transport_start_packet( transport, slot0, 8, &context );
        context_client = context.client_index;
        check( rocketnet_server_transport_finish_packet( transport, &context, 8 ) == 0 );
        rocketnet_server_transport_abort_packet( transport, &context ); } );
    tb.join();

    check( finished == 0 );
    check( started == NULL );
    check( context_client == -1 );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) == two + 2 );
    check( rn_message_open_count( object ) == 1 );
    check( object->connections[slot0].open.pending == 1 );

    // the next main-thread operation closes the open start

    rocketnet_server_send_packets( world.server );
    check( rn_message_open_count( object ) == 0 );
    check( object->connections[slot0].open.pending == 0 );
    check( object->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED] == refused + 1 );

    // its finish, now on the opening thread, finds nothing open

    check( rocketnet_server_finish_client_message( world.server, slot0, 8 ) == 0 );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

// ---------------------------------------------------------------------------
// the writer's finish: a packet finished on the thread that wrote it
// ---------------------------------------------------------------------------
//
// rocketnet_server_finish_client_message seals the packet of an unreliable
// message written in place inside the call (ack block, sequence, packet id,
// encryption, handed to the transport), and refuses an unreliable start that
// found no capacity (MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY) instead of
// queueing it for send_packets. The serial finish is unchanged. One trace is
// run through the serial finish on one thread, the writer's finish on one
// thread, and the writer's finish on four and on eight threads, ownership rotating
// every frame; per client, what was sent, acked and received must be equal.

#define WRITER_FINISH_CLIENTS 8
#define WRITER_FINISH_PACKET_BYTES 1200

enum writer_finish_mode_t
{
    WRITER_FINISH_SERIAL,           // start + rocketnet_server_finish_message, one thread
    WRITER_FINISH_CLIENT,           // start + rocketnet_server_finish_client_message
};

struct writer_finish_trace_t
{
    uint64_t packets_sent[WIDE_FILL_MAX_CLIENTS];     // per client, from the session's stats, after connect
    uint64_t bytes_sent[WIDE_FILL_MAX_CLIENTS];
    uint64_t acked[WIDE_FILL_MAX_CLIENTS];            // per client, packet ids the transport surfaced as acked
    uint64_t acked_id_sum[WIDE_FILL_MAX_CLIENTS];     // per client, the sum of those ids above the run's lowest (a block names its ids highest first, so their order is not compared)
    uint64_t acked_twice;                             // a packet id surfaced as acked more than once
    uint32_t received[WIDE_FILL_MAX_CLIENTS][3];      // per client, messages taken by kind
    uint64_t corrupt;
    uint64_t finished;                                // finishes that returned a serial
    uint64_t refused;                                 // finishes that returned 0
    uint64_t walk_packets;                            // packets send_packets finished
    uint64_t no_capacity;                             // MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY, moved
    uint64_t fallback;                                // MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK, moved
};

struct writer_finish_fill_t
{
    rocketnet_server_t * server;
    wide_fill_slot_t * slots;
    const int * slot_of_client;
    int num_clients;
    int num_threads;
    int frame;
    int unreliable_per_frame;
    writer_finish_mode_t mode;
    uint64_t finished[8];
    uint64_t refused[8];
};

static void writer_finish_job( writer_finish_fill_t * f, int thread )
{
    for ( int c = 0; c < f->num_clients; c++ )
    {
        if ( ( c + f->frame ) % f->num_threads != thread )
        {
            continue;
        }

        const int slot = f->slot_of_client[c];
        wide_fill_slot_t * s = &f->slots[slot];
        for ( int m = 0; m <= f->unreliable_per_frame; m++ )
        {
            // packet-sized unreliable payloads, then one small reliable message
            const int k = m < f->unreliable_per_frame ? 0 : 2;
            const int bytes = k == 0 ? WRITER_FINISH_PACKET_BYTES : WIDE_FILL_HEADER_BYTES + 6;
            uint8_t * p = rocketnet_server_start_message( f->server, (uint64_t) slot, 0, (enum rocketnet_reliability_t) wide_fill_kinds[k], bytes );
            check( p != NULL );
            if ( !p )
            {
                continue;
            }
            wide_fill_write( p, bytes, k, slot, s->sent[k], (uint32_t) f->frame );
            const uint32_t serial = f->mode == WRITER_FINISH_SERIAL ? rocketnet_server_finish_message( f->server, bytes )
                                                                    : rocketnet_server_finish_client_message( f->server, slot, bytes );
            if ( serial == 0 )
            {
                f->refused[thread]++;
                check( k == 0 );                        // only an unreliable start with no capacity is refused
                continue;
            }
            f->finished[thread]++;
            s->sent[k]++;
        }
    }
}

// the main thread's half of a frame: what was filled goes out, then update,
// the acked ids read, and every receive drained
static void writer_finish_main( wide_fill_world_t * world, wide_fill_slot_t * slots, const int * slot_of_client, writer_finish_trace_t * trace,
                                std::vector<uint64_t> * acked_ids )
{
    rn_send_pass_cost_reset();
    rocketnet_server_send_packets( world->server );
    trace->walk_packets += rn_send_pass_cost_read().frames_finished;

    wide_fill_clients_step( world );
    rocketnet_server_update( world->server, world->time );

    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( world->server ) );
    const int * client_index = NULL;
    const uint64_t * packet_ids = NULL;
    const int acked = rocketnet_server_transport_acked_packets( transport, &client_index, &packet_ids );
    for ( int i = 0; i < acked; i++ )
    {
        const int slot = client_index[i];
        check( slot >= 0 && slot < WIDE_FILL_MAX_CLIENTS );
        if ( slot < 0 || slot >= WIDE_FILL_MAX_CLIENTS )
        {
            continue;
        }
        trace->acked[slot]++;
        acked_ids[slot].push_back( packet_ids[i] );
    }

    while ( rocketnet_server_receive_messages( world->server ) != NULL )
    {
    }
    for ( int c = 0; c < world->num_clients; c++ )
    {
        wide_fill_client_receive( world, c, &slots[slot_of_client[c]], slot_of_client[c] );
    }
}

// the message layer's counters (the fallback and the refusal are counted there)
static uint64_t writer_finish_message_counter( rocketnet_server_t * server, int counter )
{
    const rocketnet_counters_t * counters = rocketnet_message_server_counters( rocketnet_server_message_server( server ) );
    return counters ? counters->values[counter] : 0;
}

static void writer_finish_run( writer_finish_mode_t mode, int num_threads, int packets_per_update, int frames, int unreliable_per_frame,
                               writer_finish_trace_t * trace )
{
    memset( trace, 0, sizeof( *trace ) );

    wide_fill_world_t world;
    wide_fill_world_create( &world, WRITER_FINISH_CLIENTS, packets_per_update );
    wide_fill_connect_all( &world );

    wide_fill_slot_t slots[WIDE_FILL_MAX_CLIENTS];
    memset( slots, 0, sizeof( slots ) );
    int slot_of_client[WIDE_FILL_MAX_CLIENTS];
    std::vector<uint64_t> acked_ids[WIDE_FILL_MAX_CLIENTS];
    rocketnet_server_client_stats_t before[WIDE_FILL_MAX_CLIENTS];
    for ( int c = 0; c < world.num_clients; c++ )
    {
        slot_of_client[c] = rocketnet_client_index( world.clients[c] );
        slots[slot_of_client[c]].last_sequenced = -1;
        slots[slot_of_client[c]].last_unreliable = -1;
        rocketnet_server_client_stats( world.server, slot_of_client[c], &before[slot_of_client[c]] );
    }

    const uint64_t no_capacity = writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY );
    const uint64_t fallback = writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );

    for ( int frame = 0; frame < frames; frame++ )
    {
        writer_finish_main( &world, slots, slot_of_client, trace, acked_ids );

        writer_finish_fill_t f;
        memset( &f, 0, sizeof( f ) );
        f.server = world.server;
        f.slots = slots;
        f.slot_of_client = slot_of_client;
        f.num_clients = world.num_clients;
        f.num_threads = num_threads;
        f.frame = frame;
        f.unreliable_per_frame = unreliable_per_frame;
        f.mode = mode;

        if ( num_threads == 1 )
        {
            writer_finish_job( &f, 0 );
        }
        else
        {
            std::vector<std::thread> threads;
            for ( int t = 0; t < num_threads; t++ )
            {
                threads.emplace_back( writer_finish_job, &f, t );
            }
            for ( std::thread & thread : threads )
            {
                thread.join();
            }
        }
        for ( int t = 0; t < num_threads; t++ )
        {
            trace->finished += f.finished[t];
            trace->refused += f.refused[t];
        }

        world.time += 1.0 / 60.0;
    }

    for ( int step = 0; step < 60; step++ )
    {
        writer_finish_main( &world, slots, slot_of_client, trace, acked_ids );
        world.time += 1.0 / 60.0;
    }

    for ( int c = 0; c < world.num_clients; c++ )
    {
        const int slot = slot_of_client[c];
        rocketnet_server_client_stats_t after;
        rocketnet_server_client_stats( world.server, slot, &after );
        trace->packets_sent[slot] = after.packets_sent - before[slot].packets_sent;
        trace->bytes_sent[slot] = after.bytes_sent - before[slot].bytes_sent;
        for ( int k = 0; k < 3; k++ )
        {
            trace->received[slot][k] = slots[slot].received[k];
        }
        trace->corrupt += slots[slot].corrupt;
        check( slots[slot].received[2] == slots[slot].sent[2] );     // every reliable message, once, in order
    }

    // packet ids come from one counter the whole process shares, so they are
    // read from the run's lowest acked id; no id is acked twice, on any client
    std::vector<uint64_t> all_acked;
    for ( int slot = 0; slot < WIDE_FILL_MAX_CLIENTS; slot++ )
    {
        all_acked.insert( all_acked.end(), acked_ids[slot].begin(), acked_ids[slot].end() );
    }
    std::sort( all_acked.begin(), all_acked.end() );
    for ( size_t i = 1; i < all_acked.size(); i++ )
    {
        if ( all_acked[i] == all_acked[i - 1] )
        {
            trace->acked_twice++;
        }
    }
    for ( int slot = 0; slot < WIDE_FILL_MAX_CLIENTS; slot++ )
    {
        for ( const uint64_t id : acked_ids[slot] )
        {
            trace->acked_id_sum[slot] += id - all_acked[0];
        }
    }

    trace->no_capacity = writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY ) - no_capacity;
    trace->fallback = writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) - fallback;

    wide_fill_world_destroy( &world );
}

// same_ids: the two runs started their packets in the same order (one thread
// each), so every client's acked ids are the same ids; with several threads
// the ids interleave between clients as the threads ran, and only how many
// were acked is compared
static bool writer_finish_same_accounting( const writer_finish_trace_t * a, const writer_finish_trace_t * b, bool same_ids )
{
    bool same = true;
    for ( int slot = 0; slot < WIDE_FILL_MAX_CLIENTS; slot++ )
    {
        same = same && a->packets_sent[slot] == b->packets_sent[slot];
        same = same && a->bytes_sent[slot] == b->bytes_sent[slot];
        same = same && a->acked[slot] == b->acked[slot];
        same = same && ( !same_ids || a->acked_id_sum[slot] == b->acked_id_sum[slot] );
        for ( int k = 0; k < 3; k++ )
        {
            same = same && a->received[slot][k] == b->received[slot][k];
        }
        if ( !same )
        {
            printf( "writer finish accounting differs at client %d: packets %llu %llu, bytes %llu %llu, acked %llu %llu, acked id sum %llu %llu, received %u,%u,%u %u,%u,%u\n",
                    slot, (unsigned long long) a->packets_sent[slot], (unsigned long long) b->packets_sent[slot],
                    (unsigned long long) a->bytes_sent[slot], (unsigned long long) b->bytes_sent[slot],
                    (unsigned long long) a->acked[slot], (unsigned long long) b->acked[slot],
                    (unsigned long long) a->acked_id_sum[slot], (unsigned long long) b->acked_id_sum[slot],
                    a->received[slot][0], a->received[slot][1], a->received[slot][2],
                    b->received[slot][0], b->received[slot][1], b->received[slot][2] );
            return false;
        }
    }
    return same;
}

// packet-sized payloads: the writer's finish leaves send_packets one packet
// fewer per client per frame (the last packet the serial finish left open),
// and sends, acks and delivers exactly what the serial finish does
static void test_writer_finish_equals_serial()
{
    const int frames = 30;
    const int per_client = 3;

    writer_finish_trace_t serial;
    writer_finish_trace_t one;
    writer_finish_trace_t four;
    writer_finish_trace_t eight;
    writer_finish_run( WRITER_FINISH_SERIAL, 1, 0, frames, per_client, &serial );
    writer_finish_run( WRITER_FINISH_CLIENT, 1, 0, frames, per_client, &one );
    writer_finish_run( WRITER_FINISH_CLIENT, 4, 0, frames, per_client, &four );
    writer_finish_run( WRITER_FINISH_CLIENT, 8, 0, frames, per_client, &eight );

    const uint64_t messages = (uint64_t) frames * WRITER_FINISH_CLIENTS * ( per_client + 1 );
    for ( const writer_finish_trace_t * t : { &serial, &one, &four, &eight } )
    {
        check( t->finished == messages );
        check( t->refused == 0 );
        check( t->no_capacity == 0 );
        check( t->fallback == 0 );
        check( t->corrupt == 0 );
        check( t->acked_twice == 0 );
    }

    check( writer_finish_same_accounting( &serial, &one, true ) );
    check( writer_finish_same_accounting( &serial, &four, false ) );
    check( writer_finish_same_accounting( &serial, &eight, false ) );

    uint64_t sent = 0;
    uint64_t acked = 0;
    for ( int slot = 0; slot < WIDE_FILL_MAX_CLIENTS; slot++ )
    {
        sent += serial.packets_sent[slot];
        acked += serial.acked[slot];
    }
    check( sent >= (uint64_t) frames * WRITER_FINISH_CLIENTS * ( per_client + 1 ) );
    check( acked > 0 );

    // the serial finish leaves each client's last unreliable packet open for
    // send_packets; the writer's finish leaves it nothing of the unreliable
    check( serial.walk_packets == one.walk_packets + (uint64_t) frames * WRITER_FINISH_CLIENTS );
    check( one.walk_packets == four.walk_packets );
    check( one.walk_packets == eight.walk_packets );
}

// packets_per_update 2 and four packet-sized payloads per client: the third
// and fourth find the update's allowance spent. The serial finish queues them
// for send_packets; the writer's finish refuses them, each refusal counted
// once by value, and send_packets builds none of them
static void test_writer_finish_no_capacity_refused()
{
    const int frames = 10;
    const int per_client = 4;

    writer_finish_trace_t serial;
    writer_finish_trace_t writer;
    writer_finish_trace_t wide;
    writer_finish_run( WRITER_FINISH_SERIAL, 1, 2, frames, per_client, &serial );
    writer_finish_run( WRITER_FINISH_CLIENT, 1, 2, frames, per_client, &writer );
    writer_finish_run( WRITER_FINISH_CLIENT, 4, 2, frames, per_client, &wide );

    check( serial.refused == 0 );
    check( serial.no_capacity == 0 );
    check( serial.fallback >= (uint64_t) frames * WRITER_FINISH_CLIENTS * 2 );

    for ( const writer_finish_trace_t * t : { &writer, &wide } )
    {
        check( t->refused >= (uint64_t) frames * WRITER_FINISH_CLIENTS * 2 );
        check( t->no_capacity == t->refused );          // by value: one count per refused finish
        check( t->fallback == t->refused );             // each refusal was a start that fell back
        check( t->corrupt == 0 );
        check( t->acked_twice == 0 );
    }
    check( writer_finish_same_accounting( &writer, &wide, false ) );
    check( writer.refused == wide.refused );

    // the serial run's walk built the queued unreliable payloads; the
    // writer's walk built none of them
    check( serial.walk_packets > writer.walk_packets );
}

// A full server on a buffered transport (the in-memory one): 1000 clients,
// every slot connected, so the server's send ring (one slot a client) holds
// exactly the round the main thread's send keeps and no slot beyond it. The
// game threads finish one packet-sized unreliable message per client per
// frame. None is refused: each waits for the main thread's send, as the
// serial finish's does, and every one arrives
// (MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY reads 0 by value).
struct full_server_frame_t
{
    rocketnet_server_t * server;
    const int * slot_of_client;
    int num_clients;
    int num_threads;
    int frame;
    int bytes;
    uint32_t * not_started;                             // one a thread
    uint32_t * not_finished;                            // one a thread
};

static void full_server_job( const full_server_frame_t * f, int thread )
{
    for ( int c = 0; c < f->num_clients; c++ )
    {
        if ( ( c + f->frame ) % f->num_threads != thread )
        {
            continue;                                   // another thread's client this frame
        }
        const int slot = f->slot_of_client[c];
        uint8_t * p = rocketnet_server_start_message( f->server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, f->bytes );
        if ( !p )
        {
            f->not_started[thread]++;
            continue;
        }
        wide_fill_write( p, f->bytes, 0, slot, (uint32_t) f->frame, (uint32_t) f->frame );
        if ( rocketnet_server_finish_client_message( f->server, slot, f->bytes ) == 0 )
        {
            f->not_finished[thread]++;
        }
    }
}

static void test_writer_finish_full_server_buffered_all_delivered()
{
    const int clients = WIDE_FILL_FULL_SERVER_CLIENTS;
    const int num_threads = 8;
    const int frames = 4;
    const int bytes = 1000;

    wide_fill_world_t world;
    wide_fill_world_create( &world, clients, 0, clients );
    wide_fill_connect_all( &world );

    std::vector<int> slot_of_client( (size_t) clients );
    std::vector<wide_fill_slot_t> slots( (size_t) clients );
    memset( slots.data(), 0, slots.size() * sizeof( wide_fill_slot_t ) );
    for ( int c = 0; c < clients; c++ )
    {
        slot_of_client[(size_t) c] = rocketnet_client_index( world.clients[c] );
        slots[(size_t) slot_of_client[(size_t) c]].last_unreliable = -1;
    }

    const uint64_t no_capacity = writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY );
    const uint64_t fallback = writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK );
    uint32_t not_started[8] = {};
    uint32_t not_finished[8] = {};

    for ( int frame = 0; frame < frames + 8; frame++ )
    {
        // main thread: what the game threads finished goes out, then update and receive

        rocketnet_server_send_packets( world.server );
        wide_fill_clients_step( &world );
        rocketnet_server_update( world.server, world.time );
        while ( rocketnet_server_receive_messages( world.server ) != NULL )
        {
        }
        for ( int c = 0; c < clients; c++ )
        {
            const int slot = slot_of_client[(size_t) c];
            wide_fill_client_receive( &world, c, &slots[(size_t) slot], slot );
        }

        // the game threads, joined before the next main thread call; the
        // last eight frames only drain

        if ( frame < frames )
        {
            full_server_frame_t f;
            f.server = world.server;
            f.slot_of_client = slot_of_client.data();
            f.num_clients = clients;
            f.num_threads = num_threads;
            f.frame = frame;
            f.bytes = bytes;
            f.not_started = not_started;
            f.not_finished = not_finished;

            std::vector<std::thread> threads;
            for ( int t = 0; t < num_threads; t++ )
            {
                threads.emplace_back( full_server_job, &f, t );
            }
            for ( std::thread & thread : threads )
            {
                thread.join();
            }
        }

        world.time += 1.0 / 60.0;
    }

    for ( int t = 0; t < num_threads; t++ )
    {
        check( not_started[t] == 0 );
        check( not_finished[t] == 0 );
    }
    for ( int c = 0; c < clients; c++ )
    {
        const wide_fill_slot_t * s = &slots[(size_t) slot_of_client[(size_t) c]];
        check( s->corrupt == 0 );
        check( s->received[0] == (uint32_t) frames );   // every one delivered, once
    }
    check( writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY ) == no_capacity );

    // the case was the one meant: every start found the ring without a slot for it
    check( writer_finish_message_counter( world.server, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK ) - fallback == (uint64_t) clients * frames );

    wide_fill_world_destroy( &world );
}

REGISTER_UNIT_TEST( test_writer_finish_equals_serial );
REGISTER_UNIT_TEST( test_writer_finish_full_server_buffered_all_delivered );
REGISTER_UNIT_TEST( test_writer_finish_no_capacity_refused );
// The default, one thread at a time and the calls that name no client: a
// message started on one thread is finished, or aborted, on another, with no
// server call between the start and its end. Nothing is refused and nothing
// is counted: the thread rule check hands the client over by one main thread
// call of its own.
static void wide_fill_start_here_close_there( bool finish )
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    const int slot = rocketnet_client_index( world.clients[1] );
    rn_message_object_t * object = &rocketnet_server_message_server( world.server )->object;

    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    const uint64_t two = wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS );
    const uint64_t misuse = rocketnet_api_misuse_count();

    for ( int round = 0; round < 3; round++ )
    {
        uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x61 + round, 8 );
        }
        check( rn_message_open_count( object ) == 1 );

        uint32_t serial = 0;
        std::thread other( [&]()
                           {
            if ( finish )
            {
                serial = rocketnet_server_finish_message( world.server, 8 );
            }
            else
            {
                rocketnet_server_abort_message( world.server );
            } } );
        other.join();

        check( rn_message_open_count( object ) == 0 );
        check( object->connections[slot].open.pending == 0 );
        check( finish ? serial != 0 : serial == 0 );

        // the frame goes on: the next start for this client by the first
        // thread follows a main thread call (without one it would be a second
        // thread on the client, which is refused: CLIENT_SEND_TWO_THREADS)
        wide_fill_pump( &world );
    }

    // the starting thread sends to the client again
    uint8_t * again = rocketnet_server_start_message( world.server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( again != NULL );
    if ( again )
    {
        memset( again, 0x6F, 8 );
        check( rocketnet_server_finish_message( world.server, 8 ) != 0 );
    }

    // the same once the game has named a client and the rules are checked:
    // the call that names no client hands the client over to its thread
    wide_fill_pump( &world );
    wide_fill_opt_in( &world );
    {
        uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x6E, 8 );
        }
        uint32_t serial = 0;
        std::thread other( [&]()
                           {
            if ( finish )
            {
                serial = rocketnet_server_finish_message( world.server, 8 );
            }
            else
            {
                rocketnet_server_abort_message( world.server );
            } } );
        other.join();
        check( rn_message_open_count( object ) == 0 );
        check( finish ? serial != 0 : serial == 0 );
    }

    check( g_wide_fill_asserts == 0 );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) == two );
    check( rocketnet_api_misuse_count() == misuse );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

static void test_wide_fill_default_start_on_one_thread_finish_on_another()
{
    wide_fill_start_here_close_there( true );
}

static void test_wide_fill_default_start_on_one_thread_abort_on_another()
{
    wide_fill_start_here_close_there( false );
}

// The default, one thread at a time and today's calls only: two threads send
// to one client in turn with no main thread call between, as a game that
// shares its server under a lock may. Nothing is refused and nothing is
// counted. Once the game has made a call that names a client, the same turn
// is a second thread on the client: refused and counted.
static void test_wide_fill_default_two_threads_one_client_in_turn()
{
    rocketnet_assert_function( wide_fill_assert_hook );
    g_wide_fill_asserts = 0;

    wide_fill_world_t world;
    wide_fill_world_create( &world, 2 );
    wide_fill_connect_all( &world );
    const int slot = rocketnet_client_index( world.clients[0] );

    const uint64_t overlap = wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
    const uint64_t two = wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS );
    const uint64_t misuse = rocketnet_api_misuse_count();

    auto send_one = [&]( uint8_t value ) -> uint32_t
    {
        uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        if ( !p )
        {
            return 0;
        }
        memset( p, value, 8 );
        return rocketnet_server_finish_message( world.server, 8 );
    };

    for ( int round = 0; round < 3; round++ )
    {
        check( send_one( 0x71 ) != 0 );
        uint32_t serial = 0;
        std::thread other( [&]()
                           { serial = send_one( 0x72 ); } );
        other.join();
        check( serial != 0 );
        check( send_one( 0x73 ) != 0 );
    }
    check( g_wide_fill_asserts == 0 );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) == overlap );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) == two );
    check( rocketnet_api_misuse_count() == misuse );

    // the game names a client: from here one thread a client is the rule
    wide_fill_pump( &world );
    rocketnet_server_abort_client_message( world.server, slot );
    wide_fill_pump( &world );
    // the client named, as a game filling from several threads sends: the
    // plain finish is now a main operation, whose end begins a new fill
    // phase and so would end this thread's claim
    uint8_t * mine = rocketnet_server_start_message( world.server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( mine != NULL );
    memset( mine, 0x74, 8 );
    check( rocketnet_server_finish_client_message( world.server, slot, 8 ) != 0 );
    uint8_t * refused = (uint8_t *) &world;
    std::thread second( [&]()
                        { refused = rocketnet_server_start_message( world.server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 ); } );
    second.join();
    check( refused == NULL );
    check( wide_fill_counter( world.server, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) == two + 1 );
    check( g_wide_fill_asserts == wide_fill_expected_asserts( 1 ) );

    wide_fill_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_wide_fill_one_thread );
REGISTER_UNIT_TEST( test_wide_fill_default_two_threads_one_client_in_turn );
REGISTER_UNIT_TEST( test_wide_fill_default_start_on_one_thread_finish_on_another );
REGISTER_UNIT_TEST( test_wide_fill_default_start_on_one_thread_abort_on_another );
REGISTER_UNIT_TEST( test_wide_fill_four_threads );
REGISTER_UNIT_TEST( test_wide_fill_eight_threads_large );
REGISTER_UNIT_TEST( test_wide_fill_update_in_three_calls );
REGISTER_UNIT_TEST( test_wide_fill_check_steps );
REGISTER_UNIT_TEST( test_wide_fill_check_depth_bound );
REGISTER_UNIT_TEST( test_wide_fill_check_armed_across_restart );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_wide_fill_check_nested_never_torn );
REGISTER_UNIT_TEST( test_wide_fill_check_running_read_once );
REGISTER_UNIT_TEST( test_wide_fill_check_stop_read_once );
REGISTER_UNIT_TEST( test_wide_fill_check_disconnect_client_read_once );
REGISTER_UNIT_TEST( test_wide_fill_check_disconnect_all_read_once );
REGISTER_UNIT_TEST( test_wide_fill_check_set_send_kbps_read_once );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_wide_fill_check_public );
REGISTER_UNIT_TEST( test_wide_fill_open_per_client );
REGISTER_UNIT_TEST( test_wide_fill_main_refused_during_fill );
REGISTER_UNIT_TEST( test_wide_fill_refused_receive_keeps_batch );
REGISTER_UNIT_TEST( test_wide_fill_split_update_window );
REGISTER_UNIT_TEST( test_wide_fill_split_update_open_update_call );
REGISTER_UNIT_TEST( test_wide_fill_plain_finish_checked );
REGISTER_UNIT_TEST( test_wide_fill_split_update_window_on_early_return );
REGISTER_UNIT_TEST( test_wide_fill_refused_finish_and_start );
