/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check's walk of the io_uring per-queue receive
// (sdk/rocketnet_uring_receive_queue.cpp), checked by
// proofs/tla/TraceRNUringQueueReceiveLayout.tla
// (proofs/tla/README-TraceCheck.md). A seeded random walk plays the network,
// the kernel and main around the code: receives of one or two datagrams
// (UDP_GRO), each accepted or refused by the prefilter, arrive at a fake
// socket; the fake kernel takes buffers from the queue's real buffer ring
// (rocketnet_uring_receive_queue_test_take) and fills them as a multishot
// receive does; the worker's calls are the code's own (needs_arm, arm, cqe,
// stop) and main's are drain and the end of its update. One record per step
// (tla_step): the call, what it gave back, the action its own result says
// it took, and every model variable after it: the bank's indexes, slots and
// counters and the buffer ring's tail and entries read from the code's
// memory, the environment's choices from the walk. The kernel's take is
// KernelReceive(bid) with the buffer test_take returned; a completion is
// Complete(n) with the datagrams cqe returned it banked; a drain is Drain(n)
// with the count drain returned; an arm is Arm when arm returned true. No
// socket, no clock, no thread.
//
// The plant binary's code is the queue with
// ROCKETNET_URING_RECEIVE_QUEUE_PLANT (the drain releases the slots it hands
// main). The walk reports (tla_report_t) the planted effect where it shows:
// a drain that took datagrams and moved head past the drain before's end,
// which the clean code never does.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_URING_RECEIVE_QUEUE_PLANT 1
#include "../sdk/rocketnet_uring_receive_queue.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_uring_receive_queue.h"

#include "tla_trace_record.h"

#if defined( __linux__ ) && ROCKETNET_DEV
#include <linux/io_uring.h>
#endif // #if defined( __linux__ ) && ROCKETNET_DEV

#if defined( __linux__ ) && ROCKETNET_DEV && defined( IORING_RECV_MULTISHOT )

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

// the instance: a bank of three slots, four buffers, receives of up to two
// datagrams, drains of up to two
#define RECEIVE_SLOTS 3
#define RECEIVE_BUFFERS 4
#define RECEIVE_MAX_SEGS 2
#define RECEIVE_DRAIN_MAX 2
#define RECEIVE_MAX_ARRIVE 40
#define RECEIVE_DATAGRAM_BYTES 64

// each record carries the datagrams banked and delivered so far, so a
// record's size grows with the walk, and TLC's time for a trace grows faster
// than its length cubed (on amd-2: 201 records 5 s, 401 records 116 s,
// against a 110 s budget): the row's steps is 200, and coverage comes from
// more seeds, each inside the budget
#define RECEIVE_BUFFER_BYTES ( 70 * 1024 )
#define RECEIVE_TAIL_WRAP 65536

static const uint8_t receive_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x31, 0x41, 0x59, 0x26 };

struct receive_datagram_t
{
    int d;
    bool ok;
};

struct receive_completion_t
{
    bool end;
    int bid;
    int32_t res;
    std::vector<receive_datagram_t> segs;
};

struct receive_walk_t
{
    struct rocketnet_uring_receive_queue_t * queue;
    int arrived;
    int next_id;
    std::vector<std::vector<receive_datagram_t>> sock;
    bool karmed;
    std::vector<receive_completion_t> cq;
    uint16_t br_head;
    bool owner_ring[RECEIVE_BUFFERS];
    bool reading;
    std::vector<int> vidx;
    std::vector<int> view;
    std::vector<uint8_t *> pointers;
    std::vector<int> banked;
    std::vector<int> delivered;
    bool spurious;
    bool dup;
    bool torn;
};

// a slot's datagram: the id the walk wrote at byte 20, 0 for a slot never written
static int receive_slot_id( const struct rocketnet_uring_receive_queue_t * queue, int slot )
{
    if ( queue->slot[slot].packet_bytes == 0 )
    {
        return 0;
    }
    uint32_t id = 0;
    memcpy( &id, queue->slot_data + (size_t) slot * ROCKETNET_FRAME_STRIDE + 20, sizeof( id ) );
    return (int) id;
}

static int receive_pointer_id( const uint8_t * pointer )
{
    uint32_t id = 0;
    memcpy( &id, pointer + 20, sizeof( id ) );
    return (int) id;
}

static uint64_t receive_named( const struct rocketnet_uring_receive_queue_t * queue )
{
    uint64_t counters[ROCKETNET_URING_RECEIVE_NUM_COUNTERS];
    rocketnet_uring_receive_queue_counters( queue, counters );
    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS];
    memset( totals, 0, sizeof( totals ) );
    rocketnet_uring_receive_queue_drops( queue, totals );
    uint64_t named = counters[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE] + counters[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE] +
                     counters[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_NO_BUFFER] + counters[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_OVERFLOW];
    for ( int d = ROCKETNET_RECEIVE_ACCEPT + 1; d < ROCKETNET_RECEIVE_NUM_DROPS; d++ )
    {
        if ( d != ROCKETNET_RECEIVE_DROP_BANK_FULL )
        {
            named += totals[d];
        }
    }
    return named;
}

static uint64_t receive_full( const struct rocketnet_uring_receive_queue_t * queue )
{
    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS];
    memset( totals, 0, sizeof( totals ) );
    rocketnet_uring_receive_queue_drops( queue, totals );
    return totals[ROCKETNET_RECEIVE_DROP_BANK_FULL];
}

static std::string receive_segs( const std::vector<receive_datagram_t> & segs )
{
    std::string text = "<<";
    for ( size_t i = 0; i < segs.size(); i++ )
    {
        if ( i > 0 )
        {
            text += ", ";
        }
        text += "[d |-> " + tla_int( segs[i].d ) + ", ok |-> " + tla_bool( segs[i].ok ) + "]";
    }
    return text + ">>";
}

static std::string receive_ints( const std::vector<int> & values )
{
    std::string text = "<<";
    for ( size_t i = 0; i < values.size(); i++ )
    {
        if ( i > 0 )
        {
            text += ", ";
        }
        text += tla_int( values[i] );
    }
    return text + ">>";
}

static tla_record_t receive_record( const receive_walk_t & walk )
{
    const struct rocketnet_uring_receive_queue_t * queue = walk.queue;

    std::string sock = "<<";
    for ( size_t i = 0; i < walk.sock.size(); i++ )
    {
        sock += ( i > 0 ? ", " : "" ) + receive_segs( walk.sock[i] );
    }
    sock += ">>";

    std::string cq = "<<";
    for ( size_t i = 0; i < walk.cq.size(); i++ )
    {
        cq += i > 0 ? ", " : "";
        if ( walk.cq[i].end )
        {
            cq += "[kind |-> \"end\"]";
        }
        else
        {
            cq += "[kind |-> \"data\", bid |-> " + tla_int( walk.cq[i].bid ) + ", segs |-> " + receive_segs( walk.cq[i].segs ) + "]";
        }
    }
    cq += ">>";

    const struct io_uring_buf * bufs = (const struct io_uring_buf *) (const void *) queue->buf_ring;
    std::vector<std::string> buffer_keys;
    std::vector<std::string> entries;
    std::vector<std::string> owner;
    for ( int b = 0; b < RECEIVE_BUFFERS; b++ )
    {
        buffer_keys.push_back( tla_int( b ) );
        entries.push_back( tla_int( bufs[b].bid ) );
        owner.push_back( walk.owner_ring[b] ? "\"ring\"" : "\"cq\"" );
    }

    std::vector<std::string> slot_keys;
    std::vector<std::string> slots;
    for ( int s = 0; s < RECEIVE_SLOTS; s++ )
    {
        slot_keys.push_back( tla_int( s ) );
        slots.push_back( tla_int( receive_slot_id( queue, s ) ) );
    }

    tla_record_t record;
    record.add( "arrived", tla_int( walk.arrived ) );
    record.add( "nextId", tla_int( walk.next_id ) );
    record.add( "sock", sock );
    record.add( "karmed", tla_bool( walk.karmed ) );
    record.add( "cq", cq );
    record.add( "armed", tla_bool( queue->armed ) );
    record.add( "stopping", tla_bool( queue->stopping ) );
    record.add( "brHead", tla_int( walk.br_head ) );
    record.add( "brTail", tla_int( __atomic_load_n( &queue->buf_ring->tail, __ATOMIC_ACQUIRE ) ) );
    record.add( "entries", tla_function( buffer_keys, entries ) );
    record.add( "owner", tla_function( buffer_keys, owner ) );
    record.add( "slotD", tla_function( slot_keys, slots ) );
    record.add( "tail", tla_int( (int64_t) queue->tail.load() ) );
    record.add( "head", tla_int( (int64_t) queue->head.load() ) );
    record.add( "headSeen", tla_int( (int64_t) queue->head_seen ) );
    record.add( "exposed", tla_int( (int64_t) queue->exposed ) );
    record.add( "reading", tla_bool( walk.reading ) );
    record.add( "vidx", receive_ints( walk.vidx ) );
    record.add( "view", receive_ints( walk.view ) );
    record.add( "banked", receive_ints( walk.banked ) );
    record.add( "delivered", receive_ints( walk.delivered ) );
    record.add( "named", tla_int( (int64_t) receive_named( queue ) ) );
    record.add( "full", tla_int( (int64_t) receive_full( queue ) ) );
    record.add( "spurious", tla_bool( walk.spurious ) );
    record.add( "dup", tla_bool( walk.dup ) );
    record.add( "torn", tla_bool( walk.torn ) );
    return record;
}

// the network: a receive of one or two datagrams, each accepted or refused
static void receive_arrive( receive_walk_t & walk, tla_random_t & random )
{
    const int n = 1 + random.below( RECEIVE_MAX_SEGS );
    std::vector<receive_datagram_t> segs;
    for ( int i = 0; i < n; i++ )
    {
        receive_datagram_t datagram;
        datagram.d = walk.next_id++;
        datagram.ok = random.below( 4 ) != 0;
        segs.push_back( datagram );
    }
    walk.sock.push_back( segs );
    walk.arrived++;
}

// the fake kernel takes the socket's oldest receive into the buffer at its
// head of the queue's real buffer ring; returns the buffer test_take gave
static int receive_kernel_receive( receive_walk_t & walk )
{
    const int bid = rocketnet_uring_receive_queue_test_take( walk.queue, &walk.br_head );
    if ( bid < 0 || bid >= RECEIVE_BUFFERS )
    {
        walk.dup = true;
        return bid;
    }
    if ( !walk.owner_ring[bid] )
    {
        walk.dup = true;
    }
    walk.owner_ring[bid] = false;

    const std::vector<receive_datagram_t> segs = walk.sock.front();
    walk.sock.erase( walk.sock.begin() );

    uint8_t payload[RECEIVE_MAX_SEGS * RECEIVE_DATAGRAM_BYTES];
    for ( size_t i = 0; i < segs.size(); i++ )
    {
        uint8_t * packet = payload + i * RECEIVE_DATAGRAM_BYTES;
        memset( packet, 0, RECEIVE_DATAGRAM_BYTES );
        packet[0] = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
        packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;
        memcpy( packet + 2, receive_filter, ROCKETNET_FILTER_VALUE_BYTES );
        if ( !segs[i].ok )
        {
            packet[2] ^= 0x01;
        }
        const uint32_t id = (uint32_t) segs[i].d;
        memcpy( packet + 20, &id, sizeof( id ) );
    }

    struct sockaddr_storage name;
    memset( &name, 0, sizeof( name ) );
    struct sockaddr_in * in = (struct sockaddr_in *) &name;
    in->sin_family = AF_INET;
    in->sin_port = htons( 40000 );
    in->sin_addr.s_addr = htonl( 0x0A000009 );

    const int payload_bytes = (int) segs.size() * RECEIVE_DATAGRAM_BYTES;
    receive_completion_t completion;
    completion.end = false;
    completion.bid = bid;
    completion.segs = segs;
    completion.res = rocketnet_uring_receive_queue_test_write( walk.queue, bid, &name, (int) sizeof( struct sockaddr_in ), payload, payload_bytes,
                                                               segs.size() > 1 ? RECEIVE_DATAGRAM_BYTES : 0, 0, 0 );
    walk.cq.push_back( completion );
    return bid;
}

static void receive_kernel_end( receive_walk_t & walk, int32_t res )
{
    walk.karmed = false;
    receive_completion_t completion;
    completion.end = true;
    completion.bid = -1;
    completion.res = res;
    walk.cq.push_back( completion );
}

// the worker: one completion, oldest first; returns what cqe returned, the
// datagrams it banked
static int receive_complete( receive_walk_t & walk )
{
    const receive_completion_t completion = walk.cq.front();
    walk.cq.erase( walk.cq.begin() );

    struct rocketnet_uring_receive_queue_t * queue = walk.queue;
    const uint64_t tail_before = queue->tail.load();
    const uint64_t full_before = receive_full( queue );

    if ( completion.end )
    {
        return rocketnet_uring_receive_queue_cqe( queue, completion.res, 0 );
    }

    const uint32_t flags = IORING_CQE_F_BUFFER | ( (uint32_t) completion.bid << IORING_CQE_BUFFER_SHIFT ) | IORING_CQE_F_MORE;
    const int banked = rocketnet_uring_receive_queue_cqe( queue, completion.res, flags );
    walk.owner_ring[completion.bid] = true;

    // the datagrams the code banked, from its slots
    for ( uint64_t p = tail_before; p < queue->tail.load(); p++ )
    {
        walk.banked.push_back( receive_slot_id( queue, (int) ( p % RECEIVE_SLOTS ) ) );
    }

    // a full drop while main had released room: the worker's view was stale
    if ( receive_full( queue ) != full_before && queue->tail.load() - queue->head.load() < RECEIVE_SLOTS )
    {
        walk.spurious = true;
    }
    return banked;
}

// main's drain; returns what drain returned, and in released whether it
// moved head past the drain before's end with datagrams taken (the plant)
static int receive_drain( receive_walk_t & walk, bool * released )
{
    struct rocketnet_uring_receive_queue_t * queue = walk.queue;
    const uint64_t exposed_before = queue->exposed;

    struct rocketnet_address_t from[RECEIVE_DRAIN_MAX];
    int bytes[RECEIVE_DRAIN_MAX];
    uint8_t * data[RECEIVE_DRAIN_MAX];
    const int n = rocketnet_uring_receive_queue_drain( queue, RECEIVE_DRAIN_MAX, from, bytes, data );

    *released = n > 0 && queue->head.load() != exposed_before;

    walk.vidx.clear();
    walk.view.clear();
    walk.pointers.clear();
    for ( int i = 0; i < n; i++ )
    {
        walk.vidx.push_back( (int) ( ( data[i] - queue->slot_data ) / ROCKETNET_FRAME_STRIDE ) );
        walk.view.push_back( receive_pointer_id( data[i] ) );
        walk.pointers.push_back( data[i] );
    }
    walk.reading = true;
    return n;
}

static void receive_done( receive_walk_t & walk )
{
    std::vector<int> now;
    for ( size_t i = 0; i < walk.pointers.size(); i++ )
    {
        now.push_back( receive_pointer_id( walk.pointers[i] ) );
    }
    if ( now != walk.view )
    {
        walk.torn = true;
    }
    walk.delivered.insert( walk.delivered.end(), now.begin(), now.end() );
    walk.reading = false;
}

static int trace_receive( uint64_t seed, int steps, const char * out )
{
    struct rocketnet_uring_receive_queue_config_t config;
    memset( &config, 0, sizeof( config ) );
    config.queue_index = 0;
    config.fd = -1;
    config.bgid = 0;
    config.buffers = RECEIVE_BUFFERS;
    config.buffer_bytes = RECEIVE_BUFFER_BYTES;
    config.slots = RECEIVE_SLOTS;
    config.gro = true;
    config.packet_filter = receive_filter;

    receive_walk_t walk;
    walk.queue = rocketnet_uring_receive_queue_create( NULL, NULL, &config );
    if ( !walk.queue )
    {
        return 1;
    }
    walk.arrived = 0;
    walk.next_id = 1;
    walk.karmed = false;
    walk.br_head = 0;
    for ( int b = 0; b < RECEIVE_BUFFERS; b++ )
    {
        walk.owner_ring[b] = true;
    }
    walk.reading = false;
    walk.spurious = false;
    walk.dup = false;
    walk.torn = false;

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", receive_record( walk ) ) );

    enum
    {
        ARRIVE,
        KERNEL_RECEIVE,
        KERNEL_NO_BUFFERS,
        KERNEL_CANCEL,
        STOP,
        ARM,
        COMPLETE,
        DRAIN,
        DONE
    };

    for ( int step = 0; step < steps; step++ )
    {
        struct rocketnet_uring_receive_queue_t * queue = walk.queue;
        const uint16_t ring_count = (uint16_t) ( __atomic_load_n( &queue->buf_ring->tail, __ATOMIC_ACQUIRE ) - walk.br_head );

        // the enabled actions, as the model's guards say; the stop is
        // offered rarely, so most walks run long before it
        std::vector<int> enabled;
        if ( walk.arrived < RECEIVE_MAX_ARRIVE )
        {
            enabled.push_back( ARRIVE );
            enabled.push_back( ARRIVE );
        }
        if ( walk.karmed && !walk.sock.empty() && ring_count > 0 )
        {
            enabled.push_back( KERNEL_RECEIVE );
            enabled.push_back( KERNEL_RECEIVE );
        }
        if ( walk.karmed && !walk.sock.empty() && ring_count == 0 )
        {
            enabled.push_back( KERNEL_NO_BUFFERS );
        }
        if ( walk.karmed && queue->stopping )
        {
            enabled.push_back( KERNEL_CANCEL );
        }
        if ( !queue->stopping && random.below( 64 ) == 0 )
        {
            enabled.push_back( STOP );
        }
        if ( rocketnet_uring_receive_queue_needs_arm( queue ) )
        {
            enabled.push_back( ARM );
        }
        if ( !walk.cq.empty() )
        {
            enabled.push_back( COMPLETE );
            enabled.push_back( COMPLETE );
        }
        if ( !walk.reading )
        {
            enabled.push_back( DRAIN );
        }
        else
        {
            enabled.push_back( DONE );
        }

        const char * op = "";
        const char * act = "";
        std::string args = "<<>>";
        bool released = false;
        switch ( enabled[(size_t) random.below( (int) enabled.size() )] )
        {
            case ARRIVE:
                receive_arrive( walk, random );
                op = "arrive";
                act = "Arrive";
                break;
            case KERNEL_RECEIVE:
                args = tla_sequence( { tla_int( receive_kernel_receive( walk ) ) } );
                op = "receive";
                act = "KernelReceive";
                break;
            case KERNEL_NO_BUFFERS:
                receive_kernel_end( walk, -ENOBUFS );
                op = "noBuffers";
                act = "KernelNoBuffers";
                break;
            case KERNEL_CANCEL:
                receive_kernel_end( walk, -ECANCELED );
                op = "cancel";
                act = "KernelCancel";
                break;
            case STOP:
                rocketnet_uring_receive_queue_stop( queue );
                op = "stop";
                act = "Stop";
                break;
            case ARM:
            {
                struct io_uring_sqe sqe;
                const bool armed = rocketnet_uring_receive_queue_arm( queue, &sqe );
                if ( armed )
                {
                    walk.karmed = true;
                }
                op = "arm";
                act = armed ? "Arm" : "ArmRefused";
                break;
            }
            case COMPLETE:
                args = tla_sequence( { tla_int( receive_complete( walk ) ) } );
                op = "cqe";
                act = "Complete";
                break;
            case DRAIN:
                args = tla_sequence( { tla_int( receive_drain( walk, &released ) ) } );
                op = "drain";
                act = "Drain";
                break;
            default:
                receive_done( walk );
                op = "done";
                act = "Done";
                break;
        }
        report.act( act );
        records.push_back( tla_step( op, act, args, receive_record( walk ) ) );
        if ( released )
        {
            report.planted( (int) records.size() );
        }
    }

    rocketnet_uring_receive_queue_destroy( walk.queue );

    report.print();

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    S = %d\n    B = %d\n    TailWrap = %d\n    MaxArrive = %d\n    MaxSegs = %d\n    DrainMax = %d\n"
              "    RoomRule = \"refresh\"\n    SlotRule = \"mod\"\n    ReleaseRule = \"next\"\n    CounterWrap = 0\n",
              RECEIVE_SLOTS, RECEIVE_BUFFERS, RECEIVE_TAIL_WRAP, RECEIVE_MAX_ARRIVE, RECEIVE_MAX_SEGS, RECEIVE_DRAIN_MAX );
    return tla_write_trace( out, "MCTraceReceive", seed, "TraceRNUringQueueReceiveLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "receive", trace_receive );

#endif // #if defined( __linux__ ) && ROCKETNET_DEV && defined( IORING_RECV_MULTISHOT )
