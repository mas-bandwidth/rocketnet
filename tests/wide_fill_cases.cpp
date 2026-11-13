/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The wide fill: the game's own threads fill and seal each client's sends
// between update and send_packets, one thread per client at a time
// (sdk/rocketnet_fill_phase.h, proofs/tla/RNFillPhase.tla), each holding one
// open message on its own client (proofs/tla/RNMessageOpen.tla).
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

#define check rocketnet_unit_check

#define WIDE_FILL_PROTOCOL 0x57494445F1110001ULL
#define WIDE_FILL_MAX_CLIENTS 16

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
    rocketnet_client_t * clients[WIDE_FILL_MAX_CLIENTS];
    int num_clients;
    double time;
};

static void wide_fill_world_create( wide_fill_world_t * world, int num_clients )
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

    world->hub = rocketnet_loopback_hub_create( NULL, WIDE_FILL_MAX_CLIENTS, NULL );
    check( world->hub != NULL );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = WIDE_FILL_PROTOCOL;
    server_config.max_clients = WIDE_FILL_MAX_CLIENTS;
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = world->hub;

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

    for ( int step = 0; step < 600; step++ )
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

static void wide_fill_run( int num_threads, int frames, int unreliable_per_frame, int bytes, bool one_thread_finish )
{
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
        rocketnet_server_update( world.server, world.time );
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
        rocketnet_server_update( world.server, world.time );
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
        std::thread other( [&]() {
            if ( finish )
            {
                serial = rocketnet_server_finish_message( world.server, 8 );
            }
            else
            {
                rocketnet_server_abort_message( world.server );
            }
        } );
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
    check( send_one( 0x74 ) != 0 );
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
REGISTER_UNIT_TEST( test_wide_fill_check_steps );
REGISTER_UNIT_TEST( test_wide_fill_check_public );
REGISTER_UNIT_TEST( test_wide_fill_open_per_client );
REGISTER_UNIT_TEST( test_wide_fill_main_refused_during_fill );
REGISTER_UNIT_TEST( test_wide_fill_refused_finish_and_start );
