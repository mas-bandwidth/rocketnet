/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the deferred ack apply
// (proofs/tla/README-TraceCheck.md, proofs/tla/RNAckApplyPhase.tla,
// proofs/tla/TraceRNAckApplyPhase.tla).
//
// The code under test is the server's acks with the deferral on
// (sdk/rocketnet_ack.cpp, rocketnet_server_acks_t), the state behind the
// update in three calls: a server acks of AA_CLIENTS clients, created and
// deferred as the server transport does it, then its queue lowered to
// AA_PENDING entries and its range store to one block's ranges
// (rocketnet_server_acks_lower_capacity_for_test), so the walk fills both
// and the early gather runs in the code. The main thread is the walk's
// thread. The game threads are AA_GAME_THREADS real threads, because the take
// is the one call the code gives to any thread; the walk hands one call at a
// time to one of them and waits for it, so a seed is one order of whole
// calls. Each step is one call:
//
//   begin_receive  rocketnet_server_acks_begin_receive, while closed:
//                  BeginReceive
//   receive_block  rocketnet_server_acks_receive_block for one client, while
//                  receiving: Receive(c, b, r). The block acknowledges the
//                  client's next sent sequence b, which no block has
//                  acknowledged, and r ranges below it over sequences
//                  acknowledged already, so its apply gives exactly one id,
//                  b. Most blocks carry up to 2 ranges and some
//                  AA_BIG_RANGES, three of which do not fit the range store.
//                  A block that finds the queue or the range store full is
//                  the same call and the same action: the code gathers first
//   settle         rocketnet_server_acks_settle for a client the code says
//                  has blocks queued, while receiving: Settle(c)
//   open           rocketnet_server_acks_open: Open
//   take           rocketnet_server_acks_apply_client for any client, in any
//                  phase, on the main thread or a game thread: Take(t, c, n)
//                  when it returns n > 0 blocks applied; when it returns 0,
//                  TakeRefused(t, c) while the code's phase is open and
//                  TakeOutside(t, c) in any other phase
//   close          rocketnet_server_acks_close, while open, or sometimes
//                  straight from the receive (the code takes both): Close
//
// Every state field but one is read from the code after the call:
//
//   phase      acks->phase
//   queue      acks->pending[0 .. num_pending): each entry's client, block
//              (its largest), ranges, range offset, next, and the offset and
//              count of its ids in its client's segment
//   numRanges  acks->num_ranges
//   head, tail, claim
//              per client, through the seam
//              rocketnet_server_acks_client_for_test (the client struct is
//              the sdk file's own)
//   segment    per client, the ids in its segment (acks->segments), as many
//              as the seam says it holds
//   batch      acks->acked_client_index and acks->acked_packet_ids, the
//              first num_acked
//
// Two variables the code holds no word for are kept by the walk from its
// calls, as the writer's finish layer keeps its touched:
//
//   took       per client, the threads whose take returned blocks applied
//              since the last open: the history OneTaker reads. The code
//              keeps only the claim, which says a client is taken, not by
//              whom
//   arrived    the blocks the walk handed to receive_block since the last
//              begin_receive, in order: the history Whole and Published
//              read. The code's queue forgets it at the gather
//
// The row has no ghost.
//
// The walk fails if a block counts an unknown sequence or a full batch: its
// blocks are made so that none does.
//
// The plant (ROCKETNET_ACK_APPLY_PLANT, the tla_trace_plant_ack_apply
// build): the take reads the claim and leaves it set. PLANT-FIRED counts the
// records where a take returned blocks applied and the client's claim still
// stands, which the clean walk never shows.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_ACK_APPLY_PLANT 1
#include "../sdk/rocketnet_ack.cpp"
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
#include "../sdk/rocketnet_ack.h"
#include "tla_trace_record.h"

void rocketnet_server_acks_client_for_test( const struct rocketnet_server_acks_t * acks, int client_index, int * head, int * tail, int * claim, int * ids );
bool rocketnet_server_acks_lower_capacity_for_test( struct rocketnet_server_acks_t * acks, int pending, int ranges );

#define AA_CLIENTS       3
#define AA_GAME_THREADS  2
#define AA_THREADS       ( 1 + AA_GAME_THREADS ) // the main thread is thread 0
#define AA_PENDING       6                       // the queue's entries, lowered from the code's own
#define AA_SMALL_RANGES  2                       // most blocks carry 0 .. AA_SMALL_RANGES ranges
#define AA_BIG_RANGES    100                     // some carry this many: the third does not fit a store of one block's ranges
#define AA_FIRST_BLOCK   600                     // each client's first block: every sequence below it is acknowledged before the walk
#define AA_SEND_TIME     1000.0                  // the walk's two times: no step of the model is time's
#define AA_RECEIVE_TIME  1000.5

// the calls a step can make
enum aa_call_t
{
    AA_BEGIN,
    AA_RECEIVE,
    AA_SETTLE,
    AA_OPEN,
    AA_TAKE,
    AA_CLOSE,
};

// the game threads: each makes one take when the walk hands it one
struct aa_takers_t
{
    std::mutex mutex;
    std::condition_variable changed;
    struct rocketnet_server_acks_t * acks;
    int job[AA_GAME_THREADS];       // 0 none, 1 take, 2 quit
    int client[AA_GAME_THREADS];
    bool started[AA_GAME_THREADS];
    int applied;                    // what the last take returned
};

static void aa_taker( aa_takers_t * takers, int g )
{
    std::unique_lock<std::mutex> lock( takers->mutex );
    takers->started[g] = true;
    takers->changed.notify_all();
    for ( ;; )
    {
        takers->changed.wait( lock, [&]
                              { return takers->job[g] != 0; } );
        const int job = takers->job[g];
        if ( job == 1 )
        {
            takers->applied = rocketnet_server_acks_apply_client( takers->acks, takers->client[g] );
        }
        takers->job[g] = 0;
        takers->changed.notify_all();
        if ( job == 2 )
        {
            return;
        }
    }
}

// one job on game thread g, run to its end
static void aa_call( aa_takers_t * takers, int g, int job, int client )
{
    std::unique_lock<std::mutex> lock( takers->mutex );
    takers->client[g] = client;
    takers->job[g] = job;
    takers->changed.notify_all();
    takers->changed.wait( lock, [&]
                          { return takers->job[g] == 0; } );
}

static std::string aa_thread_name( int t )
{
    return t == 0 ? "\"main\"" : "\"g" + tla_int( t ) + "\"";
}

struct aa_walk_t
{
    struct rocketnet_server_acks_t * acks;
    bool took[AA_CLIENTS][AA_THREADS];      // the threads whose take returned blocks applied, per client, since the last open
};

struct aa_client_t
{
    int head;
    int tail;
    int claim;
    int ids;
};

static aa_client_t aa_client( const aa_walk_t * walk, int c )
{
    aa_client_t client;
    rocketnet_server_acks_client_for_test( walk->acks, c, &client.head, &client.tail, &client.claim, &client.ids );
    return client;
}

// every variable of RNAckApplyPhase: read from the code, but for took and
// arrived, which the walk keeps from its calls
static tla_record_t aa_state( const aa_walk_t * walk, const std::vector<std::string> & arrived )
{
    static const char * const phase_names[] = { "\"closed\"", "\"receiving\"", "\"open\"" };
    const struct rocketnet_server_acks_t * acks = walk->acks;

    std::vector<std::string> queue;
    for ( int e = 0; e < acks->num_pending; e++ )
    {
        const struct rocketnet_server_ack_pending_t * entry = &acks->pending[e];
        tla_record_t record;
        record.add( "client", tla_int( entry->client_index ) );
        record.add( "block", tla_int( (int64_t) entry->largest ) );
        record.add( "ranges", tla_int( entry->num_ranges ) );
        record.add( "roff", tla_int( entry->range_offset ) );
        record.add( "next", tla_int( entry->next ) );
        record.add( "ioff", tla_int( entry->ids_offset ) );
        record.add( "icnt", tla_int( entry->ids_count ) );
        queue.push_back( record.text() );
    }

    std::vector<std::string> clients, head, tail, claim, segment, took;
    for ( int c = 0; c < AA_CLIENTS; c++ )
    {
        const aa_client_t client = aa_client( walk, c );
        clients.push_back( tla_int( c ) );
        head.push_back( tla_int( client.head ) );
        tail.push_back( tla_int( client.tail ) );
        claim.push_back( tla_int( client.claim ) );

        std::vector<std::string> ids;
        const uint64_t * ids_of_client = acks->segments + (size_t) c * ROCKETNET_SENT_PACKET_BUFFER_SIZE;
        for ( int i = 0; i < client.ids; i++ )
        {
            ids.push_back( tla_int( (int64_t) ids_of_client[i] ) );
        }
        segment.push_back( tla_sequence( ids ) );

        std::string set = "{";
        for ( int t = 0; t < AA_THREADS; t++ )
        {
            if ( walk->took[c][t] )
            {
                set += ( set.size() > 1 ? ", " : "" ) + aa_thread_name( t );
            }
        }
        took.push_back( set + "}" );
    }

    std::vector<std::string> batch;
    for ( int i = 0; i < acks->num_acked; i++ )
    {
        std::vector<std::string> pair;
        pair.push_back( tla_int( acks->acked_client_index[i] ) );
        pair.push_back( tla_int( (int64_t) acks->acked_packet_ids[i] ) );
        batch.push_back( tla_sequence( pair ) );
    }

    tla_record_t state;
    state.add( "phase", acks->phase >= 0 && acks->phase <= 2 ? phase_names[acks->phase] : "\"unknown\"" );
    state.add( "queue", tla_sequence( queue ) );
    state.add( "numRanges", tla_int( acks->num_ranges ) );
    state.add( "head", tla_function( clients, head ) );
    state.add( "tail", tla_function( clients, tail ) );
    state.add( "claim", tla_function( clients, claim ) );
    state.add( "segment", tla_function( clients, segment ) );
    state.add( "batch", tla_sequence( batch ) );
    state.add( "took", tla_function( clients, took ) );
    state.add( "arrived", tla_sequence( arrived ) );
    return state;
}

static int trace_ack_apply( uint64_t seed, int steps, const char * out )
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    enum rocketnet_status_t status = rocketnet_init( NULL, NULL, NULL );
    if ( status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "tla_trace ackapply: rocketnet_init: %s\n", rocketnet_status_name( status ) );
        return 1;
    }

    aa_walk_t walk;
    memset( &walk, 0, sizeof( walk ) );
    walk.acks = rocketnet_server_acks_create( NULL, AA_CLIENTS );
    if ( !walk.acks || !rocketnet_server_acks_defer( walk.acks ) )
    {
        fprintf( stderr, "tla_trace ackapply: no deferred server acks\n" );
        return 1;
    }
    struct rocketnet_server_acks_t * acks = walk.acks;

    // before the first record: the queue and the range store are lowered so
    // the walk fills them; each client has sent every sequence a block of the
    // walk can acknowledge, each with its sequence as its packet id, and has
    // had every sequence below AA_FIRST_BLOCK acknowledged, so a block's
    // ranges find nothing new. The sends and that first acknowledgment are
    // the endpoint's, not steps of this model

    const int blocks = AA_FIRST_BLOCK + steps;
    bool fault = false;
    if ( !rocketnet_server_acks_lower_capacity_for_test( acks, AA_PENDING, ROCKETNET_ACK_BLOCK_MAX_RANGES ) )
    {
        fprintf( stderr, "tla_trace ackapply: the queue was not lowered to %d entries and %d ranges\n", AA_PENDING, ROCKETNET_ACK_BLOCK_MAX_RANGES );
        fault = true;
    }
    if ( blocks >= ROCKETNET_SENT_PACKET_BUFFER_SIZE || 2 * AA_BIG_RANGES >= AA_FIRST_BLOCK || 3 * AA_BIG_RANGES <= ROCKETNET_ACK_BLOCK_MAX_RANGES ||
         2 * AA_BIG_RANGES + ( AA_PENDING - 2 ) * AA_SMALL_RANGES > ROCKETNET_ACK_BLOCK_MAX_RANGES )
    {
        fprintf( stderr, "tla_trace ackapply: %d steps and blocks of %d ranges do not fit the sent window of %d and a store of %d ranges\n", steps,
                 AA_BIG_RANGES, ROCKETNET_SENT_PACKET_BUFFER_SIZE, ROCKETNET_ACK_BLOCK_MAX_RANGES );
        fault = true;
    }
    std::vector<uint64_t> first_ids( (size_t) AA_FIRST_BLOCK );
    for ( int c = 0; c < AA_CLIENTS && !fault; c++ )
    {
        for ( int sequence = 1; sequence <= blocks; sequence++ )
        {
            if ( rocketnet_ack_endpoint_record_sent_classified( &acks->endpoints[c], (uint64_t) sequence, 100, (uint64_t) sequence, 1, AA_SEND_TIME ) != ROCKETNET_ACK_SENT_RECORDED )
            {
                fprintf( stderr, "tla_trace ackapply: client %d's send %d was not recorded\n", c, sequence );
                fault = true;
            }
        }
        struct rocketnet_ack_block_t first;
        memset( &first, 0, sizeof( first ) );
        first.largest = AA_FIRST_BLOCK - 1;
        first.first_range_length = AA_FIRST_BLOCK - 1;
        int first_count = 0;
        uint64_t first_unknown = 0;
        uint64_t first_full = 0;
        rocketnet_ack_endpoint_process_block( &acks->endpoints[c], &first, AA_RECEIVE_TIME, first_ids.data(), AA_FIRST_BLOCK, &first_count, &first_unknown, &first_full );
        if ( first_count != AA_FIRST_BLOCK - 1 || first_unknown != 0 || first_full != 0 )
        {
            fprintf( stderr, "tla_trace ackapply: client %d's first acknowledgment gave %d ids\n", c, first_count );
            fault = true;
        }
    }

    std::vector<uint64_t> connection_ids( (size_t) acks->acked_capacity );
    uint64_t unknown_sequence = 0;
    uint64_t batch_full = 0;
    int next_block[AA_CLIENTS];
    for ( int c = 0; c < AA_CLIENTS; c++ )
    {
        next_block[c] = AA_FIRST_BLOCK;
    }
    std::vector<std::string> arrived;

    aa_takers_t * takers = new aa_takers_t();
    takers->acks = acks;
    std::vector<std::thread> threads;
    for ( int g = 0; g < AA_GAME_THREADS; g++ )
    {
        threads.emplace_back( aa_taker, takers, g );
    }
    {
        std::unique_lock<std::mutex> lock( takers->mutex );
        takers->changed.wait( lock, [&]
                              {
            for ( int g = 0; g < AA_GAME_THREADS; g++ )
            {
                if ( !takers->started[g] )
                {
                    return false;
                }
            }
            return true; } );
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", "<<>>", aa_state( &walk, arrived ) ) );

    for ( int step = 0; step < steps && !fault; step++ )
    {
        const char * op = "";
        const char * act = "";
        std::string args = "<<>>";
        bool planted = false;

        // the call: by the code's phase and the seed
        aa_call_t call = AA_TAKE;
        const int phase = acks->phase;
        const int pick = random.below( 100 );
        int client = random.below( AA_CLIENTS );
        if ( phase == ROCKETNET_SERVER_ACKS_CLOSED )
        {
            call = pick < 80 ? AA_BEGIN : AA_TAKE;
        }
        else if ( phase == ROCKETNET_SERVER_ACKS_RECEIVING )
        {
            call = pick < 66 ? AA_RECEIVE : ( pick < 74 ? AA_SETTLE : ( pick < 80 ? AA_TAKE : ( pick < 96 ? AA_OPEN : AA_CLOSE ) ) );
            if ( call == AA_SETTLE )
            {
                // a client with blocks queued, the first from the seed's pick; with none, a block arrives instead
                call = AA_RECEIVE;
                for ( int i = 0; i < AA_CLIENTS; i++ )
                {
                    const int c = ( client + i ) % AA_CLIENTS;
                    if ( aa_client( &walk, c ).head >= 0 )
                    {
                        client = c;
                        call = AA_SETTLE;
                        break;
                    }
                }
            }
        }
        else
        {
            call = pick < 72 ? AA_TAKE : AA_CLOSE;
        }

        if ( call == AA_BEGIN )
        {
            rocketnet_server_acks_begin_receive( acks, AA_RECEIVE_TIME );
            arrived.clear();
            op = "begin_receive";
            act = "BeginReceive";
        }
        else if ( call == AA_RECEIVE )
        {
            // the client's next sequence, with ranges over sequences acknowledged already
            const int b = next_block[client]++;
            // (not named small: that is a macro in the Windows headers)
            const int few = random.below( AA_SMALL_RANGES + 1 );
            const int ranges = random.below( 10 ) < 3 ? AA_BIG_RANGES : few;
            struct rocketnet_ack_block_t block;
            memset( &block, 0, sizeof( block ) );
            block.largest = (uint64_t) b;
            block.first_range_length = 1;
            block.num_ranges = ranges;
            for ( int r = 0; r < ranges; r++ )
            {
                block.range_gap[r] = 1;
                block.range_length[r] = 1;
            }
            rocketnet_server_acks_receive_block( acks, client, &block, AA_RECEIVE_TIME, 0x100 + (uint64_t) client, connection_ids.data(),
                                                 &unknown_sequence, &batch_full );
            std::vector<std::string> call_args;
            call_args.push_back( tla_int( client ) );
            call_args.push_back( tla_int( b ) );
            call_args.push_back( tla_int( ranges ) );
            args = tla_sequence( call_args );
            std::vector<std::string> pair;
            pair.push_back( tla_int( client ) );
            pair.push_back( tla_int( b ) );
            arrived.push_back( tla_sequence( pair ) );
            op = "receive_block";
            act = "Receive";
        }
        else if ( call == AA_SETTLE )
        {
            rocketnet_server_acks_settle( acks, client );
            std::vector<std::string> call_args;
            call_args.push_back( tla_int( client ) );
            args = tla_sequence( call_args );
            op = "settle";
            act = "Settle";
        }
        else if ( call == AA_OPEN )
        {
            rocketnet_server_acks_open( acks );
            memset( walk.took, 0, sizeof( walk.took ) );
            op = "open";
            act = "Open";
        }
        else if ( call == AA_TAKE )
        {
            // any thread, any client, any phase: what the call returns says what it was
            const int thread = random.below( AA_THREADS );
            int applied = 0;
            if ( thread == 0 )
            {
                applied = rocketnet_server_acks_apply_client( acks, client );
            }
            else
            {
                aa_call( takers, thread - 1, 1, client );
                applied = takers->applied;
            }
            std::vector<std::string> call_args;
            call_args.push_back( aa_thread_name( thread ) );
            call_args.push_back( tla_int( client ) );
            op = "take";
            if ( applied > 0 )
            {
                call_args.push_back( tla_int( applied ) );
                walk.took[client][thread] = true;
                act = "Take";
                planted = aa_client( &walk, client ).claim != 0;
            }
            else
            {
                act = phase == ROCKETNET_SERVER_ACKS_OPEN ? "TakeRefused" : "TakeOutside";
            }
            args = tla_sequence( call_args );
        }
        else
        {
            rocketnet_server_acks_close( acks, connection_ids.data(), &unknown_sequence, &batch_full );
            op = "close";
            act = "Close";
        }

        if ( unknown_sequence != 0 || batch_full != 0 )
        {
            fprintf( stderr, "tla_trace ackapply: a block of the walk counted %d unknown sequences and %d ids past the batch\n", (int) unknown_sequence, (int) batch_full );
            fault = true;
        }

        report.act( act );
        records.push_back( tla_step( op, act, args, aa_state( &walk, arrived ) ) );

        // the planted effect, where it shows: a client taken and its claim still standing
        if ( planted )
        {
            report.planted( (int) records.size() );
        }
    }

    report.print();

    // the model's bound on a block's number: the highest the walk used
    int max_block = AA_FIRST_BLOCK;
    for ( int c = 0; c < AA_CLIENTS; c++ )
    {
        if ( next_block[c] - 1 > max_block )
        {
            max_block = next_block[c] - 1;
        }
    }

    char range_counts[64];
    snprintf( range_counts, sizeof( range_counts ), "{0" );
    for ( int r = 1; r <= AA_SMALL_RANGES; r++ )
    {
        const size_t used = strlen( range_counts );
        snprintf( range_counts + used, sizeof( range_counts ) - used, ", %d", r );
    }
    {
        const size_t used = strlen( range_counts );
        snprintf( range_counts + used, sizeof( range_counts ) - used, ", %d}", AA_BIG_RANGES );
    }

    char constants[640];
    snprintf( constants, sizeof( constants ),
              "    Clients = {0, 1, 2}\n"
              "    Threads = {\"main\", \"g1\", \"g2\"}\n"
              "    Main = \"main\"\n"
              "    MinBlock = %d\n"
              "    MaxBlock = %d\n"
              "    MaxRanges = %d\n"
              "    RangeCounts = %s\n"
              "    PendingCap = %d\n"
              "    RangesCap = %d\n"
              "    ExchangeTake = TRUE\n"
              "    CloseAppliesUntaken = TRUE\n"
              "    CloseClearsClaim = TRUE\n"
              "    SettleOnlyReceiving = TRUE\n"
              "    GatherWhenFull = TRUE\n"
              "    AdvanceTail = TRUE\n",
              AA_FIRST_BLOCK, max_block, ROCKETNET_ACK_BLOCK_MAX_RANGES, range_counts, acks->pending_capacity, acks->ranges_capacity );
    const bool written = !fault && tla_write_trace( out, "MCTraceAckApply", seed, "TraceRNAckApplyPhase", "", constants, records );

    for ( int g = 0; g < AA_GAME_THREADS; g++ )
    {
        aa_call( takers, g, 2, 0 );
    }
    for ( size_t i = 0; i < threads.size(); i++ )
    {
        threads[i].join();
    }
    delete takers;

    if ( acks->phase != ROCKETNET_SERVER_ACKS_CLOSED )
    {
        rocketnet_server_acks_close( acks, connection_ids.data(), &unknown_sequence, &batch_full );
    }
    rocketnet_server_acks_destroy( acks );
    rocketnet_term();

    return written ? 0 : 1;
}

TLA_TRACE_LAYER( "ackapply", trace_ack_apply );
