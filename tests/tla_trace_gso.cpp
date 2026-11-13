/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// layer L2 of the gso push: the per-queue send staging
// (proofs/tla/RNGsoQueueBankLayout.tla), checked by
// proofs/tla/TraceRNGsoQueueBankLayout.tla (README-TraceCheck.md).
//
//   tla_trace --layer gso --seed N --out DIR [--steps K]
//
// writes DIR/MCTraceGsoSeed<seed>.tla and .cfg (MCTraceGsoPlantSeed<seed>
// from tla_trace_plant_gso). The walk drives the library's start, finish,
// abort, send_packets (publish), take and return on the
// rocketnet_server_gso_test_open_slots fixture with two queues: no socket, no
// thread, no clock. Each producer bank hands out three slots (the fixture's
// send_slots, which the walk writes as S), so a fill runs past a bank and each
// record stays small; every other constant is the code's (NB is
// ROCKETNET_GSO_TX_BANKS, Stride ROCKETNET_FRAME_STRIDE). The walk reads the
// model's variables from the lanes as the library holds them
// (rocketnet_server_gso_test_send_lane).
//
// One record per step (tla_step):
//   load              rocketnet_server_gso_send_queue, then the lane's producer
//                     and counter as start reads them      Load(t, d, q)
//   start_packet      rocketnet_server_gso_start_packet    Claim(t, r)
//   reload            a counter another thread moved since the load: the
//                     compare-and-swap that lost, which compare_exchange_weak
//                     reloads inside the one call; the walk calls nothing and
//                     reads the counter again              Claim(t, None)
//   finish_packet     rocketnet_server_gso_finish_packet with the pointer k
//                     bytes off the one start returned     Finish(t, k)
//   finish_abort      finish_packet at the exact pointer (so the abort's 0
//                     shows), then rocketnet_server_gso_abort_packet k bytes
//                     off                                  Finish(t, k)
//   send_packets      rocketnet_server_gso_send_packets    Flush
//   take_return_send  the queue worker's take and return  WorkerReturn(q, b)
// What a call returns is an argument of its action: the queue the hash gave
// (q), the pointer and id start returned (r, None for NULL), the bank the
// worker took (b). The slot a finish or abort wrote is found by comparing
// every slot's packet_bytes before and after the call.
//
// The plant (ROCKETNET_GSO_SEND_SLOT_PLANT, in rocketnet_server_gso_send_slot)
// divides the slot out of the pointer's offset before any test (SlotRule
// "divide"): a pointer a byte early on slot s writes slot s - 1, another
// claim's. Its effect is a finish or abort that wrote a slot not its own,
// reported at that record; the clean build's count is 0. The walk makes sure
// of the condition once: the first finish of a claim on a slot past 0 is a
// byte early.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_GSO_SEND_SLOT_PLANT 1
#include "../sdk/rocketnet_server_gso_linux.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_DEV

#include "../sdk/rocketnet_server_gso.h"
#include "../sdk/rocketnet_constants.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the instance: two queues of the code's banks, each handing out three slots,
// two game threads making four starts each per frame so claims run past a
// bank, and three destinations: destination d is an address the code's hash
// puts on queue d % 2 (the model's QueueOf), so two on queue 1 and one on
// queue 0. Flushes are bounded by the walk's length, so the walk takes a step
// at every one of its steps.
#define GSO_QUEUES 2
#define GSO_SLOTS 3
#define GSO_BANKS ROCKETNET_GSO_TX_BANKS
#define GSO_THREADS 2
#define GSO_DESTS 3
#define GSO_DEMAND 4
// a finish's pointer is -GSO_SKEW_SPAN..GSO_SKEW_SPAN bytes off the one start
// returned (the model's SkewSpan)
#define GSO_SKEW_SPAN 1

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
    int id_queue;                   // the queue the code says the id names
    int64_t ptr;
    uint64_t pid;
};

struct gso_walk_t
{
    struct rocketnet_server_gso_t * gso;
    struct rocketnet_server_gso_test_send_lane_t lane[GSO_QUEUES];     // read after every call
    struct rocketnet_address_t dest[GSO_DESTS + 1];                     // destinations 1..GSO_DESTS
    gso_load_t load[GSO_THREADS + 1];                                   // threads 1..GSO_THREADS
    gso_claim_t claim[GSO_THREADS + 1];
    int owner[GSO_QUEUES][GSO_BANKS][GSO_SLOTS + 1];
    int demand[GSO_THREADS + 1];
    int flushes;
    int max_flush;
    bool oob;
    bool shared;
    bool foreign;
    bool route_bad;
    bool worker_write;
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
    int steps_lost;         // claims whose library compare-and-swap lost to a second claimer the walk ran inside start_packet
    int own_missed;         // exact finishes that did not write their own slot
    std::vector<tla_record_t> * records;    // the walk's records, so a contested claim can add the steps inside its call
    tla_report_t * report;
    tla_random_t * random;
};

// the lanes as the library holds them
static bool gso_read( gso_walk_t & walk )
{
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        if ( !rocketnet_server_gso_test_send_lane( walk.gso, q, &walk.lane[q] ) )
        {
            return false;
        }
    }
    return true;
}

// RNGsoQueueBankLayout Addr(q, b): the model lays the banks end to end
static int64_t gso_addr( int q, int b )
{
    return (int64_t) ( q * GSO_BANKS + b ) * GSO_SLOTS * ROCKETNET_FRAME_STRIDE;
}

// the bank a pointer the code returned lies in, and its byte offset there
static bool gso_locate( const gso_walk_t & walk, const uint8_t * pointer, int * q_out, int * b_out, int64_t * offset_out )
{
    const uintptr_t address = (uintptr_t) pointer;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < GSO_BANKS; b++ )
        {
            const uintptr_t first = (uintptr_t) walk.lane[q].first[b];
            if ( address >= first && address - first < walk.lane[q].bank_bytes )
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
static std::string gso_own( const struct rocketnet_server_gso_test_send_lane_t & lane, int b )
{
    if ( b == lane.producer )
    {
        return "\"prod\"";
    }
    for ( int i = 0; i < lane.free_count; i++ )
    {
        if ( lane.free_banks[( lane.free_head + i ) % GSO_BANKS] == b )
        {
            return "\"free\"";
        }
    }
    return "\"worker\"";
}

// the next finish's packet_bytes, 1..ROCKETNET_RECEIVE_CEILING: distinct for a
// walk's first ROCKETNET_RECEIVE_CEILING finishes (a 400-step walk makes 800 at
// most), so a write is seen whatever the slot held
static int gso_bytes( gso_walk_t & walk )
{
    return 1 + walk.finishes++ % ROCKETNET_RECEIVE_CEILING;
}

static std::string gso_none()
{
    return "[none |-> TRUE]";
}

static tla_record_t gso_state( const gso_walk_t & walk )
{
    std::vector<std::string> queue_keys;
    std::vector<std::string> prod;
    std::vector<std::string> own;
    std::vector<std::string> cnt;
    std::vector<std::string> turn;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        const struct rocketnet_server_gso_test_send_lane_t & lane = walk.lane[q];
        queue_keys.push_back( tla_int( q ) );
        prod.push_back( tla_int( lane.producer ) );
        turn.push_back( tla_int( (int64_t) lane.turn ) );
        std::vector<std::string> bank_keys;
        std::vector<std::string> owners;
        std::vector<std::string> counts;
        for ( int b = 0; b < GSO_BANKS; b++ )
        {
            bank_keys.push_back( tla_int( b ) );
            owners.push_back( gso_own( lane, b ) );
            counts.push_back( tla_int( lane.num_packets[b] ) );
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
            load.push_back( gso_none() );
        }
        else
        {
            load.push_back( "[q |-> " + tla_int( l.q ) + ", b |-> " + tla_int( l.b ) + ", i |-> " + tla_int( l.i ) + "]" );
        }
        const gso_claim_t & c = walk.claim[t];
        if ( !c.held )
        {
            held.push_back( gso_none() );
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
        for ( int b = 0; b < GSO_BANKS; b++ )
        {
            for ( int s = 0; s <= GSO_SLOTS; s++ )
            {
                slot_keys.push_back( "<<" + tla_int( q ) + ", " + tla_int( b ) + ", " + tla_int( s ) + ">>" );
                owner.push_back( tla_int( walk.owner[q][b][s] ) );
            }
        }
    }

    tla_record_t state;
    state.add( "prod", tla_function( queue_keys, prod ) );
    state.add( "own", tla_function( queue_keys, own ) );
    state.add( "cnt", tla_function( queue_keys, cnt ) );
    state.add( "turn", tla_function( queue_keys, turn ) );
    state.add( "load", tla_function( thread_keys, load ) );
    state.add( "held", tla_function( thread_keys, held ) );
    state.add( "owner", tla_function( slot_keys, owner ) );
    state.add( "demand", tla_function( thread_keys, demand ) );
    state.add( "flushes", tla_int( walk.flushes ) );
    state.add( "oob", tla_bool( walk.oob ) );
    state.add( "shared", tla_bool( walk.shared ) );
    state.add( "foreign", tla_bool( walk.foreign ) );
    state.add( "routeBad", tla_bool( walk.route_bad ) );
    state.add( "workerWrite", tla_bool( walk.worker_write ) );
    return state;
}

static std::string gso_args( const std::vector<std::string> & args )
{
    return tla_sequence( args );
}

// RNGsoQueueBankLayout Load(t, d, q): the first half of a start, the queue the
// code's hash returns and that queue's producer and counter as the lane holds
// them
static std::string gso_load( gso_walk_t & walk, int t, int d )
{
    const int q = rocketnet_server_gso_send_queue( &walk.dest[d], GSO_QUEUES );
    gso_load_t & l = walk.load[t];
    l.loaded = true;
    l.d = d;
    l.q = q;
    l.b = walk.lane[q].producer;
    l.i = walk.lane[q].producer >= 0 && walk.lane[q].producer < GSO_BANKS ? walk.lane[q].num_packets[walk.lane[q].producer] : -1;
    walk.demand[t]++;
    walk.steps_load++;
    return gso_args( { tla_int( t ), tla_int( d ), tla_int( q ) } );
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
        c.id_queue = -1;
        walk.oob = true;
        return;
    }

    // the slot and the queue the id names are the code's own answers
    // (rocketnet_server_gso_send_slot, the id's low part), not a test copy of
    // the rule; a refusal there leaves the pointer's own geometry for the claim
    int id_queue = 0;
    const int code_slot = rocketnet_server_gso_test_send_slot( walk.gso, pid, frame, &id_queue );
    c.q = q;
    c.b = b;
    c.slot = code_slot >= 0 ? code_slot : (int) ( offset / ROCKETNET_FRAME_STRIDE );
    c.id_queue = id_queue;
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
    if ( b != walk.lane[q].producer )
    {
        walk.worker_write = true;
    }
}

// RNGsoQueueBankLayout Claim(t, r): the second half. A counter another
// thread moved since the load is the compare-and-swap that lost: op reload,
// no call, r = None. Otherwise the code's start is called; r is what it
// returned, the pointer (as the model addresses it) and id, or None for NULL.
// the second claimer the walk runs inside the library's start_packet, between
// its load of the producer's count and its compare-and-swap
// (rocketnet_server_gso_test_set_claim_hook): thread u's whole claim, recorded
// as the model's Claim(u) before the first thread's compare-and-swap is tried
struct gso_hook_t
{
    gso_walk_t * walk;
    int u;
    bool fired;
    bool refused;
    struct rocketnet_server_gso_test_send_lane_t snap[GSO_QUEUES];
};

static void gso_claim_hook( void * user, int )
{
    gso_hook_t * h = (gso_hook_t *) user;
    gso_walk_t & walk = *h->walk;
    rocketnet_server_gso_test_set_claim_hook( walk.gso, NULL, NULL );
    h->fired = true;
    gso_load_t & lu = walk.load[h->u];
    uint64_t pid = 0;
    const uint8_t * frame = rocketnet_server_gso_start_packet( walk.gso, &walk.dest[lu.d], &pid );
    lu.loaded = false;
    if ( !frame )
    {
        h->refused = true;
        return;
    }
    gso_read( walk );
    gso_take_claim( walk, h->u, frame, pid );
    walk.steps_claim++;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        h->snap[q] = walk.lane[q];
    }
    const gso_claim_t & c = walk.claim[h->u];
    walk.report->act( "Claim" );
    walk.records->push_back( tla_step( "start_packet", "Claim",
                                       gso_args( { tla_int( h->u ), "[ptr |-> " + tla_int( c.ptr ) + ", pid |-> " + tla_int( (int64_t) pid ) + "]" } ),
                                       gso_state( walk ) ) );
}

static std::string gso_claim( gso_walk_t & walk, int t, const char ** op, const std::vector<int> & others )
{
    gso_load_t & l = walk.load[t];
    const struct rocketnet_server_gso_test_send_lane_t & lane = walk.lane[l.q];

    if ( l.i < GSO_SLOTS && lane.num_packets[l.b] != l.i )
    {
        l.b = lane.producer;
        l.i = lane.num_packets[lane.producer];
        walk.steps_reload++;
        *op = "reload";
        return gso_args( { tla_int( t ), gso_none() } );
    }

    // another thread that loaded the same counter claims inside this call: the
    // library's compare-and-swap then loses and reloads on its own, which the
    // trace records as the model's reload step; a library that stored blindly
    // (no loss) takes the same slot, and the state it records no model step reaches
    gso_hook_t hook = {};
    int loss_before = 0;
    if ( walk.records && l.i >= 0 && l.i + 1 < GSO_SLOTS && lane.num_packets[l.b] == l.i )
    {
        for ( size_t k = 0; k < others.size(); k++ )
        {
            const int u = others[k];
            const gso_load_t & lu = walk.load[u];
            if ( u != t && lu.q == l.q && lu.b == l.b && lu.i == l.i && walk.random->below( 2 ) == 0 )
            {
                hook.walk = &walk;
                hook.u = u;
                loss_before = rocketnet_server_gso_test_claim_losses( walk.gso );
                rocketnet_server_gso_test_set_claim_hook( walk.gso, gso_claim_hook, &hook );
                break;
            }
        }
    }

    uint64_t pid = 0;
    const uint8_t * frame = rocketnet_server_gso_start_packet( walk.gso, &walk.dest[l.d], &pid );
    rocketnet_server_gso_test_set_claim_hook( walk.gso, NULL, NULL );
    l.loaded = false;
    if ( hook.fired && !hook.refused && rocketnet_server_gso_test_claim_losses( walk.gso ) > loss_before )
    {
        // the library's own loop lost and reloaded: the model's reload step,
        // on the lanes as the second claimer left them
        for ( int q = 0; q < GSO_QUEUES; q++ )
        {
            walk.lane[q] = hook.snap[q];
        }
        const int reload_b = walk.lane[l.q].producer;
        const int reload_i = walk.lane[l.q].num_packets[reload_b];
        walk.load[t].b = reload_b;
        walk.load[t].i = reload_i;
        walk.load[t].loaded = true;
        walk.steps_reload++;
        walk.steps_lost++;
        walk.report->act( "Claim" );
        walk.records->push_back( tla_step( "reload", "Claim", gso_args( { tla_int( t ), gso_none() } ), gso_state( walk ) ) );
        walk.load[t].loaded = false;
    }
    *op = "start_packet";
    if ( !frame )
    {
        walk.steps_refused++;
        return gso_args( { tla_int( t ), gso_none() } );
    }
    // the lanes as they were when start returned, to locate the pointer
    gso_read( walk );
    gso_take_claim( walk, t, frame, pid );
    walk.steps_claim++;
    const gso_claim_t & c = walk.claim[t];
    return gso_args( { tla_int( t ), "[ptr |-> " + tla_int( c.ptr ) + ", pid |-> " + tla_int( (int64_t) pid ) + "]" } );
}

// every slot's packet_bytes, in queue, bank, slot order
static std::vector<int> gso_packet_bytes( const gso_walk_t & walk )
{
    std::vector<int> bytes;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < GSO_BANKS; b++ )
        {
            for ( int s = 0; s < GSO_SLOTS; s++ )
            {
                bytes.push_back( rocketnet_server_gso_test_send_packet_bytes( walk.gso, q, b, s ) );
            }
        }
    }
    return bytes;
}

// RNGsoQueueBankLayout Finish(t, k): the code's finish or abort with the
// pointer k bytes off the one start returned; the slot it wrote is found by
// comparing every slot's packet_bytes before and after. Returns the finishes
// and aborts that wrote a slot not their own: the plant firing.
static int gso_finish( gso_walk_t & walk, int t, int k, bool abort )
{
    gso_claim_t & c = walk.claim[t];
    struct rocketnet_server_gso_t * gso = walk.gso;

    const int64_t offset = c.ptr - gso_addr( c.q, c.b );
    uint8_t * exact = (uint8_t *) ( (uintptr_t) walk.lane[c.q].first[c.b] + (uintptr_t) offset );
    uint8_t * pointer = (uint8_t *) ( (uintptr_t) exact + (uintptr_t) (intptr_t) k );

    if ( abort )
    {
        // an abort writes 0, which an unfinished slot already holds: finish the
        // claim's own slot through the exact pointer first so the abort shows
        rocketnet_server_gso_finish_packet( gso, c.pid, exact, gso_bytes( walk ) );
    }

    const std::vector<int> before = gso_packet_bytes( walk );

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

    const std::vector<int> after = gso_packet_bytes( walk );
    int foreign_writes = 0;
    size_t index = 0;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < GSO_BANKS; b++ )
        {
            for ( int s = 0; s < GSO_SLOTS; s++ )
            {
                if ( after[index] != before[index] )
                {
                    if ( q != c.q || b != c.b || s != c.slot )
                    {
                        foreign_writes++;
                    }
                    if ( walk.owner[q][b][s] != t || q != c.q || b != c.b || s != c.slot )
                    {
                        walk.foreign = true;
                    }
                    if ( b != walk.lane[q].producer )
                    {
                        walk.worker_write = true;
                    }
                }
                index++;
            }
        }
    }

    if ( c.id_queue != c.q )
    {
        walk.route_bad = true;
    }

    // a finish through the exact pointer writes its own slot: the claim's slot
    // changed from what it held before the call
    if ( k == 0 && !abort && c.slot >= 0 && c.slot < GSO_SLOTS && c.q >= 0 && c.b >= 0 )
    {
        const size_t own = (size_t) ( ( c.q * GSO_BANKS + c.b ) * GSO_SLOTS + c.slot );
        if ( after[own] == before[own] )
        {
            walk.own_missed++;
        }
    }

    c.held = false;
    return foreign_writes;
}

// RNGsoQueueBankLayout Flush: the code's send_packets with the game threads joined;
// each queue that turned over starts its new producer with no slot handed out
static void gso_flush( gso_walk_t & walk )
{
    int before[GSO_QUEUES];
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        before[q] = walk.lane[q].producer;
    }
    rocketnet_server_gso_send_packets( walk.gso );
    gso_read( walk );
    walk.steps_flush++;
    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        if ( walk.lane[q].producer == before[q] )
        {
            if ( before[q] >= 0 && before[q] < GSO_BANKS && walk.lane[q].num_packets[before[q]] != 0 )
            {
                walk.steps_kept++;
            }
            continue;
        }
        walk.steps_turn++;
        for ( int b = 0; b < GSO_BANKS; b++ )
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
// layer L3's, RNGsoHandoff); b is the bank take returned
static std::string gso_return( gso_walk_t & walk, int q )
{
    bool final = false;
    const int index = rocketnet_server_gso_test_take_send( walk.gso, q, &final );
    if ( index >= 0 && !final )
    {
        rocketnet_server_gso_test_return_send( walk.gso, q, index );
        walk.steps_return++;
    }
    return gso_args( { tla_int( q ), tla_int( final ? -1 : index ) } );
}

static int trace_gso( uint64_t seed, int steps, const char * out )
{
    static gso_walk_t walk;
    memset( &walk, 0, sizeof( walk ) );
    walk.max_flush = steps;
    walk.gso = rocketnet_server_gso_test_open_slots( GSO_QUEUES, GSO_SLOTS );
    if ( !walk.gso )
    {
        fprintf( stderr, "tla_trace: gso: the fixture could not be opened\n" );
        return 1;
    }

    // destinations: destination d on queue d % GSO_QUEUES, by the code's hash
    int d = 1;
    for ( int port = 40000; port < 41000 && d <= GSO_DESTS; port++ )
    {
        struct rocketnet_address_t a;
        memset( &a, 0, sizeof( a ) );
        a.type = ROCKETNET_ADDRESS_IPV4;
        a.data.ipv4[0] = 10;
        a.data.ipv4[3] = 9;
        a.port = (uint16_t) port;
        if ( rocketnet_server_gso_send_queue( &a, GSO_QUEUES ) == d % GSO_QUEUES )
        {
            walk.dest[d++] = a;
        }
    }
    if ( d <= GSO_DESTS || !gso_read( walk ) )
    {
        fprintf( stderr, "tla_trace: gso: no destination for every queue, or the lanes could not be read\n" );
        rocketnet_server_gso_test_close( walk.gso );
        return 1;
    }

    for ( int q = 0; q < GSO_QUEUES; q++ )
    {
        for ( int b = 0; b < GSO_BANKS; b++ )
        {
            for ( int s = 0; s <= GSO_SLOTS; s++ )
            {
                walk.owner[q][b][s] = -1;
            }
        }
    }

    tla_random_t random = { seed };
    tla_report_t report;
    std::vector<tla_record_t> records;
    walk.records = &records;
    walk.report = &report;
    walk.random = &random;
    records.push_back( tla_step( "init", "init", "<<>>", gso_state( walk ) ) );

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
            if ( offer_return && walk.lane[q].ready_count > 0 )
            {
                can_return.push_back( q );
            }
        }
        // a flush before the threads have spent their demand is offered one
        // step in eight, so most fills run past a bank and stage on both queues
        const bool can_flush = all_free && walk.flushes < walk.max_flush && ( all_done || random.below( 8 ) == 0 );

        const int n_load = (int) can_load.size();
        const int n_claim = (int) can_claim.size();
        const int n_finish = (int) can_finish.size();
        const int n_return = (int) can_return.size();
        const int choices = n_load + n_claim + n_finish + n_return + ( can_flush ? 1 : 0 );
        if ( choices == 0 )
        {
            fprintf( stderr, "tla_trace: gso: no step enabled at step %d\n", step );
            break;
        }

        const int pick = random.below( choices );
        const char * op = NULL;
        const char * act = NULL;
        std::string args;
        int fired = 0;
        if ( pick < n_load )
        {
            args = gso_load( walk, can_load[(size_t) pick], 1 + random.below( GSO_DESTS ) );
            op = "load";
            act = "Load";
        }
        else if ( pick < n_load + n_claim )
        {
            args = gso_claim( walk, can_claim[(size_t) ( pick - n_load )], &op, can_claim );
            act = "Claim";
        }
        else if ( pick < n_load + n_claim + n_finish )
        {
            const int t = can_finish[(size_t) ( pick - n_load - n_claim )];
            int k = random.below( 2 * GSO_SKEW_SPAN + 1 ) - GSO_SKEW_SPAN;
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
            fired = gso_finish( walk, t, k, abort );
            op = abort ? "finish_abort" : "finish_packet";
            act = "Finish";
            args = gso_args( { tla_int( t ), tla_int( k ) } );
        }
        else if ( pick < n_load + n_claim + n_finish + n_return )
        {
            args = gso_return( walk, can_return[(size_t) ( pick - n_load - n_claim - n_finish )] );
            op = "take_return_send";
            act = "WorkerReturn";
        }
        else
        {
            gso_flush( walk );
            walk.worker_slow = random.below( 2 ) == 0;
            op = "send_packets";
            act = "Flush";
            args = "<<>>";
        }

        if ( !gso_read( walk ) )
        {
            fprintf( stderr, "tla_trace: gso: the lanes could not be read\n" );
            break;
        }
        report.act( act );
        records.push_back( tla_step( op, act, args, gso_state( walk ) ) );
        // the planted effect, where it shows: a finish or abort wrote a slot
        // not its own
        if ( fired > 0 )
        {
            report.planted( (int) records.size() );
        }
    }

    rocketnet_server_gso_test_close( walk.gso );

    printf( "WALK records=%d loads=%d claims=%d reloads=%d refused=%d finishes=%d aborts=%d flushes=%d turns=%d kept=%d returns=%d\n",
            (int) records.size(), walk.steps_load, walk.steps_claim, walk.steps_reload, walk.steps_refused, walk.steps_finish,
            walk.steps_abort, walk.steps_flush, walk.steps_turn, walk.steps_kept, walk.steps_return );
    fprintf( stderr, "tla_trace: gso: %d claims lost their compare-and-swap inside start_packet to a second claimer (library loop), %d exact finishes missed their own slot\n", walk.steps_lost, walk.own_missed );
    report.print();

    std::string threads = "{";
    for ( int t = 1; t <= GSO_THREADS; t++ )
    {
        threads += ( t > 1 ? ", " : "" ) + tla_int( t );
    }
    threads += "}";
    std::string dests = "{";
    for ( int i = 1; i <= GSO_DESTS; i++ )
    {
        dests += ( i > 1 ? ", " : "" ) + tla_int( i );
    }
    dests += "}";

    const std::string constants = "    Q = " + tla_int( GSO_QUEUES ) + "\n" + "    NB = " + tla_int( ROCKETNET_GSO_TX_BANKS ) + "\n" + "    S = " +
                                  tla_int( walk.lane[0].slots ) + "\n" + "    Stride = " + tla_int( (int64_t) ROCKETNET_FRAME_STRIDE ) + "\n" +
                                  "    Threads = " + threads + "\n" + "    Dests = " + dests + "\n" + "    Demand = " + tla_int( GSO_DEMAND ) + "\n" +
                                  "    MaxFlush = " + tla_int( walk.max_flush ) + "\n" + "    SkewSpan = " + tla_int( GSO_SKEW_SPAN ) + "\n" +
                                  "    ClaimCmp = \"ge\"\n    ClaimRule = \"cas\"\n    SlotRule = \"check\"\n    IdRule = \"mul\"\n"
                                  "    FlushRule = \"joined\"\n";

    if ( walk.own_missed > 0 )
    {
        fprintf( stderr, "tla_trace: gso: a finish through the exact pointer did not write its own slot\n" );
        return 1;
    }

    return tla_write_trace( out, "MCTraceGso", seed, "TraceRNGsoQueueBankLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "gso", trace_gso );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_DEV
