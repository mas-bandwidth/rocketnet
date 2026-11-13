/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Layer L0 of the io_uring per-queue send: the trace walk of the per-queue
// send banks (sdk/rocketnet_uring_send_queue.cpp, the model
// proofs/tla/RNUringQueueBankLayout.tla), checked by
// proofs/tla/TraceRNUringQueueBankLayout.tla (proofs/tla/README-TraceCheck.md).
// No socket, no clock, no thread: the walk interleaves the fill threads'
// calls itself, one call per step, and writes one record per call
// (tla_step): the call, what it took and gave back, the action its own
// result says it took, and every model variable after it. A start that
// returns a pointer is Start(t, d, ptr, pid) with the pointer's offset and
// the id it wrote, one that returns NULL is StartFull(t, d, pid); a finish
// or abort is Finish(t, k, w) when it returns true, FinishRefused(t, k) when
// false; a turn is Flush(n) with the queues it says it turned.
//
// The plant binary's code is the banks with ROCKETNET_URING_SEND_QUEUE_PLANT
// (the turn without its reset). The walk reports (tla_report_t) the planted
// effect where it shows: a turn whose new producer bank still counts claims,
// which the clean code never leaves.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_URING_SEND_QUEUE_PLANT 1
#include "../sdk/rocketnet_uring_send_queue.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_send_queue.h"
#include "../sdk/rocketnet_uring_send_queue.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the instance: small enough for TLC to check each trace in seconds, with
// claims past a bank's slots and every bank of a queue reused. Destinations
// are numbered BANK_FIRST_DEST.. so that d mod BANK_QUEUES is the queue the
// code's hash gives (the model's QueueOf): 2 and 4 on queue 0, 3 on queue 1
#define BANK_QUEUES 2
#define BANK_SLOTS 3
#define BANK_THREADS 2
#define BANK_FIRST_DEST 2
#define BANK_DESTS 3
#define BANK_DEMAND 4

// the most bytes a finish's pointer is off the start's, either way (the model's MaxSkew)
#define BANK_MAX_SKEW 1

struct bank_claim_t
{
    bool held;
    int q;
    int b;
    int slot;
    int64_t ptr;
    uint64_t pid;
};

struct bank_walk_t
{
    struct rocketnet_uring_send_queues_t * queues;
    struct rocketnet_address_t dest[BANK_FIRST_DEST + BANK_DESTS];     // destinations BANK_FIRST_DEST..
    bank_claim_t claim[BANK_THREADS + 1];               // threads 1..BANK_THREADS
    int owner[BANK_QUEUES][ROCKETNET_URING_SEND_BANKS][BANK_SLOTS + 1];
    int demand[BANK_THREADS + 1];
    int flushes;
    bool oob;
    bool shared;
    bool foreign;
    bool route_bad;
    int lw_q;          // the slot the last call wrote (the model's lw), -1 for none
    int lw_b;
    int lw_s;
    bool lw_nonzero;   // its packet_bytes after the call is non-zero
};

// the byte offset of a pointer past the first bank: the model's address
static int64_t bank_offset( const bank_walk_t & walk, const uint8_t * pointer )
{
    return (int64_t) ( pointer - walk.queues->queue[0].bank[0].data );
}

static tla_record_t bank_record( const bank_walk_t & walk )
{
    const struct rocketnet_uring_send_queues_t * queues = walk.queues;

    std::vector<std::string> queue_keys;
    std::vector<std::string> prod;
    std::vector<std::string> cnt;
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        queue_keys.push_back( tla_int( q ) );
        prod.push_back( tla_int( queues->queue[q].producer ) );
        std::vector<std::string> bank_keys;
        std::vector<std::string> counts;
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            bank_keys.push_back( tla_int( b ) );
            counts.push_back( tla_int( (int64_t) queues->queue[q].bank[b].num_packets.load() ) );
        }
        cnt.push_back( tla_function( bank_keys, counts ) );
    }

    std::vector<std::string> thread_keys;
    std::vector<std::string> held;
    std::vector<std::string> demand;
    for ( int t = 1; t <= BANK_THREADS; t++ )
    {
        thread_keys.push_back( tla_int( t ) );
        demand.push_back( tla_int( walk.demand[t] ) );
        const bank_claim_t & c = walk.claim[t];
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
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            for ( int s = 0; s <= BANK_SLOTS; s++ )
            {
                slot_keys.push_back( "<<" + tla_int( q ) + ", " + tla_int( b ) + ", " + tla_int( s ) + ">>" );
                owner.push_back( tla_int( walk.owner[q][b][s] ) );
            }
        }
    }

    // stg: what staged returns for each queue, called once for this record
    std::vector<std::string> staged;
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        staged.push_back( tla_int( rocketnet_uring_send_queues_staged( queues, q ) ) );
    }

    tla_record_t record;
    record.add( "prod", tla_function( queue_keys, prod ) );
    record.add( "cnt", tla_function( queue_keys, cnt ) );
    record.add( "gid", tla_int( (int64_t) queues->packet_id.load() ) );
    record.add( "held", tla_function( thread_keys, held ) );
    record.add( "owner", tla_function( slot_keys, owner ) );
    record.add( "lw", tla_int( walk.lw_q >= 0 && walk.lw_nonzero ? 1 : 0 ) );
    record.add( "stg", tla_function( queue_keys, staged ) );
    record.add( "demand", tla_function( thread_keys, demand ) );
    record.add( "flushes", tla_int( walk.flushes ) );
    record.add( "oob", tla_bool( walk.oob ) );
    record.add( "shared", tla_bool( walk.shared ) );
    record.add( "foreign", tla_bool( walk.foreign ) );
    record.add( "routeBad", tla_bool( walk.route_bad ) );
    return record;
}

// RNUringQueueBankLayout Start(t, d, ptr, pid), or StartFull(t, d, pid) when
// the code returns NULL: the code's start, read back as the model's claim:
// the queue, bank and slot are where the returned pointer lies. Returns the
// action and writes the call's arguments and results
static const char * bank_start( bank_walk_t & walk, int t, int d, std::string & args )
{
    uint64_t pid = 0;
    uint8_t * frame = rocketnet_uring_send_queues_start( walk.queues, &walk.dest[d], &pid );
    walk.demand[t]++;
    walk.lw_q = -1;
    if ( !frame )
    {
        args = tla_sequence( { tla_int( t ), tla_int( d ), tla_int( (int64_t) pid ) } );
        return "StartFull";
    }

    const int64_t ptr = bank_offset( walk, frame );
    args = tla_sequence( { tla_int( t ), tla_int( d ), tla_int( ptr ), tla_int( (int64_t) pid ) } );
    const int64_t bank_bytes = (int64_t) BANK_SLOTS * ROCKETNET_FRAME_STRIDE;
    const int64_t bank_index = ptr >= 0 ? ptr / bank_bytes : -1;
    bank_claim_t & c = walk.claim[t];
    c.held = true;
    c.q = (int) ( bank_index / ROCKETNET_URING_SEND_BANKS );
    c.b = (int) ( bank_index % ROCKETNET_URING_SEND_BANKS );
    c.slot = (int) ( ( ptr - bank_index * bank_bytes ) / ROCKETNET_FRAME_STRIDE );
    c.ptr = ptr;
    c.pid = pid;

    if ( ptr < 0 || c.q >= BANK_QUEUES || c.slot >= BANK_SLOTS || ( ptr - bank_index * bank_bytes ) % ROCKETNET_FRAME_STRIDE != 0 )
    {
        walk.oob = true;
        return "Start";
    }

    if ( walk.owner[c.q][c.b][c.slot] != -1 )
    {
        walk.shared = true;
    }
    walk.owner[c.q][c.b][c.slot] = t;
    return "Start";
}

// RNUringQueueBankLayout Finish(t, k, w), or FinishRefused(t, k) when the code
// returns false: the code's finish or abort with the pointer k bytes off.
// The slot it wrote is found by comparing every slot record before and after.
static const char * bank_finish( bank_walk_t & walk, int t, int k, bool abort, int packet_bytes )
{
    bank_claim_t & c = walk.claim[t];

    struct rocketnet_uring_send_queues_t * queues = walk.queues;
    std::vector<struct rocketnet_uring_send_slot_t> before;
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            for ( int s = 0; s < BANK_SLOTS; s++ )
            {
                before.push_back( queues->queue[q].bank[b].slot[s] );
            }
        }
    }

    uint8_t * pointer = queues->queue[0].bank[0].data + c.ptr + k;
    walk.lw_q = -1;
    const bool ok = abort ? rocketnet_uring_send_queues_abort( queues, c.pid, pointer ) : rocketnet_uring_send_queues_finish( queues, c.pid, pointer, packet_bytes );

    size_t index = 0;
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            for ( int s = 0; s < BANK_SLOTS; s++ )
            {
                const bool written = queues->queue[q].bank[b].slot[s].packet_bytes != before[index].packet_bytes;
                if ( written )
                {
                    walk.lw_q = q;
                    walk.lw_b = b;
                    walk.lw_s = s;
                    walk.lw_nonzero = queues->queue[q].bank[b].slot[s].packet_bytes != 0;
                }
                if ( written && ( walk.owner[q][b][s] != t || q != c.q || b != c.b || s != c.slot ) )
                {
                    walk.foreign = true;
                }
                index++;
            }
        }
    }

    if ( (int) ( c.pid % BANK_QUEUES ) != c.q )
    {
        walk.route_bad = true;
    }

    c.held = false;
    return ok ? "Finish" : "FinishRefused";
}

// RNUringQueueBankLayout Flush(n): the code's turn, the fill joined; n is
// the queues the turn says it turned. Returns whether a turned queue's new
// producer bank still counts claims: the planted effect, which the clean
// code never leaves
static bool bank_flush( bank_walk_t & walk, int * turned )
{
    int before[BANK_QUEUES];
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        before[q] = walk.queues->queue[q].producer;
    }
    *turned = rocketnet_uring_send_queues_turn( walk.queues );
    walk.lw_q = -1;
    bool stale = false;
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        const struct rocketnet_uring_send_queue_t * queue = &walk.queues->queue[q];
        if ( queue->producer != before[q] && queue->bank[queue->producer].num_packets.load() != 0 )
        {
            stale = true;
        }
    }
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            for ( int s = 0; s <= BANK_SLOTS; s++ )
            {
                walk.owner[q][b][s] = -1;
            }
        }
    }
    for ( int t = 1; t <= BANK_THREADS; t++ )
    {
        walk.demand[t] = 0;
    }
    walk.flushes++;
    return stale;
}

static int trace_bank( uint64_t seed, int steps, const char * out )
{
    bank_walk_t walk;
    memset( &walk, 0, sizeof( walk ) );
    walk.lw_q = -1;
    walk.queues = rocketnet_uring_send_queues_create( NULL, BANK_QUEUES, BANK_SLOTS );
    if ( !walk.queues )
    {
        return 1;
    }

    // destination d is the first address, by port, whose queue by the
    // code's hash is d mod BANK_QUEUES (the model's QueueOf)
    for ( int d = BANK_FIRST_DEST; d < BANK_FIRST_DEST + BANK_DESTS; d++ )
    {
        bool found = false;
        for ( int port = 40000; port < 41000 && !found; port++ )
        {
            struct rocketnet_address_t a;
            memset( &a, 0, sizeof( a ) );
            a.type = ROCKETNET_ADDRESS_IPV4;
            a.data.ipv4[0] = 10;
            a.data.ipv4[3] = (uint8_t) d;
            a.port = (uint16_t) port;
            if ( rocketnet_send_queue_of( &a, BANK_QUEUES ) == d % BANK_QUEUES )
            {
                walk.dest[d] = a;
                found = true;
            }
        }
        if ( !found )
        {
            rocketnet_uring_send_queues_destroy( walk.queues );
            return 1;
        }
    }

    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            for ( int s = 0; s <= BANK_SLOTS; s++ )
            {
                walk.owner[q][b][s] = -1;
            }
        }
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", bank_record( walk ) ) );

    for ( int step = 0; step < steps; step++ )
    {
        // the enabled actions, as the model's guards say
        std::vector<int> can_start;
        std::vector<int> can_finish;
        bool all_free = true;
        bool all_done = true;
        for ( int t = 1; t <= BANK_THREADS; t++ )
        {
            if ( walk.claim[t].held )
            {
                can_finish.push_back( t );
                all_free = false;
            }
            else if ( walk.demand[t] < BANK_DEMAND )
            {
                can_start.push_back( t );
            }
            if ( walk.demand[t] < BANK_DEMAND )
            {
                all_done = false;
            }
        }
        // a flush before the threads have spent their demand is offered one
        // step in eight, so most fills stage on every queue and the walk
        // reuses each bank (the plant's condition); the model's bound on
        // flushes is the walk's length, which no walk reaches
        const bool can_flush = all_free && walk.flushes < steps && ( all_done || random.below( 8 ) == 0 );

        const int choices = (int) can_start.size() + (int) can_finish.size() + ( can_flush ? 1 : 0 );
        if ( choices == 0 )
        {
            break;
        }

        int pick = random.below( choices );
        if ( can_flush && all_done )
        {
            pick = choices - 1;
        }

        if ( pick < (int) can_start.size() )
        {
            const int t = can_start[(size_t) pick];
            std::string args;
            const char * act = bank_start( walk, t, BANK_FIRST_DEST + random.below( BANK_DESTS ), args );
            report.act( act );
            records.push_back( tla_step( "start", act, args, bank_record( walk ) ) );
        }
        else if ( pick < (int) ( can_start.size() + can_finish.size() ) )
        {
            const int t = can_finish[(size_t) pick - can_start.size()];
            const int k = random.below( 2 * BANK_MAX_SKEW + 1 ) - BANK_MAX_SKEW;
            const bool abort = random.below( 4 ) == 0;
            const char * act = bank_finish( walk, t, k, abort, 1 + random.below( ROCKETNET_RECEIVE_CEILING ) );
            report.act( act );
            records.push_back( tla_step( abort ? "abort" : "finish", act, tla_sequence( { tla_int( t ), tla_int( k ), tla_bool( !abort ) } ), bank_record( walk ) ) );
        }
        else
        {
            int turned = 0;
            const bool stale = bank_flush( walk, &turned );
            report.act( "Flush" );
            records.push_back( tla_step( "turn", "Flush", tla_sequence( { tla_int( turned ) } ), bank_record( walk ) ) );
            if ( stale )
            {
                report.planted( (int) records.size() );
            }
        }
    }

    // the model's constants, read from the code's own structures and definitions
    const int num_queues = walk.queues->num_queues;
    const int slots = walk.queues->slots;
    rocketnet_uring_send_queues_destroy( walk.queues );

    report.print();

    std::string threads;
    for ( int t = 1; t <= BANK_THREADS; t++ )
    {
        threads += ( t > 1 ? ", " : "" ) + tla_int( t );
    }
    std::string dests;
    for ( int d = BANK_FIRST_DEST; d < BANK_FIRST_DEST + BANK_DESTS; d++ )
    {
        dests += ( d > BANK_FIRST_DEST ? ", " : "" ) + tla_int( d );
    }

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    Q = %d\n    NB = %d\n    S = %d\n    Stride = %d\n    Threads = {%s}\n    Dests = {%s}\n"
              "    Demand = %d\n    MaxFlush = %d\n    MaxSkew = %d\n"
              "    ClaimCmp = \"ge\"\n    CounterWrap = 0\n    SlotRule = \"check\"\n    IdRule = \"mul\"\n",
              num_queues, ROCKETNET_URING_SEND_BANKS, slots, ROCKETNET_FRAME_STRIDE, threads.c_str(), dests.c_str(), BANK_DEMAND, steps, BANK_MAX_SKEW );

    return tla_write_trace( out, "MCTraceBank", seed, "TraceRNUringQueueBankLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "bank", trace_bank );
