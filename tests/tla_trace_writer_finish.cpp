/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the writer's finish
// (proofs/tla/README-TraceCheck.md, proofs/tla/RNWriterFinish.tla,
// proofs/tla/TraceRNWriterFinish.tla).
//
// The code under test is the message server's start, writer's finish and
// abort in sdk/rocketnet_message_server.cpp (rn_message_object_start,
// rn_message_object_finish_client behind
// rocketnet_server_finish_client_message, rn_message_object_abort,
// rn_message_object_send_packets): a message server with WF_CLIENTS clients
// seated, its transport's I/O swapped for a fake. The seed picks the
// transport's kind, and the walk writes it as the model's Buffered: an odd
// seed is a staged transport of one queue whose room the fake owns (Buffered
// = FALSE); an even seed is a buffered one, the frames held in the server's
// own ring and handed to the fake by its flush (Buffered = TRUE), the ring
// created with a slot a client and WF_CAPACITY more. Seed 3 modulo 4, a
// staged one, runs with a send budget (Budgeted = TRUE): before a small
// start the walk may lower the bucket to what pays a frame of the message's
// own and not a packed one (the whole-frame form), or to less (no capacity),
// and gives the bucket back after the start returns. Two writers are two
// real threads, because a client's owner is named by a thread's id
// (sdk/rocketnet_fill_phase.h); the walk hands one call at a time to one of
// them and waits for it, so a seed is one order of whole calls. Each step is
// one call:
//
//   update        rocketnet_message_server_update on the walk's thread, the
//                 main thread's last operation before the game threads
//                 write: BeginFill
//   start         rocketnet_message_server_start for one client, on a
//                 writer's thread, of one of three kinds: a small unreliable
//                 message (WF_MESSAGE_BYTES, StartSmall), an unreliable
//                 reservation over one packet (WF_LARGE_BYTES, StartLarge),
//                 or a reliable message (StartReliable). The walk keeps to
//                 one writer per client between two main thread calls, as
//                 the game must: the client's owner is read from the code,
//                 and a client nobody owns goes to the writer the seed
//                 picks. The refusal of a second thread is the fillphase
//                 layer's (tests/tla_trace_fill_phase.cpp)
//   finish        rocketnet_message_server_finish_client for a client with a
//                 start open, on its owner's thread, with bytes_written
//                 WF_MESSAGE_BYTES (Finish; on a large start FinishRestart,
//                 a size one packet carries), WF_OVER_BYTES on a large start
//                 (FinishOver), or outside 1..reserved (FinishBadBytes)
//   abort         rocketnet_message_server_abort for a client with a start
//                 open, on its owner's thread: Abort
//   send_packets  rocketnet_message_server_send_packets on the walk's
//                 thread, with no start open: SendPackets
//
// What each call did is in the state the record carries, read from the code
// after the call:
//
//   open     per client, the connection's open start: none; inplace or
//            whole, a transport frame taken (open.in_place 2, a packed
//            build, or 1, a frame of its own); nocap
//            (RN_MESSAGE_OPEN_PENDING_NO_CAPACITY); or a start pending with
//            no in-place form and not marked as having no capacity (the
//            queued path), named by what the walk asked and what the code
//            held before the call: large or reliable by the kind started;
//            for a small one behind, when the client's endpoint held an
//            unreliable message queued before the call, or ring, when the
//            transport is buffered (rocketnet_server_transport_send_buffered)
//            and cap, read from the code just before the call, was 0. Any
//            other queued start is written "queued", which the model does
//            not have
//   build    per client, 1 while its unreliable build holds records unsealed
//   queued   per client, the unreliable messages its endpoint holds queued
//            for send_packets (unreliable_count)
//   held     per client, the messages its channel 0 holds queued on the
//            reliable path, not yet taken by a send pass (the channel's live
//            queued count): reliable sends and promoted ones
//   owner    per client, the writer whose thread id the one-thread-per-client
//            check (sdk/rocketnet_fill_phase.h) holds as the client's owner
//            in its current epoch, or none
//   cap      the frames a start may still take in place: the transport's
//            send capacity above the round the message layer keeps for
//            send_packets (one frame a connected client), the expression
//            rn_message_object_start tests
//   starts   the writers' starts that gave a pointer
//   sent     the packets the writers' finishes sealed: the moves across
//            each finish call of the staged fake's count of frames handed
//            back finished, or on a buffered transport of the ring's count
//            of sealed slots
//            (rocketnet_server_transport_payload_ring_sealed_for_test)
//   refused  MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY, read by value
//   lost     the moves of MESSAGE_UNRELIABLE_OVER_BUDGET and
//            MESSAGE_UNRELIABLE_SEND_FAILED across the writers' finishes
//   walked   the writer messages send_packets took: at each send_packets,
//            the build, queued and held the code holds just before it,
//            less what it still holds just after
//
// Three variables the code holds no word for are kept by the walk from the
// calls the code ran, as the fillphase layer keeps its control state and its
// contract's history:
//
//   phase     main after a send_packets, fill after an update
//   touched   per client, the writers whose start, finish or abort the code
//             ran on it since the last update: the history OneWriter reads.
//             The code keeps only the one owner, above
//   cancelled the aborts, and the finishes with a bad bytes_written, the
//             code ran: each checked to have returned 0, left no start
//             open and moved no counter of the refusal's
//
// The row has no ghost. A finish's return value is checked here against the
// refusal it must match (0 exactly when the refusal's counter moved by one,
// or for a bad bytes_written), a start must give a pointer, and no call may
// be refused by that check: any of these fails the walk.
//
// The plant (ROCKETNET_WRITER_FINISH_PLANT, the
// tla_trace_plant_writer_finish build): the writer's finish of a start that
// found no capacity queues it for send_packets, as the serial finish does,
// in place of refusing it; and on a buffered transport a start the ring had
// no slot for is marked as having no capacity. PLANT-FIRED counts, on a
// staged transport, the records where a client holds an unreliable message
// queued, and on a buffered one the records where a start is open as having
// no capacity: neither is ever shown by the clean walk on that transport.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_WRITER_FINISH_PLANT 1
#include "../sdk/rocketnet_message_server.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <condition_variable>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_fill_phase.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_packet_batch.h"
#include "../sdk/rocketnet_platform.h"
#include "tla_trace_record.h"

bool rocketnet_server_transport_seat_clients_for_test( struct rocketnet_server_transport_t * server, int clients );

#define WF_CLIENTS       2
#define WF_WRITERS       2
#define WF_CAPACITY      3      // the frames an update leaves the writers, above the round kept for send_packets
#define WF_FRAMES        16     // the fake's frames: more than can be open at once
#define WF_MESSAGE_BYTES 16
#define WF_LARGE_BYTES   2000   // an unreliable reservation over one packet
#define WF_OVER_BYTES    1500   // its finish over one packet: promoted
#define WF_BUDGET_KBPS   1000000        // a bucket of 100 ms at this rate holds the whole walk
#define WF_TIME          100.0  // the time the seam seats the clients at, and the walk's one time: no step of the model is time's

enum wf_kind_t
{
    WF_SMALL,
    WF_LARGE,
    WF_RELIABLE,
};

// the fake staged transport: one queue. A start takes a frame of the room,
// and a flush sends everything staged, so the room is whole again
struct wf_fake_t
{
    int room;                     // free frames
    int max_room;                 // the room after a flush
    uint64_t started;             // frames handed out
    uint64_t finished;            // frames handed back finished
    bool used[WF_FRAMES];
};

static wf_fake_t wf_fake;

alignas( 16 ) static uint8_t wf_frames[WF_FRAMES][ROCKETNET_FRAME_STRIDE];

static std::mutex wf_fake_mutex;

static uint8_t * wf_start( void *, const rocketnet_address_t *, uint64_t * id )
{
    std::lock_guard<std::mutex> lock( wf_fake_mutex );
    *id = 0;
    if ( wf_fake.room <= 0 )
    {
        return NULL;
    }
    for ( int i = 0; i < WF_FRAMES; i++ )
    {
        if ( !wf_fake.used[i] )
        {
            wf_fake.used[i] = true;
            wf_fake.room--;
            wf_fake.started++;
            *id = (uint64_t) i + 1;
            return wf_frames[i];
        }
    }
    return NULL;
}

static void wf_release( uint64_t id )
{
    if ( id >= 1 && id <= WF_FRAMES )
    {
        wf_fake.used[id - 1] = false;
    }
}

static void wf_finish( void *, uint64_t id, uint8_t *, int )
{
    std::lock_guard<std::mutex> lock( wf_fake_mutex );
    wf_fake.finished++;
    wf_release( id );
}

static void wf_abort( void *, uint64_t id, uint8_t * )
{
    std::lock_guard<std::mutex> lock( wf_fake_mutex );
    wf_release( id );
}

static void wf_flush( void * )
{
    std::lock_guard<std::mutex> lock( wf_fake_mutex );
    wf_fake.room = wf_fake.max_room;
}

static int wf_send_room( void * )
{
    std::lock_guard<std::mutex> lock( wf_fake_mutex );
    return wf_fake.room;
}

static bool wf_send_idle( void * )
{
    return true;
}

// an update's receive: nothing arrived
static struct rocketnet_server_socket_process_packets_t wf_received;

static struct rocketnet_server_socket_process_packets_t * wf_receive( void * )
{
    wf_received.num_packets = 0;
    return &wf_received;
}

// the fake buffered transport: the server's own ring holds the frames and
// its flush hands them here; nothing arrives
static void wf_send_batch( void *, const rocketnet_platform_send_t *, int )
{
}

static int wf_receive_batch( void *, rocketnet_address_t *, uint8_t *, int, int *, int )
{
    return 0;
}

// the writers: each a thread that makes one call when the walk hands it one
enum wf_job_t
{
    WF_NONE,
    WF_START,
    WF_FINISH,
    WF_ABORT,
    WF_QUIT,
};

struct wf_writers_t
{
    std::mutex mutex;
    std::condition_variable changed;
    struct rocketnet_message_server_t * server;
    wf_job_t job[WF_WRITERS];
    int client[WF_WRITERS];
    int reliability;                // the start's reliability
    int bytes;                      // the start's max_bytes, or the finish's bytes_written
    uint64_t id[WF_WRITERS];        // each writer's thread id, 0 until it has started
    uint8_t * pointer;              // what the last start returned
    uint32_t serial;                // what the last finish returned
};

static void wf_writer( wf_writers_t * writers, int w )
{
    std::unique_lock<std::mutex> lock( writers->mutex );
    writers->id[w] = rocketnet_platform_thread_id();
    writers->changed.notify_all();
    for ( ;; )
    {
        writers->changed.wait( lock, [&]
                               { return writers->job[w] != WF_NONE; } );
        const wf_job_t job = writers->job[w];
        const int client = writers->client[w];
        if ( job == WF_START )
        {
            uint8_t * pointer = rocketnet_message_server_start( writers->server, client, 0, writers->reliability, writers->bytes );
            if ( pointer )
            {
                memset( pointer, 0x5A, (size_t) writers->bytes );
            }
            writers->pointer = pointer;
        }
        else if ( job == WF_FINISH )
        {
            writers->serial = rocketnet_message_server_finish_client( writers->server, client, writers->bytes );
        }
        else if ( job == WF_ABORT )
        {
            rocketnet_message_server_abort( writers->server, client );
        }
        writers->job[w] = WF_NONE;
        writers->changed.notify_all();
        if ( job == WF_QUIT )
        {
            return;
        }
    }
}

// one call on writer w's thread, run to its end
static void wf_call( wf_writers_t * writers, int w, wf_job_t job, int client, int reliability = ROCKETNET_RELIABILITY_UNRELIABLE, int bytes = WF_MESSAGE_BYTES )
{
    std::unique_lock<std::mutex> lock( writers->mutex );
    writers->client[w] = client;
    writers->reliability = reliability;
    writers->bytes = bytes;
    writers->job[w] = job;
    writers->changed.notify_all();
    writers->changed.wait( lock, [&]
                           { return writers->job[w] == WF_NONE; } );
}

static std::string wf_writer_name( int w )
{
    return "\"w" + tla_int( w + 1 ) + "\"";
}

struct wf_walk_t
{
    struct rocketnet_message_server_t * server;
    struct rocketnet_server_transport_t * transport;
    struct rn_message_object_t * object;
    wf_writers_t * writers;
    bool fill;                      // the phase: an update ran since the last send_packets
    bool touched[WF_CLIENTS][WF_WRITERS];   // the writers whose calls the code ran on each client since the last update
    int queued_as[WF_CLIENTS];      // the client's open start, when it took the queued path: WF_OPEN_* by the kind asked and the code before the call
    uint64_t sealed;                // the frames the writers' finishes sealed: each finish's move of the sealed count
    uint64_t starts;                // the writers' starts that gave a pointer
    uint64_t cancelled;             // aborts, and finishes refused for a bad bytes_written
    uint64_t lost;                  // the over-budget and send-failed counters' moves across the writers' finishes
    uint64_t walked;                // build, queued and held, summed just before each send_packets
};

// the open start, as the model names it
enum wf_open_t
{
    WF_OPEN_NONE,
    WF_OPEN_INPLACE,
    WF_OPEN_WHOLE,
    WF_OPEN_NOCAP,
    WF_OPEN_RING,
    WF_OPEN_BEHIND,
    WF_OPEN_LARGE,
    WF_OPEN_RELIABLE,
    WF_OPEN_QUEUED,                 // a queued start of no cause the model has
};

// the transport sends through the server's own ring: the code's own answer
static bool wf_buffered( const wf_walk_t * walk )
{
    return rocketnet_server_transport_send_buffered( walk->transport );
}

static uint64_t wf_message_counter( const wf_walk_t * walk, int counter )
{
    return rocketnet_message_server_counters( walk->server )->values[counter];
}

static uint64_t wf_transport_counter( const wf_walk_t * walk, int counter )
{
    return rocketnet_server_transport_counters( walk->transport )->values[counter];
}

// the writer that owns the client in the thread check's current epoch: -1
// none, -2 a thread that is no writer
static int wf_owner( const wf_walk_t * walk, int client )
{
    const struct rocketnet_fill_phase_t * phase = walk->object->fill_phase;
    const uint64_t word = phase->clients[client].claim.load();
    if ( ( word >> ROCKETNET_FILL_PHASE_EPOCH_SHIFT ) != phase->epoch.load() )
    {
        return -1;
    }
    const uint64_t id = phase->clients[client].owner.load();
    if ( id == 0 )
    {
        return -1;
    }
    for ( int w = 0; w < WF_WRITERS; w++ )
    {
        if ( walk->writers->id[w] == id )
        {
            return w;
        }
    }
    return -2;
}

// the client's open start (wf_open_t): read from the code, but for a start
// on the queued path, which the walk names when it is made (queued_as)
static int wf_open( const wf_walk_t * walk, int client )
{
    const struct rn_message_open_t * open = &walk->object->connections[client].open;
    if ( !open->pending )
    {
        return WF_OPEN_NONE;
    }
    if ( open->pending == RN_MESSAGE_OPEN_PENDING_NO_CAPACITY )
    {
        return WF_OPEN_NOCAP;
    }
    if ( open->in_place == 2 )
    {
        return WF_OPEN_INPLACE;
    }
    if ( open->in_place == 1 )
    {
        return WF_OPEN_WHOLE;
    }
    return walk->queued_as[client];
}

// the messages the client's channel 0 holds on the reliable path, not yet
// taken by a send pass: the channel's live queued count
static uint64_t wf_held( const wf_walk_t * walk, int client )
{
    const struct rn_channel_send_t * send = &walk->object->connections[client].region->channel_send[0];
    return send->queued[0] + send->queued[1] + send->queued[2] + send->queued[3];
}

// the frames a start may still take in place: rn_message_object_start's own
// test, the transport's capacity above one frame a connected client
static int wf_cap( const wf_walk_t * walk )
{
    const int available = rocketnet_server_transport_send_capacity_available( walk->transport );
    const int kept = rocketnet_server_transport_num_connected_clients( walk->transport );
    return available > kept ? available - kept : 0;
}

static uint32_t wf_queued( const wf_walk_t * walk, int client )
{
    return walk->object->connections[client].unreliable_count;
}

// the writer messages left for the walk: every client's open build, queued
// and held
static uint64_t wf_left( const wf_walk_t * walk )
{
    uint64_t left = 0;
    for ( int c = 0; c < WF_CLIENTS; c++ )
    {
        const struct rn_message_connection_t * connection = &walk->object->connections[c];
        left += wf_queued( walk, c ) + wf_held( walk, c ) + ( connection->unreliable_build.writer.payload && connection->unreliable_build.writer.records > 0 ? 1u : 0u );
    }
    return left;
}

// the frames sealed so far: the staged fake's count of frames handed back
// finished, or the server ring's count of sealed slots
static uint64_t wf_sealed_now( const wf_walk_t * walk )
{
    if ( wf_buffered( walk ) )
    {
        return rocketnet_server_transport_payload_ring_sealed_for_test( walk->transport );
    }
    std::lock_guard<std::mutex> lock( wf_fake_mutex );
    return wf_fake.finished;
}

// every variable of RNWriterFinish: read from the code and the fake
// transport, but for phase and touched, which the walk keeps from its calls
static tla_record_t wf_state( const wf_walk_t * walk )
{
    static const char * const open_names[] = { "\"none\"", "\"inplace\"", "\"whole\"", "\"nocap\"", "\"ring\"", "\"behind\"", "\"large\"", "\"reliable\"", "\"queued\"" };
    std::vector<std::string> clients, owner, touched, open, build, queued, held;
    for ( int c = 0; c < WF_CLIENTS; c++ )
    {
        const struct rn_message_connection_t * connection = &walk->object->connections[c];
        const int w = wf_owner( walk, c );
        clients.push_back( tla_int( c ) );
        owner.push_back( w == -1 ? "\"none\"" : ( w == -2 ? "\"unknown\"" : wf_writer_name( w ) ) );
        std::string set = "{";
        for ( int t = 0; t < WF_WRITERS; t++ )
        {
            if ( walk->touched[c][t] )
            {
                set += ( set.size() > 1 ? ", " : "" ) + wf_writer_name( t );
            }
        }
        touched.push_back( set + "}" );
        open.push_back( open_names[wf_open( walk, c )] );
        build.push_back( tla_int( connection->unreliable_build.writer.payload && connection->unreliable_build.writer.records > 0 ? 1 : 0 ) );
        queued.push_back( tla_int( (int64_t) wf_queued( walk, c ) ) );
        held.push_back( tla_int( (int64_t) wf_held( walk, c ) ) );
    }

    tla_record_t state;
    state.add( "phase", walk->fill ? "\"fill\"" : "\"main\"" );
    state.add( "owner", tla_function( clients, owner ) );
    state.add( "touched", tla_function( clients, touched ) );
    state.add( "open", tla_function( clients, open ) );
    state.add( "build", tla_function( clients, build ) );
    state.add( "queued", tla_function( clients, queued ) );
    state.add( "held", tla_function( clients, held ) );
    state.add( "cap", tla_int( wf_cap( walk ) ) );
    state.add( "starts", tla_int( (int64_t) walk->starts ) );
    state.add( "sent", tla_int( (int64_t) walk->sealed ) );
    state.add( "refused", tla_int( (int64_t) wf_message_counter( walk, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY ) ) );
    state.add( "cancelled", tla_int( (int64_t) walk->cancelled ) );
    state.add( "lost", tla_int( (int64_t) walk->lost ) );
    state.add( "walked", tla_int( (int64_t) walk->walked ) );
    return state;
}

static uint64_t wf_lost_counters( const wf_walk_t * walk )
{
    return wf_message_counter( walk, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) +
           wf_message_counter( walk, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
}

static int trace_writer_finish( uint64_t seed, int steps, const char * out )
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    enum rocketnet_status_t status = rocketnet_init( NULL, NULL, NULL );
    if ( status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "tla_trace writerfinish: rocketnet_init: %s\n", rocketnet_status_name( status ) );
        return 1;
    }
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 8, &status );
    if ( !hub )
    {
        fprintf( stderr, "tla_trace writerfinish: no loopback hub: %s\n", rocketnet_status_name( status ) );
        rocketnet_term();
        return 1;
    }

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = WF_CLIENTS;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = hub;

    // the transport kind is the seed's: odd staged, even buffered. A buffered
    // server's ring is created with a slot a client and WF_CAPACITY more
    const bool buffered_seed = seed % 2 == 0;

    // no send budget but on seed 3 modulo 4: elsewhere a start is refused its
    // frame by the update's capacity alone
    const bool budget_seed = seed % 4 == 3;
    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    message_config.server_send_kbps = budget_seed ? WF_BUDGET_KBPS : 0;
    if ( buffered_seed )
    {
        rocketnet_server_transport_set_create_send_capacity_for_test( WF_CLIENTS + WF_CAPACITY );
    }

    wf_walk_t walk;
    memset( &walk, 0, sizeof( walk ) );
    walk.server = rocketnet_message_server_create( NULL, "127.0.0.1:0", &config, &message_config, NULL, 0, 0 );
    walk.transport = walk.server ? rocketnet_message_server_transport( walk.server ) : NULL;
    if ( !walk.transport || !rocketnet_server_transport_seat_clients_for_test( walk.transport, WF_CLIENTS ) )
    {
        fprintf( stderr, "tla_trace writerfinish: no server\n" );
        return 1;
    }
    walk.object = &walk.server->object;

    struct rocketnet_server_transport_io_operations_t fake;
    memset( &fake, 0, sizeof( fake ) );
    if ( buffered_seed )
    {
        fake.send_batch = wf_send_batch;
        fake.receive_batch = wf_receive_batch;
    }
    else
    {
        fake.flags = ROCKETNET_TRANSPORT_STAGED;
        fake.start_packet = wf_start;
        fake.finish_packet = wf_finish;
        fake.abort_packet = wf_abort;
        fake.flush = wf_flush;
        fake.send_room = wf_send_room;
        fake.send_idle = wf_send_idle;
        fake.receive = wf_receive;
    }
    const struct rocketnet_server_transport_io_operations_t * operations = &fake;
    void * instance = NULL;
    rocketnet_server_transport_swap_io_for_test( walk.transport, &operations, &instance );

    // before the first record: the message layer observes its clients, each
    // keep-alive is marked acknowledged (no client holds a frame of its own
    // in the reserve), and the staged fake's room is sized so that an update
    // leaves the writers WF_CAPACITY frames above the round kept for
    // send_packets. The reserve is the code's own number, read after an
    // opening; the capacity written for the model is read back from the
    // code after the last opening, on either transport, not taken from
    // WF_CAPACITY

    // the walk drives the message layer below rocketnet_server_finish_client_message,
    // whose first call is what turns the one-thread-a-client check on
    // (sdk/rocketnet_api.cpp): turn it on here, as that call would, so the
    // owner read from the code is the check's from the first record

    rocketnet_fill_phase_arm( walk.object->fill_phase, true );

    memset( &wf_fake, 0, sizeof( wf_fake ) );
    wf_fake.max_room = 4 * WF_FRAMES;
    wf_fake.room = wf_fake.max_room;
    bool fault = false;
    for ( int round = 0; round < 4; round++ )
    {
        for ( int c = 0; c < WF_CLIENTS; c++ )
        {
            rocketnet_server_transport_set_keep_alive_acked_for_test( walk.transport, c, 1 );
        }
        rocketnet_message_server_update( walk.server, WF_TIME );
        rocketnet_message_server_send_packets( walk.server );
        if ( !buffered_seed && ( round == 1 || round == 2 ) )
        {
            // first from the reserve, then by what the code says that left the writers
            const int reserve = (int) rocketnet_server_transport_send_reserve_for_test( walk.transport, 0 );
            const int over = round == 2 ? wf_cap( &walk ) - WF_CAPACITY : 0;
            std::lock_guard<std::mutex> lock( wf_fake_mutex );
            wf_fake.max_room = round == 1 ? reserve + WF_CLIENTS + WF_CAPACITY : wf_fake.max_room - over;
            wf_fake.room = wf_fake.max_room;
        }
    }
    for ( int c = 0; c < WF_CLIENTS; c++ )
    {
        if ( !rocketnet_message_server_client_connected( walk.server, c ) || walk.object->connections[c].send_budget_enabled != ( budget_seed ? 1 : 0 ) )
        {
            fprintf( stderr, "tla_trace writerfinish: client %d: connected %d (the transport holds %d), send budget %d; the walk needs 1 and %d\n", c,
                     rocketnet_message_server_client_connected( walk.server, c ), rocketnet_server_transport_num_connected_clients( walk.transport ),
                     walk.object->connections[c].send_budget_enabled, budget_seed ? 1 : 0 );
            fault = true;
        }
    }
    const int capacity = wf_cap( &walk );
    if ( capacity < 1 || capacity > WF_FRAMES - WF_CLIENTS )
    {
        fprintf( stderr, "tla_trace writerfinish: an update leaves the writers %d frames\n", capacity );
        fault = true;
    }
    const bool buffered = wf_buffered( &walk );
    if ( buffered != buffered_seed )
    {
        fprintf( stderr, "tla_trace writerfinish: the transport is %s, the seed's kind is the other\n", buffered ? "buffered" : "staged" );
        fault = true;
    }
    {
        std::lock_guard<std::mutex> lock( wf_fake_mutex );
        wf_fake.started = 0;
        wf_fake.finished = 0;
    }
    for ( int c = 0; c < WF_CLIENTS; c++ )
    {
        if ( wf_held( &walk, c ) != 0 || wf_queued( &walk, c ) != 0 )
        {
            fprintf( stderr, "tla_trace writerfinish: client %d holds %d queued and %d held before the first record\n", c, (int) wf_queued( &walk, c ), (int) wf_held( &walk, c ) );
            fault = true;
        }
    }

    wf_writers_t * writers = new wf_writers_t();
    writers->server = walk.server;
    walk.writers = writers;
    std::vector<std::thread> threads;
    for ( int w = 0; w < WF_WRITERS; w++ )
    {
        threads.emplace_back( wf_writer, writers, w );
    }
    {
        std::unique_lock<std::mutex> lock( writers->mutex );
        writers->changed.wait( lock, [&]
                               {
            for ( int w = 0; w < WF_WRITERS; w++ )
            {
                if ( writers->id[w] == 0 )
                {
                    return false;
                }
            }
            return true; } );
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", "<<>>", wf_state( &walk ) ) );

    for ( int step = 0; step < steps && !fault; step++ )
    {
        const char * op = "";
        const char * act = "";
        std::string args = "<<>>";

        const uint64_t overlaps = wf_transport_counter( &walk, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP );
        const uint64_t two_threads = wf_transport_counter( &walk, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS );

        // any client may be called on: one with a start open is finished or
        // aborted, one without is started on, whatever it holds queued
        int open_count = 0;
        for ( int c = 0; c < WF_CLIENTS; c++ )
        {
            open_count += wf_open( &walk, c ) != WF_OPEN_NONE ? 1 : 0;
        }

        const int pick = random.below( 100 );
        const int picked_client = random.below( WF_CLIENTS );
        const int picked_other = random.below( 4 );
        const int seeded = random.below( WF_WRITERS );
        const int kind_pick = random.below( 100 );
        const int bytes_pick = random.below( 100 );
        const int budget_pick = random.below( 100 );
        if ( !walk.fill )
        {
            rocketnet_message_server_update( walk.server, WF_TIME );
            walk.fill = true;
            memset( walk.touched, 0, sizeof( walk.touched ) );
            op = "update";
            act = "BeginFill";
        }
        else if ( open_count == 0 && pick < 25 )
        {
            // what the walk took of what the writers left: before less after
            const uint64_t left = wf_left( &walk );
            rocketnet_message_server_send_packets( walk.server );
            walk.walked += left - wf_left( &walk );
            walk.fill = false;
            op = "send_packets";
            act = "SendPackets";
        }
        else
        {
            // a client; one with a start open is mostly finished, sometimes
            // left open while another client starts
            int client = picked_client;
            if ( wf_open( &walk, client ) != WF_OPEN_NONE && picked_other == 0 )
            {
                for ( int c = 0; c < WF_CLIENTS; c++ )
                {
                    if ( wf_open( &walk, c ) == WF_OPEN_NONE )
                    {
                        client = c;
                    }
                }
            }

            // the client's writer: its owner in this phase, or the seed's pick
            int w = wf_owner( &walk, client );
            if ( w == -1 )
            {
                w = seeded;
            }
            if ( w < 0 )
            {
                fprintf( stderr, "tla_trace writerfinish: client %d is owned by a thread that is no writer\n", client );
                fault = true;
                break;
            }

            std::vector<std::string> call_args;
            call_args.push_back( wf_writer_name( w ) );
            call_args.push_back( tla_int( client ) );
            args = tla_sequence( call_args );
            walk.touched[client][w] = true;

            const int open = wf_open( &walk, client );
            if ( open == WF_OPEN_NONE )
            {
                // the kind: mostly small, some large, some reliable
                const int kind = kind_pick < 70 ? WF_SMALL : ( kind_pick < 85 ? WF_LARGE : WF_RELIABLE );
                const int reliability = kind == WF_RELIABLE ? ROCKETNET_RELIABILITY_RELIABLE : ROCKETNET_RELIABILITY_UNRELIABLE;
                const int bytes = kind == WF_LARGE ? WF_LARGE_BYTES : WF_MESSAGE_BYTES;

                // read before the call: the slots left the writers, and the
                // client's unreliable queue
                const int cap_before = wf_cap( &walk );
                const uint32_t queued_before = wf_queued( &walk, client );

                // with a send budget, a small start may find the bucket paying
                // only a frame of its own (a whole frame: no record header),
                // or not that; the bucket is given back after the start
                rocketnet_send_budget_t * budget = &walk.object->connections[client].budget;
                const double tokens = budget->tokens;
                if ( budget_seed && kind == WF_SMALL && budget_pick < 40 )
                {
                    const int whole = rocketnet_send_budget_max_wire_bytes( WF_MESSAGE_BYTES + 1 );
                    budget->tokens = (double) ( budget_pick < 25 ? whole : whole - 1 );
                }
                wf_call( writers, w, WF_START, client, reliability, bytes );
                if ( budget_seed )
                {
                    budget->tokens = budget->tokens > tokens ? budget->tokens : tokens;
                }

                walk.queued_as[client] = kind == WF_LARGE ? WF_OPEN_LARGE : kind == WF_RELIABLE       ? WF_OPEN_RELIABLE
                                                                        : queued_before > 0           ? WF_OPEN_BEHIND
                                                                        : buffered && cap_before == 0 ? WF_OPEN_RING
                                                                                                      : WF_OPEN_QUEUED;
                op = "start";
                act = kind == WF_LARGE ? "StartLarge" : kind == WF_RELIABLE ? "StartReliable"
                                                                            : "StartSmall";
                if ( !writers->pointer )
                {
                    fprintf( stderr, "tla_trace writerfinish: a start gave no pointer\n" );
                    fault = true;
                }
                else
                {
                    walk.starts++;
                }
            }
            else if ( bytes_pick < 10 )
            {
                wf_call( writers, w, WF_ABORT, client );
                walk.cancelled++;
                op = "abort";
                act = "Abort";
                if ( wf_open( &walk, client ) != WF_OPEN_NONE )
                {
                    fprintf( stderr, "tla_trace writerfinish: an abort left its start open\n" );
                    fault = true;
                }
            }
            else
            {
                // bytes_written: the message's, over one packet on a large
                // start, or outside 1..reserved
                const uint32_t reserved = walk.object->connections[client].pending_bytes;
                const bool bad = bytes_pick < 20;
                const bool over = !bad && open == WF_OPEN_LARGE && bytes_pick < 55;
                const int bytes = bad ? ( bytes_pick < 15 ? 0 : (int) reserved + 1 ) : over ? WF_OVER_BYTES
                                                                                            : WF_MESSAGE_BYTES;

                const uint64_t refused = wf_message_counter( &walk, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY );
                const uint64_t lost = wf_lost_counters( &walk );
                const uint64_t sealed = wf_sealed_now( &walk );
                wf_call( writers, w, WF_FINISH, client, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
                walk.sealed += wf_sealed_now( &walk ) - sealed;
                walk.lost += wf_lost_counters( &walk ) - lost;
                op = "finish";
                act = bad ? "FinishBadBytes" : open == WF_OPEN_LARGE ? ( over ? "FinishOver" : "FinishRestart" )
                                                                     : "Finish";
                const uint64_t moved = wf_message_counter( &walk, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY ) - refused;
                if ( bad )
                {
                    walk.cancelled++;
                    if ( writers->serial != 0 || moved != 0 )
                    {
                        fprintf( stderr, "tla_trace writerfinish: a finish with bytes_written %d returned %u and the refusal's counter moved by %d\n", bytes,
                                 (unsigned) writers->serial, (int) moved );
                        fault = true;
                    }
                }
                else if ( moved != ( writers->serial == 0 ? 1u : 0u ) )
                {
                    fprintf( stderr, "tla_trace writerfinish: a finish returned %u and the refusal's counter moved by %d\n", (unsigned) writers->serial, (int) moved );
                    fault = true;
                }
                if ( wf_open( &walk, client ) != WF_OPEN_NONE )
                {
                    fprintf( stderr, "tla_trace writerfinish: a finish left its start open\n" );
                    fault = true;
                }
            }
        }

        if ( wf_transport_counter( &walk, ROCKETNET_COUNTER_MAIN_THREAD_CALL_OVERLAP ) != overlaps ||
             wf_transport_counter( &walk, ROCKETNET_COUNTER_CLIENT_SEND_TWO_THREADS ) != two_threads )
        {
            fprintf( stderr, "tla_trace writerfinish: the thread check refused a call of the walk\n" );
            fault = true;
        }

        report.act( act );
        records.push_back( tla_step( op, act, args, wf_state( &walk ) ) );

        // the planted effect, where it shows: on a staged transport a
        // writer's message queued for send_packets; on a buffered one a
        // start open as having no capacity
        for ( int c = 0; c < WF_CLIENTS; c++ )
        {
            if ( buffered ? wf_open( &walk, c ) == WF_OPEN_NOCAP : wf_queued( &walk, c ) > 0 )
            {
                report.planted( (int) records.size() );
                break;
            }
        }
    }

    report.print();

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    Writers = {\"w1\", \"w2\"}\n"
              "    Clients = {0, 1}\n"
              "    NoOwner = \"none\"\n"
              "    Capacity = %d\n"
              "    MaxStarts = %d\n"
              "    SealAtFinish = TRUE\n"
              "    RefuseNoCapacity = TRUE\n"
              "    CheckOwner = TRUE\n"
              "    Buffered = %s\n"
              "    RingRefuses = FALSE\n"
              "    Budgeted = %s\n"
              "    RestartAtFinish = TRUE\n"
              "    ValidateFirst = TRUE\n",
              capacity, steps, buffered ? "TRUE" : "FALSE", budget_seed ? "TRUE" : "FALSE" );
    const bool written = !fault && tla_write_trace( out, "MCTraceWriterFinish", seed, "TraceRNWriterFinish", "", constants, records );

    // the starts still open are aborted by their owners, then the writers end
    for ( int c = 0; c < WF_CLIENTS; c++ )
    {
        const int w = wf_owner( &walk, c );
        if ( wf_open( &walk, c ) != 0 && w >= 0 )
        {
            wf_call( writers, w, WF_ABORT, c );
        }
    }
    for ( int w = 0; w < WF_WRITERS; w++ )
    {
        wf_call( writers, w, WF_QUIT, 0 );
    }
    for ( size_t i = 0; i < threads.size(); i++ )
    {
        threads[i].join();
    }
    delete writers;

    rocketnet_message_server_send_packets( walk.server );
    rocketnet_server_transport_swap_io_for_test( walk.transport, &operations, &instance );
    rocketnet_server_transport_stop( walk.transport );
    const bool stopped = rocketnet_server_transport_stopped( walk.transport );
    rocketnet_message_server_destroy( walk.server );
    const bool hub_gone = rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK;
    rocketnet_term();

    if ( !stopped || !hub_gone )
    {
        fprintf( stderr, "tla_trace writerfinish: the server or its hub did not stop\n" );
    }
    return written && stopped && hub_gone ? 0 : 1;
}

TLA_TRACE_LAYER( "writerfinish", trace_writer_finish );
