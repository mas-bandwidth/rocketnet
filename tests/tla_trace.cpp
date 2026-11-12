/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace harness of the TLA+ trace check (proofs/tla/README-TraceCheck.md).
// A seeded random walk drives the code of one layer through the actions its
// model names, and after every step writes one record of the model's
// variables, read from the code's own state and outputs. The records go out
// as a TLA+ module that defines the trace, with a cfg that hands it to the
// layer's Trace<Spec>.tla; TLC then checks the trace is a behaviour of the
// spec. No socket, no clock, no thread: the walk interleaves the fill
// threads' calls itself, one call per step.
//
//   tla_trace --layer bank --seed N --out DIR [--steps K]
//
// writes DIR/MCTraceBank_<seed>.tla and .cfg (MCTraceBankPlant_<seed> from the
// plant build). The plant build (tla_trace_plant) compiles the layer's code
// with its plant define: the walk is the same, and its trace must fail
// TraceConsistent.

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

// ---------------------------------------------------------------------------
// the record writer: TLA+ values as text
// ---------------------------------------------------------------------------

static std::string tla_int( int64_t value )
{
    char text[32];
    snprintf( text, sizeof( text ), "%" PRId64, value );
    return text;
}

static std::string tla_bool( bool value )
{
    return value ? "TRUE" : "FALSE";
}

// a function over 0..n-1 (or base..base+n-1): k1 :> v1 @@ k2 :> v2 ...
static std::string tla_function( const std::vector<std::string> & keys, const std::vector<std::string> & values )
{
    std::string text = "(";
    for ( size_t i = 0; i < keys.size(); i++ )
    {
        if ( i > 0 )
        {
            text += " @@ ";
        }
        text += keys[i] + " :> " + values[i];
    }
    text += ")";
    return text;
}

struct tla_record_t
{
    std::vector<std::string> fields;
    std::vector<std::string> values;

    void add( const char * field, const std::string & value )
    {
        fields.push_back( field );
        values.push_back( value );
    }

    std::string text() const
    {
        std::string out = "[";
        for ( size_t i = 0; i < fields.size(); i++ )
        {
            if ( i > 0 )
            {
                out += ", ";
            }
            out += fields[i] + " |-> " + values[i];
        }
        out += "]";
        return out;
    }
};

// the deterministic generator of the walk: splitmix64
struct tla_random_t
{
    uint64_t state;

    uint64_t next()
    {
        uint64_t z = ( state += 0x9E3779B97F4A7C15ULL );
        z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
        z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBULL;
        return z ^ ( z >> 31 );
    }

    int below( int n )
    {
        return (int) ( next() % (uint64_t) n );
    }
};

// writes MODULE <name> with the constants, definitions and trace, and its cfg
static bool tla_write_trace( const char * dir, const std::string & name, const char * extends, const std::string & definitions,
                             const std::string & constants, const std::vector<tla_record_t> & records )
{
    const std::string module_path = std::string( dir ) + "/" + name + ".tla";
    const std::string cfg_path = std::string( dir ) + "/" + name + ".cfg";

    FILE * module = fopen( module_path.c_str(), "w" );
    if ( !module )
    {
        fprintf( stderr, "tla_trace: could not write %s\n", module_path.c_str() );
        return false;
    }

    std::string header = "---- MODULE " + name + " ----";
    fprintf( module, "%s\n", header.c_str() );
    fprintf( module, "\\* written by tests/tla_trace.cpp: %d records of the code's state, one per step\n", (int) records.size() );
    fprintf( module, "EXTENDS %s, TLC\n\n", extends );
    fprintf( module, "%s\n", definitions.c_str() );
    fprintf( module, "Log == <<\n" );
    for ( size_t i = 0; i < records.size(); i++ )
    {
        fprintf( module, "    %s%s\n", records[i].text().c_str(), i + 1 < records.size() ? "," : "" );
    }
    fprintf( module, ">>\n\n====\n" );
    fclose( module );

    FILE * cfg = fopen( cfg_path.c_str(), "w" );
    if ( !cfg )
    {
        fprintf( stderr, "tla_trace: could not write %s\n", cfg_path.c_str() );
        return false;
    }

    fprintf( cfg, "\\* written by tests/tla_trace.cpp: the trace must be a behaviour of the spec\n" );
    fprintf( cfg, "SPECIFICATION TraceSpec\n\nCONSTANTS\n%s    TraceLog <- Log\n\nINVARIANT TraceConsistent\n", constants.c_str() );
    fclose( cfg );

    printf( "TRACE %s records=%d\n", module_path.c_str(), (int) records.size() );
    return true;
}

// ---------------------------------------------------------------------------
// layer L0: the per-queue send banks (proofs/tla/RNUringQueueBankLayout.tla)
// ---------------------------------------------------------------------------

// the instance: small enough for TLC to check each trace in seconds, with
// claims past a bank's slots and every bank of a queue reused
#define BANK_QUEUES 2
#define BANK_SLOTS 3
#define BANK_THREADS 2
#define BANK_DESTS 3
#define BANK_DEMAND 4
#define BANK_MAX_FLUSH 4

static const int bank_skew[] = { -1, 0, 1 };

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
    struct rocketnet_address_t dest[BANK_DESTS + 1];     // destinations 1..BANK_DESTS
    bank_claim_t claim[BANK_THREADS + 1];               // threads 1..BANK_THREADS
    int owner[BANK_QUEUES][ROCKETNET_URING_SEND_BANKS][BANK_SLOTS + 1];
    int demand[BANK_THREADS + 1];
    int flushes;
    bool oob;
    bool shared;
    bool foreign;
    bool route_bad;
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

    tla_record_t record;
    record.add( "prod", tla_function( queue_keys, prod ) );
    record.add( "cnt", tla_function( queue_keys, cnt ) );
    record.add( "gid", tla_int( (int64_t) queues->packet_id.load() ) );
    record.add( "held", tla_function( thread_keys, held ) );
    record.add( "owner", tla_function( slot_keys, owner ) );
    record.add( "demand", tla_function( thread_keys, demand ) );
    record.add( "flushes", tla_int( walk.flushes ) );
    record.add( "oob", tla_bool( walk.oob ) );
    record.add( "shared", tla_bool( walk.shared ) );
    record.add( "foreign", tla_bool( walk.foreign ) );
    record.add( "routeBad", tla_bool( walk.route_bad ) );
    return record;
}

// RNUringQueueBankLayout Start(t, d): the code's start, read back as the
// model's claim: the queue, bank and slot are where the returned pointer lies
static void bank_start( bank_walk_t & walk, int t, int d )
{
    uint64_t pid = 0;
    uint8_t * frame = rocketnet_uring_send_queues_start( walk.queues, &walk.dest[d], &pid );
    walk.demand[t]++;
    if ( !frame )
    {
        return;
    }

    const int64_t ptr = bank_offset( walk, frame );
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
        return;
    }

    if ( walk.owner[c.q][c.b][c.slot] != -1 )
    {
        walk.shared = true;
    }
    walk.owner[c.q][c.b][c.slot] = t;
}

// RNUringQueueBankLayout Finish(t, k): the code's finish or abort with the
// pointer k bytes off; the slot it wrote is found by comparing every slot
// record before and after
static void bank_finish( bank_walk_t & walk, int t, int k, bool abort, int packet_bytes )
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
    if ( abort )
    {
        // an abort of a finished packet writes 0 over packet_bytes: make the
        // claim's slot visible first by finishing it through the right pointer
        (void) rocketnet_uring_send_queues_finish( queues, c.pid, queues->queue[0].bank[0].data + c.ptr, packet_bytes );
        before[(size_t) ( ( c.q * ROCKETNET_URING_SEND_BANKS + c.b ) * BANK_SLOTS + c.slot )].packet_bytes = packet_bytes;
        (void) rocketnet_uring_send_queues_abort( queues, c.pid, pointer );
    }
    else
    {
        (void) rocketnet_uring_send_queues_finish( queues, c.pid, pointer, packet_bytes );
    }

    size_t index = 0;
    for ( int q = 0; q < BANK_QUEUES; q++ )
    {
        for ( int b = 0; b < ROCKETNET_URING_SEND_BANKS; b++ )
        {
            for ( int s = 0; s < BANK_SLOTS; s++ )
            {
                const bool written = queues->queue[q].bank[b].slot[s].packet_bytes != before[index].packet_bytes;
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
}

// RNUringQueueBankLayout Flush: the code's turn, the fill joined
static void bank_flush( bank_walk_t & walk )
{
    (void) rocketnet_uring_send_queues_turn( walk.queues );
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
}

static int trace_bank( uint64_t seed, int steps, const char * out )
{
    bank_walk_t walk;
    memset( &walk, 0, sizeof( walk ) );
    walk.queues = rocketnet_uring_send_queues_create( NULL, BANK_QUEUES, BANK_SLOTS );
    if ( !walk.queues )
    {
        return 1;
    }

    // destinations: two on queue 0 and one on queue 1, by the code's hash
    int found[BANK_QUEUES] = { 0, 0 };
    const int want[BANK_QUEUES] = { 2, 1 };
    int d = 1;
    for ( int port = 40000; port < 41000 && d <= BANK_DESTS; port++ )
    {
        struct rocketnet_address_t a;
        memset( &a, 0, sizeof( a ) );
        a.type = ROCKETNET_ADDRESS_IPV4;
        a.data.ipv4[0] = 10;
        a.data.ipv4[3] = 9;
        a.port = (uint16_t) port;
        const int q = rocketnet_send_queue_of( &a, BANK_QUEUES );
        if ( found[q] < want[q] )
        {
            found[q]++;
            walk.dest[d++] = a;
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
    records.push_back( bank_record( walk ) );

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
        const bool can_flush = all_free && walk.flushes < BANK_MAX_FLUSH;

        const int choices = (int) can_start.size() + (int) can_finish.size() + ( can_flush ? 1 : 0 );
        if ( choices == 0 )
        {
            break;
        }

        // a flush is likelier once the threads have spent their demand
        int pick = random.below( choices );
        if ( can_flush && all_done )
        {
            pick = choices - 1;
        }

        if ( pick < (int) can_start.size() )
        {
            bank_start( walk, can_start[(size_t) pick], 1 + random.below( BANK_DESTS ) );
        }
        else if ( pick < (int) ( can_start.size() + can_finish.size() ) )
        {
            const int t = can_finish[(size_t) pick - can_start.size()];
            const int k = bank_skew[random.below( (int) ( sizeof( bank_skew ) / sizeof( bank_skew[0] ) ) )];
            bank_finish( walk, t, k, random.below( 4 ) == 0, 1 + random.below( ROCKETNET_RECEIVE_CEILING ) );
        }
        else
        {
            bank_flush( walk );
        }

        records.push_back( bank_record( walk ) );
    }

    rocketnet_uring_send_queues_destroy( walk.queues );

    std::string queue_of = "MCQueueOf(d) == CASE ";
    for ( int i = 1; i <= BANK_DESTS; i++ )
    {
        if ( i > 1 )
        {
            queue_of += " [] ";
        }
        queue_of += "d = " + tla_int( i ) + " -> " + tla_int( rocketnet_send_queue_of( &walk.dest[i], BANK_QUEUES ) );
    }
    const std::string definitions = queue_of + "\nMCSkew == {-1, 0, 1}\n";

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    Q = %d\n    NB = %d\n    S = %d\n    Stride = %d\n    Threads = {1, 2}\n    Dests = {1, 2, 3}\n"
              "    QueueOf <- MCQueueOf\n    Demand = %d\n    MaxFlush = %d\n    Skew <- MCSkew\n"
              "    ClaimCmp = \"ge\"\n    CounterWrap = 0\n    SlotRule = \"check\"\n    IdRule = \"mul\"\n",
              BANK_QUEUES, ROCKETNET_URING_SEND_BANKS, BANK_SLOTS, ROCKETNET_FRAME_STRIDE, BANK_DEMAND, BANK_MAX_FLUSH );

#if defined( ROCKETNET_URING_SEND_QUEUE_PLANT )
    const std::string name = "MCTraceBankPlant_" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_URING_SEND_QUEUE_PLANT )
    const std::string name = "MCTraceBank_" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_URING_SEND_QUEUE_PLANT )

    return tla_write_trace( out, name, "TraceRNUringQueueBankLayout", definitions, constants, records ) ? 0 : 1;
}

// ---------------------------------------------------------------------------

static void usage()
{
    fprintf( stderr, "usage: tla_trace --layer bank --seed N --out DIR [--steps K]\n" );
}

int main( int argc, char ** argv )
{
    const char * layer = NULL;
    const char * out = NULL;
    uint64_t seed = 0;
    bool have_seed = false;
    int steps = 80;

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--layer" ) == 0 && i + 1 < argc )
        {
            layer = argv[++i];
        }
        else if ( strcmp( argv[i], "--out" ) == 0 && i + 1 < argc )
        {
            out = argv[++i];
        }
        else if ( strcmp( argv[i], "--seed" ) == 0 && i + 1 < argc )
        {
            seed = strtoull( argv[++i], NULL, 10 );
            have_seed = true;
        }
        else if ( strcmp( argv[i], "--steps" ) == 0 && i + 1 < argc )
        {
            steps = atoi( argv[++i] );
        }
        else
        {
            usage();
            return 2;
        }
    }

    if ( !layer || !out || !have_seed || steps < 1 )
    {
        usage();
        return 2;
    }

    if ( strcmp( layer, "bank" ) == 0 )
    {
        return trace_bank( seed, steps, out );
    }

    fprintf( stderr, "tla_trace: no layer %s\n", layer );
    return 2;
}
