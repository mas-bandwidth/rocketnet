/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client's I/O core (sdk/rocketnet_client_io.h) with a real I/O thread:
// rocketnet_client_io_thread_function steps the core on its own thread while
// this (the caller's) thread hands send rings off and exposes and returns
// receive buffers, the two handoffs of proofs/tla/RNBufferHandoff.tla. The
// transport is an in-memory fake whose wait blocks on a condition variable
// until the core's wake, so every exchange is a handshake: the caller waits
// for the I/O thread to drain what it pushed and to send what it handed off,
// never for a time. Every wait is bounded (a stalled handshake fails the case).
//
// The I/O thread checks every datagram it is given to send, in order: its
// bytes, the destination its ring was committed for, and that no aborted slot
// leaves. The caller checks every record it is exposed, in order: bytes, data,
// source and the raw verdict (no session keys here). The nightly TSan job
// (.github/workflows/nightly.yml: gcc, Debug, -fsanitize=thread -O1) runs this
// case in ./bin/functional, where a missing hand-off synchronisation in the
// core is a reported race even when the checks above still pass.
//
// Three transport tables, the three send paths of rocketnet_client_io_send_ring
// and both receive paths of rocketnet_client_io_step: a progress batch with a
// batch receive (the portable and gso clients), a batch without progress (more
// than ROCKETNET_CLIENT_IO_BATCH live slots in one ring flush mid-ring), and
// per-datagram send and receive. Each case ends with the stop; with the
// progress table the I/O thread still holds a half-sent ring when it sees
// quit (proofs/tla/RNClientIoShutdown.tla Start). The stop runs on a thread of
// its own with its wake call held, and the I/O thread's exit must not close
// the transport until that call returns (WorkerQuiesce). No socket; the only
// clock reads are the handshake bounds (the wait for the exit's discard
// count among them), the core's idle guard, and the 100 ms in which the
// exit must not close.

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_platform.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <new>
#include <thread>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../check_count.h"

// defined in tests/functional/rocketnet_functional_tests.cpp
void rocketnet_functional_log_ring_dump();

static void threaded_check_handler( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_functional_log_ring_dump();
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define threaded_check( condition )                                                               \
    do                                                                                            \
    {                                                                                             \
        ROCKETNET_TEST_COUNT_CHECK();                                                             \
        if ( !( condition ) )                                                                     \
        {                                                                                         \
            threaded_check_handler( #condition, (const char *) __FUNCTION__, __FILE__, __LINE__ ); \
        }                                                                                         \
    } while ( 0 )

#define THREADED_SLOTS 80                   // past ROCKETNET_CLIENT_IO_BATCH, so one ring can flush mid-ring
#define THREADED_QUEUE 2048                 // inbound datagrams the fake holds
#define THREADED_DATA_BYTES 320             // stored bytes of one inbound datagram
#define THREADED_ROUNDS 2000
#define THREADED_HANDSHAKE_SECONDS 20       // a bound, never a cadence: a handshake past it fails the case
#define THREADED_OVERSIZE_BYTES 70000       // past ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES

struct threaded_datagram_t
{
    rocketnet_address_t address;
    int bytes;
    uint8_t data[THREADED_DATA_BYTES];
};

// The fake's state is under its own lock: the caller pushes inbound datagrams
// and reads the counters, the I/O thread pops, sends and waits.

struct threaded_fake_t
{
    std::mutex lock;
    std::condition_variable cond;
    threaded_datagram_t inbound[THREADED_QUEUE];
    int inbound_head;
    int inbound_count;
    bool wake;
    uint64_t wait_entries;          // waits the I/O thread entered: it had nothing to do
    uint64_t sent;                  // live slots sent, each checked in order
    uint64_t send_errors;
    uint64_t send_calls;            // send, send_batch or send_batch_progress calls
    uint64_t partial_sends;         // progress calls that consumed only a prefix
    uint64_t batch_calls;           // send_batch calls (with the progress table: terminal sends only)
    // the held ring at the stop: armed, the progress call that sends half of
    // the ring sets holding; the next one sets blocked and blocks until
    // released, then sends one slot, so the I/O thread still holds the ring
    // when it sees quit
    bool hold_armed;
    bool holding;
    bool blocked;
    bool released;
    // the race-sensitive hold: released by a relaxed store, and the sends
    // read back by a relaxed load, so neither side's handshake orders the
    // caller's hand-off for a race detector
    bool hold_relaxed;
    std::atomic<bool> released_relaxed;
    std::atomic<uint64_t> sent_relaxed;
    uint64_t hold_timeouts;
    // the held wake at the stop: armed, the next wake sets the wake, marks
    // itself in flight and blocks until released, so stop's wake call is
    // still to return while the I/O thread runs its exit. The close must
    // wait for that call (proofs/tla/RNClientIoShutdown.tla WorkerQuiesce)
    bool wake_hold_armed;
    bool wake_in_flight;
    bool wake_released;
    uint64_t close_calls;
    bool close_saw_wake_in_flight;
};

// ---- the payloads ----------------------------------------------------------

// live send slot g (counted over the whole case): its byte count, then g and
// its ring's destination port in the first ten bytes, then a pattern

static int threaded_send_bytes( uint64_t g )
{
    return 12 + (int) ( ( g * 37 ) % 200 );
}

static void threaded_send_fill( uint8_t * p, uint64_t g, uint16_t port )
{
    const int bytes = threaded_send_bytes( g );
    memcpy( p, &g, 8 );
    memcpy( p + 8, &port, 2 );
    for ( int i = 10; i < bytes; i++ )
    {
        p[i] = (uint8_t) ( g * 7 + (uint64_t) i );
    }
}

static uint16_t threaded_round_port( int round )
{
    return (uint16_t) ( 40000 + round % 50 );
}

// inbound datagram h: its byte count, h in the first eight bytes, a pattern,
// and a source that cycles over three ports

static int threaded_receive_bytes( uint64_t h )
{
    return 8 + (int) ( ( h * 13 ) % 300 );
}

static void threaded_receive_from( uint64_t h, rocketnet_address_t * from )
{
    rocketnet_address_load_ipv4( from, 0x7f000001, (uint16_t) ( 50000 + h % 3 ) );
}

// ---- the fake transport (both threads) -------------------------------------

// called on the I/O thread with the fake's lock held: checks the next live
// slot is g == sent, its bytes and its destination port, and counts it

static void threaded_take_send( threaded_fake_t * fake, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    uint64_t g = 0;
    uint16_t port = 0;
    bool ok = bytes >= 10;
    if ( ok )
    {
        memcpy( &g, data, 8 );
        memcpy( &port, data + 8, 2 );
        ok = g == fake->sent && bytes == threaded_send_bytes( g );
    }

    if ( ok )
    {
        rocketnet_address_t expected = {};
        rocketnet_address_load_ipv4( &expected, 0x7f000001, port );
        ok = rocketnet_address_equal( to, &expected );
    }

    for ( int i = 10; ok && i < bytes; i++ )
    {
        ok = data[i] == (uint8_t) ( g * 7 + (uint64_t) i );
    }

    if ( !ok )
    {
        if ( fake->send_errors == 0 )
        {
            printf( "client io threaded: send %llu wrong (got g %llu, %d bytes, port %u)\n",
                    (unsigned long long) fake->sent, (unsigned long long) g, bytes, (unsigned) port );
        }
        fake->send_errors++;
    }

    fake->sent++;
    fake->sent_relaxed.store( fake->sent, std::memory_order_relaxed );
}

static void * threaded_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    return context;
}

static void threaded_destroy( void * transport )
{
    (void) transport;
}

static void threaded_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::lock_guard<std::mutex> guard( fake->lock );
    fake->send_calls++;
    threaded_take_send( fake, to, data, bytes );
    fake->cond.notify_all();
}

static void threaded_send_batch( void * transport, const rocketnet_platform_send_t * sends, int num_packets )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::lock_guard<std::mutex> guard( fake->lock );
    fake->send_calls++;
    fake->batch_calls++;
    for ( int i = 0; i < num_packets; i++ )
    {
        threaded_take_send( fake, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
    }
    fake->cond.notify_all();
}

// every fifth call consumes only the first half (at least one), as a kernel
// that refused the rest for now; the core retries the suffix

static int threaded_send_batch_progress( void * transport, const rocketnet_platform_send_t * sends, int num_packets )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::unique_lock<std::mutex> guard( fake->lock );
    fake->send_calls++;
    int consume = num_packets;
    if ( fake->hold_armed && num_packets > 1 )
    {
        if ( !fake->holding )
        {
            consume = num_packets / 2;
            fake->holding = true;
        }
        else
        {
            fake->blocked = true;
            fake->cond.notify_all();
            if ( fake->hold_relaxed )
            {
                // released by a relaxed store, waited for without the lock:
                // nothing here orders the caller's hand-off before this
                // thread's next take but the core's own mutex
                guard.unlock();
                const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds( THREADED_HANDSHAKE_SECONDS );
                bool released = fake->released_relaxed.load( std::memory_order_relaxed );
                while ( !released && std::chrono::steady_clock::now() < until )
                {
                    std::this_thread::yield();
                    released = fake->released_relaxed.load( std::memory_order_relaxed );
                }
                guard.lock();
                if ( !released )
                {
                    fake->hold_timeouts++;
                }
            }
            else if ( !fake->cond.wait_for( guard, std::chrono::seconds( THREADED_HANDSHAKE_SECONDS ), [fake]
                                            { return fake->released; } ) )
            {
                fake->hold_timeouts++;
            }
            fake->hold_armed = false;
            consume = 1;
        }
    }
    else if ( fake->send_calls % 5 == 0 && num_packets > 1 )
    {
        consume = num_packets / 2;
        fake->partial_sends++;
    }
    for ( int i = 0; i < consume; i++ )
    {
        threaded_take_send( fake, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
    }
    fake->cond.notify_all();
    return consume;
}

static int threaded_copy_bytes( const threaded_datagram_t * dg, int room )
{
    int n = dg->bytes < room ? dg->bytes : room;
    return n < (int) sizeof( dg->data ) ? n : (int) sizeof( dg->data );
}

static int threaded_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::lock_guard<std::mutex> guard( fake->lock );
    if ( fake->inbound_count == 0 )
    {
        return 0;
    }

    const threaded_datagram_t * dg = &fake->inbound[fake->inbound_head];
    fake->inbound_head = ( fake->inbound_head + 1 ) % THREADED_QUEUE;
    fake->inbound_count--;
    *from = dg->address;
    memcpy( data, dg->data, (size_t) threaded_copy_bytes( dg, capacity ) );
    return dg->bytes;
}

static int threaded_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::lock_guard<std::mutex> guard( fake->lock );
    int got = 0;
    while ( got < max_packets && fake->inbound_count > 0 )
    {
        const threaded_datagram_t * dg = &fake->inbound[fake->inbound_head];
        fake->inbound_head = ( fake->inbound_head + 1 ) % THREADED_QUEUE;
        fake->inbound_count--;
        from[got] = dg->address;
        memcpy( packet_data + (size_t) got * (size_t) packet_stride, dg->data, (size_t) threaded_copy_bytes( dg, packet_stride ) );
        packet_bytes[got] = dg->bytes;
        got++;
    }
    return got;
}

// the core's wait: counts the entry for the caller's handshake, then blocks
// until the wake, or a readable datagram when the core has room, or the
// core's own guard timeout

static bool threaded_wait( void * transport, bool readable, double timeout_seconds )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::unique_lock<std::mutex> guard( fake->lock );
    fake->wait_entries++;
    fake->cond.notify_all();
    const bool ready = fake->cond.wait_for( guard, std::chrono::duration<double>( timeout_seconds ),
                                            [fake, readable]
                                            { return fake->wake || ( readable && fake->inbound_count > 0 ); } );
    fake->wake = false;
    return ready;
}

static void threaded_wake( void * transport )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::unique_lock<std::mutex> guard( fake->lock );
    fake->wake = true;
    fake->cond.notify_all();
    if ( fake->wake_hold_armed )
    {
        fake->wake_hold_armed = false;
        fake->wake_in_flight = true;
        fake->cond.notify_all();
        if ( !fake->cond.wait_for( guard, std::chrono::seconds( THREADED_HANDSHAKE_SECONDS ), [fake]
                                   { return fake->wake_released; } ) )
        {
            fake->hold_timeouts++;
        }
        fake->wake_in_flight = false;
        fake->cond.notify_all();
    }
}

// the I/O thread's exit closes the handles once; a close while a wake call
// is still to return is the write to a closed handle the exit must not allow
static void threaded_close( void * transport )
{
    threaded_fake_t * fake = (threaded_fake_t *) transport;
    std::lock_guard<std::mutex> guard( fake->lock );
    fake->close_calls++;
    if ( fake->wake_in_flight )
    {
        fake->close_saw_wake_in_flight = true;
    }
    fake->cond.notify_all();
}

static const rocketnet_client_transport_io_operations_t threaded_operations_progress = {
    threaded_create, threaded_destroy, threaded_send, threaded_receive,
    threaded_send_batch, threaded_receive_batch, threaded_wait, threaded_wake,
    NULL, NULL, threaded_close, threaded_send_batch_progress
};

static const rocketnet_client_transport_io_operations_t threaded_operations_batch = {
    threaded_create, threaded_destroy, threaded_send, threaded_receive,
    threaded_send_batch, threaded_receive_batch, threaded_wait, threaded_wake,
    NULL, NULL, threaded_close, NULL
};

static const rocketnet_client_transport_io_operations_t threaded_operations_single = {
    threaded_create, threaded_destroy, threaded_send, threaded_receive,
    NULL, NULL, threaded_wait, threaded_wake,
    NULL, NULL, threaded_close, NULL
};

// ---- the caller's side -----------------------------------------------------

struct threaded_rig_t
{
    uint8_t ring[ROCKETNET_CLIENT_IO_NUM_RINGS][THREADED_SLOTS * ROCKETNET_FRAME_STRIDE];
    int counts[ROCKETNET_CLIENT_IO_NUM_RINGS][THREADED_SLOTS];
    rocketnet_client_io_ring_view_t view[ROCKETNET_CLIENT_IO_NUM_RINGS];
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    rocketnet_platform_mutex_t mutex;
    rocketnet_client_io_t io;
    threaded_fake_t fake;
    rocketnet_client_transport_io_t transport;
    uint64_t committed_live;        // live slots committed so far (the next g)
    uint64_t pushed;                // inbound datagrams pushed, oversize ones included (the next h)
    uint64_t pushed_oversize;
    uint64_t received;              // records exposed and checked
    uint64_t expected_received;     // pushed datagrams within the size limit
};

static double threaded_time( void * context )
{
    (void) context;
    return 1000.0;
}

// waits under the fake's lock until done() holds; a wait past the bound fails
// the case (a lost wake or a stalled handshake)

template <typename predicate_t> static void threaded_handshake( threaded_rig_t * rig, predicate_t done, const char * what )
{
    std::unique_lock<std::mutex> guard( rig->fake.lock );
    if ( !rig->fake.cond.wait_for( guard, std::chrono::seconds( THREADED_HANDSHAKE_SECONDS ), done ) )
    {
        printf( "client io threaded: handshake '%s' stalled (sent %llu of %llu, inbound %d, waits %llu)\n", what,
                (unsigned long long) rig->fake.sent, (unsigned long long) rig->committed_live, rig->fake.inbound_count,
                (unsigned long long) rig->fake.wait_entries );
        threaded_check( false );
    }
}

// fills `slots` slots of the Filling ring, every eleventh one aborted (byte
// count 0), for the round's port; returns them for the hand-off or the stop

static uint64_t threaded_fill_ring( threaded_rig_t * rig, int slots, int round )
{
    const int ring = rig->io.send_fill;
    const uint16_t port = threaded_round_port( round );
    for ( int s = 0; s < slots; s++ )
    {
        uint8_t * p = rig->ring[ring] + (size_t) s * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET;
        if ( ( round + s ) % 11 == 10 )
        {
            rig->counts[ring][s] = 0;
            memset( p, 0xEE, 16 );
            continue;
        }
        const uint64_t g = rig->committed_live++;
        threaded_send_fill( p, g, port );
        rig->counts[ring][s] = threaded_send_bytes( g );
    }
    return (uint64_t) slots;
}

// hands the Filling ring off; a LATE result (every ring in flight, or the I/O
// thread holding the mutex this instant) waits for the I/O thread to send what
// it holds, then tries again with the same slots

static void threaded_hand_off( threaded_rig_t * rig, uint64_t slots, int round )
{
    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, threaded_round_port( round ) );

    for ( int attempt = 0;; attempt++ )
    {
        threaded_check( attempt < 100000 );
        const int result = rocketnet_client_io_hand_off( &rig->io, slots, &to );
        if ( result == ROCKETNET_CLIENT_RINGS_HANDED_OFF )
        {
            return;
        }
        threaded_check( result == ROCKETNET_CLIENT_RINGS_LATE );

        uint64_t sent_before = 0;
        uint64_t waits_before = 0;
        {
            std::lock_guard<std::mutex> guard( rig->fake.lock );
            sent_before = rig->fake.sent;
            waits_before = rig->fake.wait_entries;
        }
        threaded_handshake( rig, [rig, sent_before, waits_before]
                            { return rig->fake.sent > sent_before || rig->fake.wait_entries > waits_before; }, "late hand-off" );
    }
}

static void threaded_push( threaded_rig_t * rig, int count, bool oversize_every_97th )
{
    std::lock_guard<std::mutex> guard( rig->fake.lock );
    threaded_check( rig->fake.inbound_count + count <= THREADED_QUEUE );
    for ( int i = 0; i < count; i++ )
    {
        const uint64_t h = rig->pushed++;
        threaded_datagram_t * dg = &rig->fake.inbound[( rig->fake.inbound_head + rig->fake.inbound_count ) % THREADED_QUEUE];
        threaded_receive_from( h, &dg->address );
        if ( oversize_every_97th && h % 97 == 96 )
        {
            dg->bytes = THREADED_OVERSIZE_BYTES;
            memset( dg->data, 0xAB, sizeof( dg->data ) );
            rig->pushed_oversize++;
        }
        else
        {
            const int bytes = threaded_receive_bytes( h );
            dg->bytes = bytes;
            memcpy( dg->data, &h, 8 );
            for ( int b = 8; b < bytes; b++ )
            {
                dg->data[b] = (uint8_t) ( h * 3 + (uint64_t) b );
            }
            rig->expected_received++;
        }
        rig->fake.inbound_count++;
    }
    rig->fake.cond.notify_all();
}

// the next h a record should carry: datagrams within the size limit, in order,
// skipping the oversize ones the I/O thread refused

static uint64_t threaded_next_h( uint64_t received, bool oversize_every_97th )
{
    if ( !oversize_every_97th )
    {
        return received;
    }
    // every 97th pushed datagram (h % 97 == 96) is refused: received r is h = r + r / 96
    return received + received / 96;
}

static void threaded_check_buffer( threaded_rig_t * rig, const rocketnet_client_receive_buffer_t * buffer, bool oversize_every_97th )
{
    threaded_check( buffer->num_packets > 0 );
    threaded_check( buffer->num_packets <= ROCKETNET_NUM_CLIENT_PACKETS );
    for ( int i = 0; i < buffer->num_packets; i++ )
    {
        const uint64_t h = threaded_next_h( rig->received, oversize_every_97th );
        const int bytes = threaded_receive_bytes( h );
        threaded_check( buffer->packet_bytes[i] == (size_t) bytes );
        threaded_check( buffer->verdict[i] == ROCKETNET_CLIENT_IO_VERDICT_RAW );
        threaded_check( buffer->receive_time[i] == 1000.0 );
        rocketnet_address_t from = {};
        threaded_receive_from( h, &from );
        threaded_check( rocketnet_address_equal( &buffer->from[i], &from ) );
        uint64_t got = 0;
        memcpy( &got, buffer->packet_data[i], 8 );
        threaded_check( got == h );
        for ( int b = 8; b < bytes; b++ )
        {
            threaded_check( buffer->packet_data[i][b] == (uint8_t) ( h * 3 + (uint64_t) b ) );
        }
        rig->received++;
    }
}

// exposes, checks and returns buffers until every pushed datagram within the
// size limit has been seen. A NULL expose (nothing ready, or the I/O thread
// holding the mutex) waits for the I/O thread's next wait entry, which comes
// only after the drain and the publish (or owed publish) of its step. It
// never waits for the inbound queue to empty: with a buffer ready and the
// next one full, the I/O thread waits on the wake alone (FullWaitsOnWake)
// until this thread's exchange takes the ready one.

static void threaded_receive_all( threaded_rig_t * rig, bool oversize_every_97th )
{
    while ( rig->received < rig->expected_received )
    {
        uint64_t waits_before = 0;
        {
            std::lock_guard<std::mutex> guard( rig->fake.lock );
            waits_before = rig->fake.wait_entries;
        }

        rocketnet_client_receive_buffer_t * buffer = rocketnet_client_io_expose( &rig->io );
        if ( buffer )
        {
            threaded_check_buffer( rig, buffer, oversize_every_97th );
            rocketnet_client_io_return( &rig->io );
            continue;
        }

        threaded_handshake( rig, [rig, waits_before]
                            { return rig->fake.wait_entries > waits_before; }, "receive" );
    }

    threaded_check( rig->received == rig->expected_received );
}

static void threaded_run( const rocketnet_client_transport_io_operations_t * operations, const char * name, bool flush_mid_ring )
{
    threaded_rig_t * rig = new threaded_rig_t();
    memset( rig->ring, 0, sizeof( rig->ring ) );
    memset( rig->counts, 0, sizeof( rig->counts ) );
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        rig->view[i].data = rig->ring[i];
        rig->view[i].frame_stride = ROCKETNET_FRAME_STRIDE;
        rig->view[i].wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
        rig->view[i].wire_bytes = (const uint8_t *) rig->counts[i];
        rig->view[i].wire_bytes_stride = sizeof( int );
    }
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
    {
        rig->buffers[i] = new rocketnet_client_receive_buffer_t;
    }
    threaded_check( rocketnet_platform_mutex_create( &rig->mutex ) );

    rig->fake.inbound_head = 0;
    rig->fake.inbound_count = 0;
    rig->fake.wake = false;
    rig->fake.wait_entries = 0;
    rig->fake.sent = 0;
    rig->fake.send_errors = 0;
    rig->fake.send_calls = 0;
    rig->fake.partial_sends = 0;
    rig->fake.batch_calls = 0;
    rig->fake.hold_armed = false;
    rig->fake.holding = false;
    rig->fake.blocked = false;
    rig->fake.released = false;
    rig->fake.hold_relaxed = false;
    rig->fake.released_relaxed.store( false, std::memory_order_relaxed );
    rig->fake.sent_relaxed.store( 0, std::memory_order_relaxed );
    rig->fake.hold_timeouts = 0;
    rig->fake.wake_hold_armed = false;
    rig->fake.wake_in_flight = false;
    rig->fake.wake_released = false;
    rig->fake.close_calls = 0;
    rig->fake.close_saw_wake_in_flight = false;
    rig->committed_live = 0;
    rig->pushed = 0;
    rig->pushed_oversize = 0;
    rig->received = 0;
    rig->expected_received = 0;

    rig->transport.operations = operations;
    rig->transport.instance = &rig->fake;
    rocketnet_client_io_init( &rig->io, &rig->transport, &rig->mutex, rig->view, rig->buffers, threaded_time, NULL );

    // the exit stores the discard count just before it waits for stop's
    // wake call to return; a value no exit can store marks that it has not
    rig->io.receive_discarded_at_stop.store( UINT64_MAX, std::memory_order_relaxed );

    const bool oversize = true;

    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, rocketnet_client_io_thread_function, &rig->io );
    threaded_check( thread != NULL );

    for ( int round = 0; round < THREADED_ROUNDS; round++ )
    {
        // a ring of 1 to THREADED_SLOTS slots (past the batch on every
        // seventh round when flushing mid-ring), handed off; inbound
        // datagrams pushed and received while the I/O thread sends it

        const int slots = flush_mid_ring && round % 7 == 3 ? THREADED_SLOTS : 1 + ( round * 13 ) % 24;
        const uint64_t filled = threaded_fill_ring( rig, slots, round );
        threaded_hand_off( rig, filled, round );

        threaded_push( rig, 1 + ( round * 29 ) % 48, oversize );
        threaded_receive_all( rig, oversize );

        // every 50th round, more datagrams than one buffer holds: the I/O
        // thread fills a buffer, publishes it, and fills the next one while
        // the caller is between exposes

        if ( round % 50 == 49 )
        {
            threaded_push( rig, ROCKETNET_NUM_CLIENT_PACKETS + 200, oversize );
            threaded_receive_all( rig, oversize );
        }

        const uint64_t target = rig->committed_live;
        threaded_handshake( rig, [rig, target]
                            { return rig->fake.sent >= target; }, "send" );
    }

    // With the progress table, a hand-off no handshake orders: ring D half
    // sent and the I/O thread blocked in its next send, ring E handed off,
    // then a relaxed release, and the caller reads the sends back relaxed.
    // Between E's hand-off and the I/O thread's take of E only the core's
    // own mutex orders the two threads, so a hand-off that skipped it is a
    // race TSan reports on every run, whatever the interleaving.

    if ( operations == &threaded_operations_progress )
    {
        {
            std::lock_guard<std::mutex> guard( rig->fake.lock );
            rig->fake.hold_armed = true;
            rig->fake.hold_relaxed = true;
        }
        const uint64_t ring_d = threaded_fill_ring( rig, 40, THREADED_ROUNDS + 3 );
        threaded_hand_off( rig, ring_d, THREADED_ROUNDS + 3 );
        threaded_handshake( rig, [rig]
                            { return rig->fake.holding && rig->fake.blocked; }, "ring D held" );

        rocketnet_address_t to_e = {};
        rocketnet_address_load_ipv4( &to_e, 0x7f000001, threaded_round_port( THREADED_ROUNDS + 4 ) );
        const uint64_t ring_e = threaded_fill_ring( rig, 30, THREADED_ROUNDS + 4 );
        threaded_check( rocketnet_client_io_hand_off( &rig->io, ring_e, &to_e ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );

        const uint64_t target = rig->committed_live;
        rig->fake.released_relaxed.store( true, std::memory_order_relaxed );
        const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds( THREADED_HANDSHAKE_SECONDS );
        while ( rig->fake.sent_relaxed.load( std::memory_order_relaxed ) < target && std::chrono::steady_clock::now() < until )
        {
            std::this_thread::yield();
        }
        threaded_check( rig->fake.sent_relaxed.load( std::memory_order_relaxed ) >= target );

        std::lock_guard<std::mutex> guard( rig->fake.lock );
        threaded_check( !rig->fake.hold_armed );
        threaded_check( rig->fake.hold_timeouts == 0 );
        rig->fake.hold_relaxed = false;
        rig->fake.holding = false;
        rig->fake.blocked = false;
    }

    // the stop (proofs/tla/RNClientIoShutdown.tla Start): with the progress
    // table, ring A half sent and still in the I/O thread's hand, its next
    // send blocked; ring B handed off behind it; then stop hands the Filling
    // ring C over with quit. Released, the blocked send sends one more slot
    // of A, and the next step sees quit: the rest of A, then B, then C, each
    // whole in one terminal batch, and the thread exits. The other tables
    // stop with B in flight and C final.

    const bool hold = operations == &threaded_operations_progress;
    if ( hold )
    {
        {
            std::lock_guard<std::mutex> guard( rig->fake.lock );
            rig->fake.hold_armed = true;
        }
        const uint64_t ring_a = threaded_fill_ring( rig, 40, THREADED_ROUNDS );
        threaded_hand_off( rig, ring_a, THREADED_ROUNDS );
        threaded_handshake( rig, [rig]
                            { return rig->fake.holding && rig->fake.blocked; }, "ring held" );

        uint64_t batch_calls = 0;
        {
            std::lock_guard<std::mutex> guard( rig->fake.lock );
            batch_calls = rig->fake.batch_calls;
        }
        threaded_check( batch_calls == 0 );

        // the I/O thread is inside its send, holding no mutex: this hand-off
        // cannot be late

        rocketnet_address_t to_b = {};
        rocketnet_address_load_ipv4( &to_b, 0x7f000001, threaded_round_port( THREADED_ROUNDS + 1 ) );
        const uint64_t ring_b = threaded_fill_ring( rig, 30, THREADED_ROUNDS + 1 );
        threaded_check( rocketnet_client_io_hand_off( &rig->io, ring_b, &to_b ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );
    }
    else
    {
        const uint64_t ring_b = threaded_fill_ring( rig, 30, THREADED_ROUNDS + 1 );
        threaded_hand_off( rig, ring_b, THREADED_ROUNDS + 1 );
    }

    const uint64_t final_slots = threaded_fill_ring( rig, 9, THREADED_ROUNDS + 2 );
    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, threaded_round_port( THREADED_ROUNDS + 2 ) );

    // stop runs on its own thread with its wake call held, so the I/O thread
    // reaches its exit while that call is still to return
    // (proofs/tla/RNClientIoShutdown.tla WorkerQuiesce): the exit reads what
    // is queued, stores the discard count, and then must wait. A close here
    // would be a write to a closed, possibly reused, handle by the wake

    {
        std::lock_guard<std::mutex> guard( rig->fake.lock );
        rig->fake.wake_hold_armed = true;
    }
    std::thread stop_thread( [rig, final_slots, &to]
                             { rocketnet_client_io_stop( &rig->io, final_slots, &to ); } );
    threaded_handshake( rig, [rig]
                        { return rig->fake.wake_in_flight; }, "stop wake held" );

    if ( hold )
    {
        std::lock_guard<std::mutex> guard( rig->fake.lock );
        rig->fake.released = true;
        rig->fake.cond.notify_all();
    }

    {
        const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds( THREADED_HANDSHAKE_SECONDS );
        while ( rig->io.receive_discarded_at_stop.load( std::memory_order_relaxed ) == UINT64_MAX && std::chrono::steady_clock::now() < until )
        {
            std::this_thread::yield();
        }
        threaded_check( rig->io.receive_discarded_at_stop.load( std::memory_order_relaxed ) != UINT64_MAX );
    }

    // the exit is at its wait: give it 100 ms to close, which it must not
    {
        std::unique_lock<std::mutex> guard( rig->fake.lock );
        threaded_check( !rig->fake.cond.wait_for( guard, std::chrono::milliseconds( 100 ), [rig]
                                                  { return rig->fake.close_calls > 0; } ) );
        threaded_check( rig->fake.close_calls == 0 );
        rig->fake.wake_released = true;
        rig->fake.cond.notify_all();
    }
    stop_thread.join();

    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );

    if ( hold )
    {
        threaded_check( rig->fake.hold_timeouts == 0 );
        threaded_check( !rig->fake.hold_armed );
        threaded_check( rig->fake.batch_calls == 3 );
    }

    threaded_check( rig->fake.close_calls == 1 );
    threaded_check( !rig->fake.close_saw_wake_in_flight );
    threaded_check( !rig->fake.wake_in_flight );
    threaded_check( rig->fake.hold_timeouts == 0 );
    threaded_check( rig->io.stopped.load( std::memory_order_acquire ) );
    threaded_check( rocketnet_client_io_step( &rig->io, false ) == ROCKETNET_CLIENT_IO_EXITED );
    threaded_check( rig->fake.send_errors == 0 );
    threaded_check( rig->fake.sent == rig->committed_live );
    threaded_check( rig->received == rig->expected_received );
    threaded_check( rig->io.receive_too_large.load( std::memory_order_relaxed ) == rig->pushed_oversize );
    threaded_check( rig->io.transport_faults.load( std::memory_order_relaxed ) == rig->pushed_oversize );
    if ( operations == &threaded_operations_progress )
    {
        threaded_check( rig->fake.partial_sends > 0 );
    }
    if ( oversize )
    {
        threaded_check( rig->pushed_oversize > 0 );
    }

    printf( "client io threaded %s: %d rounds, %llu slots sent, %llu datagrams received, %llu oversize refused, %llu partial sends\n", name,
            THREADED_ROUNDS, (unsigned long long) rig->fake.sent, (unsigned long long) rig->received,
            (unsigned long long) rig->pushed_oversize, (unsigned long long) rig->fake.partial_sends );

    rocketnet_platform_mutex_destroy( &rig->mutex );
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
    {
        delete rig->buffers[i];
    }
    delete rig;
}

void rocketnet_test_client_io_threaded_progress_batch()
{
    threaded_run( &threaded_operations_progress, "progress batch", false );
}

void rocketnet_test_client_io_threaded_batch_flushes_mid_ring()
{
    threaded_run( &threaded_operations_batch, "batch", true );
}

void rocketnet_test_client_io_threaded_single_datagrams()
{
    threaded_run( &threaded_operations_single, "single", false );
}
