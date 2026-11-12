/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// layer L2 of the gso push: the per-queue send staging
// (proofs/tla/RNGsoQueueBankLayout.tla), checked by
// proofs/tla/TraceRNGsoQueueBankLayout.tla (README-TraceCheck.md): each record
// the walk appends must be one TraceStep from the one before, and TraceReject
// is TLC's verdict on the first that is not.
//
//   tla_trace --layer gso --seed N --out DIR [--steps K]
//
// writes DIR/MCTraceGsoSeed<seed>.tla and .cfg (MCTraceGsoPlantSeed<seed>
// from the plant build). The walk drives the real start, finish, abort,
// send_packets (publish), take and return on the rocketnet_server_gso_test_open
// fixture with two queues: no socket, no thread, no clock. It reads the model's
// variables from the lanes' own fields, so the transport's source is compiled
// into this file (the library's copy is not linked: every symbol it defines is
// defined here first). The staging's slots per bank, ROCKETNET_GSO_SEND_QUEUE_SIZE,
// is 3 in this file only, so a fill runs past a bank and each record stays
// small: the io_uring walk's banks of three slots, where the slot count is an
// argument. Every other constant is the code's.
//
// The plant (ROCKETNET_GSO_SEND_SLOT_PLANT, the tla_trace_plant build) replaces
// finish and abort with copies whose slot is divided out of the pointer's
// offset before any test (RNGsoQueueBankLayout SlotRule "divide"): a pointer a
// byte early lands on the slot before, so its trace must fail TraceConsistent.

#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_DEV

#include "../sdk/rocketnet_server_gso.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_platform.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_frame.h"

#undef ROCKETNET_GSO_SEND_QUEUE_SIZE
#define ROCKETNET_GSO_SEND_QUEUE_SIZE 3

#if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )
void rocketnet_server_gso_finish_packet_unplanted( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );
void rocketnet_server_gso_abort_packet_unplanted( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data );
#define rocketnet_server_gso_finish_packet rocketnet_server_gso_finish_packet_unplanted
#define rocketnet_server_gso_abort_packet rocketnet_server_gso_abort_packet_unplanted
#endif // #if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )

#include "../sdk/rocketnet_server_gso_linux.cpp"

#if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )
#undef rocketnet_server_gso_finish_packet
#undef rocketnet_server_gso_abort_packet

// the plant's slot: the pointer's signed offset past the producer's first slot,
// divided with C's truncation toward zero before the range is tested, and no
// alignment test at all
static int rocketnet_server_gso_plant_send_slot( struct rocketnet_server_gso_t * gso, uint64_t packet_id, const uint8_t * packet_data,
                                                 rocketnet_server_gso_send_buffer_t ** bank_out )
{
    if ( !packet_data || gso->stopping )
    {
        return -1;
    }

    const int q = (int) ( packet_id % (uint64_t) gso->num_queues );
    struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[q].send;
    if ( lane->producer < 0 )
    {
        return -1;
    }
    rocketnet_server_gso_send_buffer_t * bank = &lane->banks[lane->producer];

    const intptr_t offset = (intptr_t) ( (uintptr_t) packet_data - (uintptr_t) bank->data[0] );
    const intptr_t slot = offset / (intptr_t) ROCKETNET_FRAME_STRIDE;
    if ( slot < 0 || slot >= ROCKETNET_GSO_SEND_QUEUE_SIZE )
    {
        return -1;
    }

    *bank_out = bank;
    return (int) slot;
}

void rocketnet_server_gso_finish_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( gso );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    rocketnet_server_gso_send_buffer_t * bank = NULL;
    const int slot = rocketnet_server_gso_plant_send_slot( gso, packet_id, packet_data, &bank );
    if ( slot < 0 )
    {
        return;
    }

    bank->info[slot].packet_bytes = packet_bytes;
}

void rocketnet_server_gso_abort_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( gso );

    rocketnet_server_gso_send_buffer_t * bank = NULL;
    const int slot = rocketnet_server_gso_plant_send_slot( gso, packet_id, packet_data, &bank );
    if ( slot < 0 )
    {
        return;
    }

    bank->info[slot].packet_bytes = 0;
}
#endif // #if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the instance: two queues of the code's three banks, two fill threads making
// four starts each per fill so claims run past a bank, three destinations (two
// on queue 0, one on queue 1, by the code's hash) and six flushes, so every
// bank of a queue is returned and turned over onto again
#define GSO_QUEUES 2
#define GSO_SLOTS ROCKETNET_GSO_SEND_QUEUE_SIZE
#define GSO_THREADS 2
#define GSO_DESTS 3
#define GSO_DEMAND 4
#define GSO_MAX_FLUSH 6

static const int gso_skew[] = { -1, 0, 1 };

struct gso_load_t
{
    bool loaded;
    int d;
    int q;
    int b;
    int i;
};

struct gso_claim_t
{
    bool held;
    int q;
    int b;
    int slot;
    int64_t ptr;
    uint64_t pid;
};

struct gso_walk_t
{
    struct rocketnet_server_gso_t * gso;
    struct rocketnet_address_t dest[GSO_DESTS + 1];     // destinations 1..GSO_DESTS
    gso_load_t load[GSO_THREADS + 1];                   // threads 1..GSO_THREADS
    gso_claim_t claim[GSO_THREADS + 1];
    int owner[GSO_QUEUES][ROCKETNET_GSO_TX_BANKS][GSO_SLOTS + 1];
    int demand[GSO_THREADS + 1];
    int flushes;
    bool oob;
    bool shared;
    bool foreign;
    bool route_bad;
    bool worker_write;
    int foreign_writes;     // finishes and aborts that wrote a slot not their own: the plant firing
    int finishes;           // finish calls so far: each writes packet_bytes no slot holds yet
    bool early_probed;      // a finish a byte early on a slot past 0 has run: the plant's condition
    bool worker_slow;       // this fill the queues' workers return nothing
    // what the walk did, printed for the record: loads, claims, reloads (a
    // compare-and-swap that lost), refused starts, finishes and aborts,
    // flushes, turn-overs, producers kept for want of a free bank, returns
    int steps_load;
    int steps_claim;
    int steps_reload;
    int steps_refused;
    int steps_finish;
    int steps_abort;
    int steps_flush;
    int steps_turn;
    int steps_kept;
    int steps_return;
};

// RNGsoQueueBankLayout Addr(q, b): the model lays the banks end to end
static int64_t gso_addr( int q, int b )
{
    return (int64_t) ( q * ROCKETNET_GSO_TX_BANKS + b ) * GSO_SLOTS * ROCKETNET_FRAME_STRIDE;
}

static uintptr_t gso_first( const gso_walk_t & walk, int q, int b )
{
    return (uintptr_t) walk.gso->queues[q].send.banks[b].data[0];
}

// the bank a pointer the code returned lies in, and its byte offset there
static bool gso_locate( const gso_walk_t & walk, const uint8_t * pointer, int * q_out, int * b_out, int64_t * offset_out )
{
    const uintptr_t address = (uintptr_t) pointer;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
        {
            const uintptr_t first = gso_first( walk, q, b );
            if ( address >= first && address - first < sizeof( walk.gso->queues[q].send.banks[b].data ) )
            {
                *q_out = q;
                *b_out = b;
                *offset_out = (int64_t) ( address - first );
                return true;
            }
        }
    }
    return false;
}

// RNGsoQueueBankLayout own: the producer, a bank in the lane's free FIFO, or
// the worker's (published, taken, not yet returned)
static std::string gso_own( const struct rocketnet_server_gso_send_lane_t * lane, int b )
{
    if ( b == lane->producer )
    {
        return "\"prod\"";
    }
    for ( int i = 0; i < lane->free_count; i++ )
    {
        if ( lane->free_banks[( lane->free_head + i ) % ROCKETNET_GSO_TX_BANKS] == b )
        {
            return "\"free\"";
        }
    }
    return "\"worker\"";
}

static int gso_count( const gso_walk_t & walk, int q, int b )
{
    return walk.gso->queues[q].send.banks[b].num_packets.load( std::memory_order_relaxed );
}

// the next finish's packet_bytes, 1..ROCKETNET_RECEIVE_CEILING: distinct for a
// walk.s first ROCKETNET_RECEIVE_CEILING finishes (a 400-step walk makes 800 at most),
// so a write is seen whatever the slot held
static int gso_bytes( gso_walk_t & walk )
{
    return 1 + walk.finishes++ % ROCKETNET_RECEIVE_CEILING;
}

static tla_record_t gso_record( const gso_walk_t & walk )
{
    std::vector<std::string> queue_keys;
    std::vector<std::string> prod;
    std::vector<std::string> own;
    std::vector<std::string> cnt;
    std::vector<std::string> turn;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        const struct rocketnet_server_gso_send_lane_t * lane = &walk.gso->queues[q].send;
        queue_keys.push_back( tla_int( q ) );
        prod.push_back( tla_int( lane->producer ) );
        turn.push_back( tla_int( (int64_t) lane->turn ) );
        std::vector<std::string> bank_keys;
        std::vector<std::string> owners;
        std::vector<std::string> counts;
        for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
        {
            bank_keys.push_back( tla_int( b ) );
            owners.push_back( gso_own( lane, b ) );
            counts.push_back( tla_int( gso_count( walk, q, b ) ) );
        }
        own.push_back( tla_function( bank_keys, owners ) );
        cnt.push_back( tla_function( bank_keys, counts ) );
    }

    std::vector<std::string> thread_keys;
    std::vector<std::string> load;
    std::vector<std::string> held;
    std::vector<std::string> demand;
    for ( int t = 1; t <= GSO_THREADS; t++ )
    {
        thread_keys.push_back( tla_int( t ) );
        demand.push_back( tla_int( walk.demand[t] ) );
        const gso_load_t & l = walk.load[t];
        if ( !l.loaded )
        {
            load.push_back( "[none |-> TRUE]" );
        }
        else
        {
            load.push_back( "[q |-> " + tla_int( l.q ) + ", b |-> " + tla_int( l.b ) + ", i |-> " + tla_int( l.i ) + "]" );
        }
        const gso_claim_t & c = walk.claim[t];
        if ( !c.held )
        {
            held.push_back( "[none |-> TRUE]" );
        }
        else
        {
            held.push_back( "[q |-> " + tla_int( c.q ) + ", b |-> " + tla_int( c.b ) + ", slot |-> " + tla_int( c.slot ) +
                            ", ptr |-> " + tla_int( c.ptr ) + ", pid |-> " + tla_int( (int64_t) c.pid ) + "]" );
        }
    }

    std::vector<std::string> slot_keys;
    std::vector<std::string> owner;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
        {
            for ( int s = 0; s <= GSO_SLOTS; s++ )
            {
                slot_keys.push_back( "<<" + tla_int( q ) + ", " + tla_int( b ) + ", " + tla_int( s ) + ">>" );
                owner.push_back( tla_int( walk.owner[q][b][s] ) );
            }
        }
    }

    tla_record_t record;
    record.add( "prod", tla_function( queue_keys, prod ) );
    record.add( "own", tla_function( queue_keys, own ) );
    record.add( "cnt", tla_function( queue_keys, cnt ) );
    record.add( "turn", tla_function( queue_keys, turn ) );
    record.add( "load", tla_function( thread_keys, load ) );
    record.add( "held", tla_function( thread_keys, held ) );
    record.add( "owner", tla_function( slot_keys, owner ) );
    record.add( "demand", tla_function( thread_keys, demand ) );
    record.add( "flushes", tla_int( walk.flushes ) );
    record.add( "oob", tla_bool( walk.oob ) );
    record.add( "shared", tla_bool( walk.shared ) );
    record.add( "foreign", tla_bool( walk.foreign ) );
    record.add( "routeBad", tla_bool( walk.route_bad ) );
    record.add( "workerWrite", tla_bool( walk.worker_write ) );
    return record;
}

// RNGsoQueueBankLayout Load(t, d): the first half of a start, the queue by
// the code's hash and that queue's producer and counter as the code holds them
static void gso_load( gso_walk_t & walk, int t, int d )
{
    const int q = rocketnet_server_gso_send_queue( &walk.dest[d], GSO_QUEUES );
    const struct rocketnet_server_gso_send_lane_t * lane = &walk.gso->queues[q].send;
    gso_load_t & l = walk.load[t];
    l.loaded = true;
    l.d = d;
    l.q = q;
    l.b = lane->producer;
    l.i = gso_count( walk, q, lane->producer );
    walk.demand[t]++;
    walk.steps_load++;
}

// the code's start returned a frame: read it back as the model's claim (the
// queue, bank and slot are where the pointer lies), with the flags a claim
// outside the bank, on a held slot or in a bank the worker owns would set
static void gso_take_claim( gso_walk_t & walk, int t, const uint8_t * frame, uint64_t pid )
{
    gso_claim_t & c = walk.claim[t];
    c.held = true;
    c.pid = pid;

    int q = 0;
    int b = 0;
    int64_t offset = 0;
    if ( !gso_locate( walk, frame, &q, &b, &offset ) || offset % ROCKETNET_FRAME_STRIDE != 0 )
    {
        c.q = -1;
        c.b = -1;
        c.slot = -1;
        c.ptr = -1;
        walk.oob = true;
        return;
    }

    c.q = q;
    c.b = b;
    c.slot = (int) ( offset / ROCKETNET_FRAME_STRIDE );
    c.ptr = gso_addr( q, b ) + offset;
    if ( c.slot >= GSO_SLOTS )
    {
        walk.oob = true;
        return;
    }
    if ( walk.owner[q][b][c.slot] != -1 )
    {
        walk.shared = true;
    }
    walk.owner[q][b][c.slot] = t;
    if ( b != walk.gso->queues[q].send.producer )
    {
        walk.worker_write = true;
    }
}

// RNGsoQueueBankLayout Claim(t): the second half. A loaded index at S is a
// refused start (the code's start is called and must return NULL). A counter
// another thread moved since the load is the compare-and-swap that lost:
// compare_exchange_weak reloads the counter, which is the model's reload and
// calls nothing. Otherwise the code's start is called and its claim recorded.
static void gso_claim( gso_walk_t & walk, int t )
{
    gso_load_t & l = walk.load[t];
    const struct rocketnet_server_gso_send_lane_t * lane = &walk.gso->queues[l.q].send;

    if ( l.i < GSO_SLOTS && gso_count( walk, l.q, l.b ) != l.i )
    {
        l.b = lane->producer;
        l.i = gso_count( walk, l.q, lane->producer );
        walk.steps_reload++;
        return;
    }

    uint64_t pid = 0;
    const uint8_t * frame = rocketnet_server_gso_start_packet( walk.gso, &walk.dest[l.d], &pid );
    l.loaded = false;
    if ( frame )
    {
        gso_take_claim( walk, t, frame, pid );
        walk.steps_claim++;
    }
    else
    {
        walk.steps_refused++;
    }
}

// RNGsoQueueBankLayout Finish(t, k): the code's finish or abort with the
// pointer k bytes off the one start returned; the slot it wrote is found by
// comparing every slot's packet_bytes before and after
static void gso_finish( gso_walk_t & walk, int t, int k, bool abort )
{
    gso_claim_t & c = walk.claim[t];
    struct rocketnet_server_gso_t * gso = walk.gso;

    const int64_t offset = c.ptr - gso_addr( c.q, c.b );
    uint8_t * exact = (uint8_t *) ( gso_first( walk, c.q, c.b ) + (uintptr_t) offset );
    uint8_t * pointer = (uint8_t *) ( (uintptr_t) exact + (uintptr_t) (intptr_t) k );

    if ( abort )
    {
        // an abort writes 0, which an unfinished slot already holds: finish the
        // claim's own slot through the exact pointer first so the abort shows
        rocketnet_server_gso_finish_packet( gso, c.pid, exact, gso_bytes( walk ) );
    }

    std::vector<int> before;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
        {
            for ( int s = 0; s < GSO_SLOTS; s++ )
            {
                before.push_back( gso->queues[q].send.banks[b].info[s].packet_bytes );
            }
        }
    }

    if ( abort )
    {
        rocketnet_server_gso_abort_packet( gso, c.pid, pointer );
        walk.steps_abort++;
    }
    else
    {
        rocketnet_server_gso_finish_packet( gso, c.pid, pointer, gso_bytes( walk ) );
        walk.steps_finish++;
    }

    size_t index = 0;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
        {
            for ( int s = 0; s < GSO_SLOTS; s++ )
            {
                if ( gso->queues[q].send.banks[b].info[s].packet_bytes != before[index] )
                {
                    if ( q != c.q || b != c.b || s != c.slot )
                    {
                        walk.foreign_writes++;
                    }
                    if ( walk.owner[q][b][s] != t || q != c.q || b != c.b || s != c.slot )
                    {
                        walk.foreign = true;
                    }
                    if ( b != gso->queues[q].send.producer )
                    {
                        walk.worker_write = true;
                    }
                }
                index++;
            }
        }
    }

    if ( (int) ( c.pid % GSO_QUEUES ) != c.q )
    {
        walk.route_bad = true;
    }

    c.held = false;
}

// RNGsoQueueBankLayout Flush: the code's send_packets with the fill joined;
// each queue that turned over starts its new producer with no slot handed out
static void gso_flush( gso_walk_t & walk )
{
    int before[GSO_QUEUES];
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        before[q] = walk.gso->queues[q].send.producer;
    }
    rocketnet_server_gso_send_packets( walk.gso );
    walk.steps_flush++;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        if ( walk.gso->queues[q].send.producer == before[q] )
        {
            if ( gso_count( walk, q, before[q] ) != 0 )
            {
                walk.steps_kept++;
            }
            continue;
        }
        walk.steps_turn++;
        for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
        {
            for ( int s = 0; s <= GSO_SLOTS; s++ )
            {
                walk.owner[q][b][s] = -1;
            }
        }
    }
    for ( int t = 1; t <= GSO_THREADS; t++ )
    {
        walk.demand[t] = 0;
    }
    walk.flushes++;
}

// RNGsoQueueBankLayout WorkerReturn(q, b): the queue's worker takes its oldest
// published bank and gives it back, its counter reset (the send between is
// layer L3's, RNGsoHandoff)
static void gso_return( gso_walk_t & walk, int q )
{
    struct rocketnet_server_gso_queue_t * queue = &walk.gso->queues[q];
    bool final = false;
    const int index = rocketnet_server_gso_take_send( queue, &final );
    if ( index >= 0 && !final )
    {
        rocketnet_server_gso_return_send( queue, index );
        walk.steps_return++;
    }
}

static int trace_gso( uint64_t seed, int steps, const char * out )
{
    gso_walk_t walk;
    memset( &walk, 0, sizeof( walk ) );
    walk.gso = rocketnet_server_gso_test_open( GSO_QUEUES );
    if ( !walk.gso )
    {
        return 1;
    }

    // destinations: two on queue 0 and one on queue 1, by the code's hash
    int found[GSO_QUEUES] = { 0, 0 };
    const int want[GSO_QUEUES] = { 2, 1 };
    int d = 1;
    for ( int port = 40000; port < 41000 && d <= GSO_DESTS; port++ )
    {
        struct rocketnet_address_t a;
        memset( &a, 0, sizeof( a ) );
        a.type = ROCKETNET_ADDRESS_IPV4;
        a.data.ipv4[0] = 10;
        a.data.ipv4[3] = 9;
        a.port = (uint16_t) port;
        const int q = rocketnet_server_gso_send_queue( &a, GSO_QUEUES );
        if ( found[q] < want[q] )
        {
            found[q]++;
            walk.dest[d++] = a;
        }
    }

    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
        {
            for ( int s = 0; s <= GSO_SLOTS; s++ )
            {
                walk.owner[q][b][s] = -1;
            }
        }
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    records.push_back( gso_record( walk ) );

    for ( int step = 0; step < steps; step++ )
    {
        // the enabled actions, as the model's guards say
        std::vector<int> can_load;
        std::vector<int> can_claim;
        std::vector<int> can_finish;
        std::vector<int> can_return;
        bool all_free = true;
        bool all_done = true;
        for ( int t = 1; t <= GSO_THREADS; t++ )
        {
            if ( walk.claim[t].held )
            {
                can_finish.push_back( t );
                all_free = false;
            }
            else if ( walk.load[t].loaded )
            {
                can_claim.push_back( t );
                all_free = false;
            }
            else if ( walk.demand[t] < GSO_DEMAND )
            {
                can_load.push_back( t );
            }
            if ( walk.demand[t] < GSO_DEMAND )
            {
                all_done = false;
            }
        }
        // a worker's return is offered one step in four while the threads
        // still fill and every step once they are done, except in a fill the
        // walk drew a slow worker for (one in two): then none is, so a run of
        // slow fills uses up the free banks and a flush keeps its producer
        const bool offer_return = !walk.worker_slow && ( all_done || random.below( 4 ) == 0 );
        for ( int q = 0; q < GSO_QUEUES; q++ )
        {
            if ( offer_return && walk.gso->queues[q].send.ready_count > 0 )
            {
                can_return.push_back( q );
            }
        }
        // a flush before the threads have spent their demand is offered one
        // step in eight, so most fills run past a bank and stage on both queues
        const bool can_flush = all_free && walk.flushes < GSO_MAX_FLUSH && ( all_done || random.below( 8 ) == 0 );

        const int n_load = (int) can_load.size();
        const int n_claim = (int) can_claim.size();
        const int n_finish = (int) can_finish.size();
        const int n_return = (int) can_return.size();
        const int choices = n_load + n_claim + n_finish + n_return + ( can_flush ? 1 : 0 );
        if ( choices == 0 )
        {
            break;
        }

        const int pick = random.below( choices );
        if ( pick < n_load )
        {
            gso_load( walk, can_load[(size_t) pick], 1 + random.below( GSO_DESTS ) );
        }
        else if ( pick < n_load + n_claim )
        {
            gso_claim( walk, can_claim[(size_t) ( pick - n_load )] );
        }
        else if ( pick < n_load + n_claim + n_finish )
        {
            const int t = can_finish[(size_t) ( pick - n_load - n_claim )];
            int k = gso_skew[random.below( (int) ( sizeof( gso_skew ) / sizeof( gso_skew[0] ) ) )];
            bool abort = random.below( 4 ) == 0;
            // the plant's condition, a finish a byte early on a slot past 0,
            // is made sure once a walk: the first finish of such a claim
            if ( !walk.early_probed && walk.claim[t].slot > 0 )
            {
                k = -1;
                abort = false;
            }
            if ( k == -1 && !abort && walk.claim[t].slot > 0 )
            {
                walk.early_probed = true;
            }
            gso_finish( walk, t, k, abort );
        }
        else if ( pick < n_load + n_claim + n_finish + n_return )
        {
            gso_return( walk, can_return[(size_t) ( pick - n_load - n_claim - n_finish )] );
        }
        else
        {
            gso_flush( walk );
            walk.worker_slow = random.below( 2 ) == 0;
        }

        records.push_back( gso_record( walk ) );
    }

    std::string queue_of = "MCQueueOf(d) == CASE ";
    for ( int i = 1; i <= GSO_DESTS; i++ )
    {
        if ( i > 1 )
        {
            queue_of += " [] ";
        }
        queue_of += "d = " + tla_int( i ) + " -> " + tla_int( rocketnet_server_gso_send_queue( &walk.dest[i], GSO_QUEUES ) );
    }
    const std::string definitions = queue_of + "\nMCSkew == {-1, 0, 1}\n";

    rocketnet_server_gso_test_close( walk.gso );

    // the plant build must fire for its trace to be a witness; the clean
    // build never does
    printf( "WALK records=%d loads=%d claims=%d reloads=%d refused=%d finishes=%d aborts=%d flushes=%d turns=%d kept=%d returns=%d\n",
            (int) records.size(), walk.steps_load, walk.steps_claim, walk.steps_reload, walk.steps_refused, walk.steps_finish,
            walk.steps_abort, walk.steps_flush, walk.steps_turn, walk.steps_kept, walk.steps_return );
    printf( "FOREIGN-WRITES %d\n", walk.foreign_writes );

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    Q = %d\n    NB = %d\n    S = %d\n    Stride = %d\n    Threads = {1, 2}\n    Dests = {1, 2, 3}\n"
              "    QueueOf <- MCQueueOf\n    Demand = %d\n    MaxFlush = %d\n    Skew <- MCSkew\n"
              "    ClaimCmp = \"ge\"\n    ClaimRule = \"cas\"\n    SlotRule = \"check\"\n    IdRule = \"mul\"\n    FlushRule = \"joined\"\n",
              GSO_QUEUES, ROCKETNET_GSO_TX_BANKS, GSO_SLOTS, (int) ROCKETNET_FRAME_STRIDE, GSO_DEMAND, GSO_MAX_FLUSH );

#if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )
    const std::string name = "MCTraceGsoPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )
    const std::string name = "MCTraceGsoSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )

    return tla_write_trace( out, name, "TraceRNGsoQueueBankLayout", definitions, constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "gso", trace_gso );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_DEV
