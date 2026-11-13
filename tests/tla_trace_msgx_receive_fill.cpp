/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The msgx receive layer of the TLA+ trace check
// (proofs/tla/README-TraceCheck.md, proofs/tla/TraceRNMsgxReceiveFill.tla),
// layer msgxreceivefill: the macOS receive worker's drain of its sockets into
// three banks, sdk/rocketnet_msgx_receive_fill.h, and its model
// proofs/tla/RNMsgxReceiveFill.tla. A seeded random walk over a fake kernel
// (three sockets, each a count of queued datagrams, a small receive buffer
// and a script of errors) drives the real rocketnet_msgx_receive_drain_step
// one model step at a time, the worker loop around it as
// rocketnet_server_msgx_receive_worker runs it (a round over every socket, a
// held drain ending the round, the round's publish through
// rocketnet_msgx_receive_publish), and main's consume and
// rocketnet_msgx_receive_claim, and on even seeds main's stop
// (rocketnet_msgx_receive_stop, after which no claim succeeds) and the
// worker's exit, which counts what it holds. The record's act is the step
// the code returned; a read's argument is the count it took.
//
// The plant (ROCKETNET_MSGX_RECEIVE_FILL_PLANT, the read size that ignores
// the room) is in sdk/rocketnet_msgx_receive_fill.cpp. The banks are small
// (six) and a batch is four, so a read meets a room below a batch with more
// queued: the planted read takes past the room and the datagram past the
// bank's end is lost, or, with no more queued than the room, comes back
// short of its larger ask and leaves the socket. Every read that asked past
// the room is PLANT-AT.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../sdk/rocketnet_msgx_receive_fill.h"
#include "tla_trace_record.h"

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_MSGX_RECEIVE_FILL_PLANT 1
#include "../sdk/rocketnet_msgx_receive_fill.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

// the instance: RNMsgxReceiveFill with three sockets, banks of six, a batch
// of four, a budget of six, receive buffers of eight
#define RF_SOCKETS 3
#define RF_CAPACITY 6
#define RF_BATCH 4
#define RF_BUDGET 6
#define RF_BUFFER 8
#define RF_MAX_ERRORS 64

enum
{
    RF_POLL,
    RF_READ,
    RF_END,
    RF_HELD,
    RF_EXIT
};

struct rf_walk_t
{
    tla_random_t random;

    // the kernel
    int kq[RF_SOCKETS];
    std::vector<std::string> errs[RF_SOCKETS]; // "weather" or "failed", in order

    // the banks and the hand
    int cnt[3];
    int calls[3];                   // reads counted in each bank since the worker took it (the bank's receive_calls)
    struct rocketnet_msgx_receive_banks_t hand;

    // the worker
    int pc;
    int cur;                        // 1-based, 0 between rounds
    struct rocketnet_msgx_receive_drain_t drain;
    int last_read;                  // what the fake read returned
    int last_want;

    int64_t arrived;
    int64_t kdrops;
    int64_t consumed;
    int64_t full_drops;
    int64_t errors;
    bool exited;
    int64_t stopped;                // counted stopped: main's tail at its stop, the worker's banks at its exit
};

static int rf_banked( void * user )
{
    const rf_walk_t * walk = (const rf_walk_t *) user;
    return walk->cnt[walk->hand.worker];
}

static int rf_read( void * user, int fd, int want )
{
    rf_walk_t * walk = (rf_walk_t *) user;
    walk->last_want = want;
    walk->calls[walk->hand.worker]++;
    walk->last_read = 0;
    if ( !walk->errs[fd].empty() )
    {
        const std::string error = walk->errs[fd].front();
        walk->errs[fd].erase( walk->errs[fd].begin() );
        if ( error == "weather" )
        {
            // an ICMP error queued for the client, or EINTR, or ENOBUFS
            const int weather[] = { ECONNREFUSED, EHOSTUNREACH, EINTR, ENOBUFS };
            return -weather[walk->random.below( 4 )];
        }
        return walk->random.below( 2 ) == 0 ? -ENOTCONN : -EBADF;
    }
    if ( walk->kq[fd] == 0 )
    {
        return -EAGAIN;
    }
    const int n = walk->kq[fd] < want ? walk->kq[fd] : want;
    walk->kq[fd] -= n;
    walk->last_read = n;
    return n;
}

static void rf_take( void * user, int i )
{
    rf_walk_t * walk = (rf_walk_t *) user;
    (void) i;
    if ( walk->cnt[walk->hand.worker] >= RF_CAPACITY )
    {
        walk->full_drops++;
        return;
    }
    walk->cnt[walk->hand.worker]++;
}

static bool rf_publish( void * user )
{
    rf_walk_t * walk = (rf_walk_t *) user;
    if ( !rocketnet_msgx_receive_publish( &walk->hand ) )
    {
        return false;
    }
    walk->calls[walk->hand.worker] = 0;
    return true;
}

static void rf_failed( void * user, int fd, int kind, int error )
{
    (void) user;
    (void) fd;
    (void) kind;
    (void) error;
}

static std::string rf_record_kq( const rf_walk_t * walk )
{
    std::vector<std::string> keys;
    std::vector<std::string> values;
    for ( int s = 0; s < RF_SOCKETS; s++ )
    {
        keys.push_back( tla_int( s + 1 ) );
        values.push_back( tla_int( walk->kq[s] ) );
    }
    return tla_function( keys, values );
}

static std::string rf_record_errs( const rf_walk_t * walk )
{
    std::vector<std::string> keys;
    std::vector<std::string> values;
    for ( int s = 0; s < RF_SOCKETS; s++ )
    {
        std::vector<std::string> sequence;
        for ( const std::string & error : walk->errs[s] )
        {
            sequence.push_back( tla_string( error ) );
        }
        keys.push_back( tla_int( s + 1 ) );
        values.push_back( tla_sequence( sequence ) );
    }
    return tla_function( keys, values );
}

static tla_record_t rf_record( const rf_walk_t * walk )
{
    static const char * const pcs[] = { "poll", "read", "end", "held", "exit" };
    std::vector<std::string> bank_keys;
    std::vector<std::string> bank_values;
    for ( int b = 0; b < 3; b++ )
    {
        bank_keys.push_back( tla_int( b ) );
        bank_values.push_back( tla_int( walk->cnt[b] ) );
    }
    tla_record_t record;
    record.add( "kq", rf_record_kq( walk ) );
    record.add( "errs", rf_record_errs( walk ) );
    record.add( "cnt", tla_function( bank_keys, bank_values ) );
    record.add( "worker", tla_int( walk->hand.worker ) );
    record.add( "ready", tla_int( walk->hand.ready ) );
    record.add( "main", tla_int( walk->hand.main ) );
    record.add( "free", tla_int( walk->hand.free ) );
    record.add( "pc", tla_string( pcs[walk->pc] ) );
    record.add( "cur", tla_int( walk->cur ) );
    record.add( "reads", tla_int( walk->pc == RF_READ || walk->pc == RF_HELD ? walk->drain.reads : 0 ) );
    record.add( "arrived", tla_int( walk->arrived ) );
    record.add( "kdrops", tla_int( walk->kdrops ) );
    record.add( "consumed", tla_int( walk->consumed ) );
    record.add( "fullDrops", tla_int( walk->full_drops ) );
    record.add( "errors", tla_int( walk->errors ) );
    record.add( "stopping", tla_bool( walk->hand.stopped ) );
    record.add( "exited", tla_bool( walk->exited ) );
    record.add( "stopped", tla_int( walk->stopped ) );
    return record;
}

static void rf_push( std::vector<tla_record_t> & records, tla_report_t & report, const rf_walk_t * walk, const char * op, const char * act,
                     const std::string & args )
{
    records.push_back( tla_step( op, act, args, rf_record( walk ) ) );
    report.act( act );
}

// the drain of cur is done: the next socket, or the round's end
static void rf_next_socket( rf_walk_t * walk )
{
    if ( walk->cur == RF_SOCKETS )
    {
        walk->cur = 0;
        walk->pc = RF_END;
        return;
    }
    walk->cur++;
    rocketnet_msgx_receive_drain_start( &walk->drain, walk->cur - 1 );
}

static const char * rf_step_act( int step )
{
    switch ( step )
    {
        case ROCKETNET_MSGX_DRAIN_STEP_PUBLISH_FULL:
            return "PublishFull";
        case ROCKETNET_MSGX_DRAIN_STEP_HOLD_FULL:
            return "HoldFull";
        case ROCKETNET_MSGX_DRAIN_STEP_BUDGET:
            return "BudgetSpent";
        case ROCKETNET_MSGX_DRAIN_STEP_READ:
            return "Read";
        case ROCKETNET_MSGX_DRAIN_STEP_EMPTY:
            return "ReadEmpty";
        case ROCKETNET_MSGX_DRAIN_STEP_WEATHER:
            return "ReadWeather";
        default:
            return "ReadFailed";
    }
}

static int trace_msgx_receive_fill( uint64_t seed, int steps, const char * out )
{
    rf_walk_t * walk = new rf_walk_t();
    walk->random.state = seed;
    memset( walk->kq, 0, sizeof( walk->kq ) );
    memset( walk->cnt, 0, sizeof( walk->cnt ) );
    memset( walk->calls, 0, sizeof( walk->calls ) );
    rocketnet_msgx_receive_banks_init( &walk->hand );
    walk->pc = RF_POLL;
    walk->cur = 0;
    memset( &walk->drain, 0, sizeof( walk->drain ) );
    walk->arrived = walk->kdrops = walk->consumed = walk->full_drops = walk->errors = walk->stopped = 0;
    walk->exited = false;

    struct rocketnet_msgx_receive_io_t io;
    io.user = walk;
    io.banked = rf_banked;
    io.read = rf_read;
    io.take = rf_take;
    io.publish = rf_publish;
    io.failed = rf_failed;

    tla_report_t report;
    std::vector<tla_record_t> records;
    records.push_back( tla_step( "init", "init", "<<>>", rf_record( walk ) ) );

    // bursts of arrivals against a slow main, so banks fill and the drain
    // holds; the walk's own mix changes by seed
    const int arrive_weight = 4 + (int) ( seed % 3 );
    while ( (int) records.size() < steps + 1 )
    {
        const int pick = walk->random.below( 16 );
        if ( pick < arrive_weight )
        {
            // Arrive(s) or KernelDrop(s): the kernel's
            const int s = walk->random.below( RF_SOCKETS );
            walk->arrived++;
            if ( walk->kq[s] < RF_BUFFER )
            {
                walk->kq[s]++;
                rf_push( records, report, walk, "arrive", "Arrive", tla_sequence( { tla_int( s + 1 ) } ) );
            }
            else
            {
                walk->kdrops++;
                rf_push( records, report, walk, "arrive", "KernelDrop", tla_sequence( { tla_int( s + 1 ) } ) );
            }
            continue;
        }
        if ( pick == arrive_weight && walk->errors < RF_MAX_ERRORS )
        {
            // Error(s, e): an error queued ahead of a socket's datagrams
            const int s = walk->random.below( RF_SOCKETS );
            const char * kind = walk->random.below( 3 ) == 0 ? "failed" : "weather";
            walk->errs[s].push_back( kind );
            walk->errors++;
            rf_push( records, report, walk, "error", "Error", tla_sequence( { tla_int( s + 1 ), tla_string( kind ) } ) );
            continue;
        }
        // MainStop, on half the seeds, three quarters of the way through:
        // main's tail counted stopped (rocketnet_server_msgx_stop), and
        // main claims nothing from here on
        if ( !walk->hand.stopped && seed % 2 == 0 && (int) records.size() > steps * 3 / 4 )
        {
            walk->stopped += walk->cnt[walk->hand.main];
            walk->cnt[walk->hand.main] = 0;
            rocketnet_msgx_receive_stop( &walk->hand );
            rf_push( records, report, walk, "stop", "MainStop", "<<>>" );
            continue;
        }
        if ( walk->exited )
        {
            // nobody runs but the kernel: main stopped calling once the
            // worker exited
            continue;
        }
        if ( pick >= 14 )
        {
            // main: consume its bank, then claim the ready one
            if ( walk->cnt[walk->hand.main] > 0 )
            {
                walk->consumed += walk->cnt[walk->hand.main];
                walk->cnt[walk->hand.main] = 0;
                rf_push( records, report, walk, "consume", "Consume", "<<>>" );
                continue;
            }
            if ( rocketnet_msgx_receive_claim( &walk->hand ) )
            {
                rf_push( records, report, walk, "claim", "Claim", "<<>>" );
                continue;
            }
        }

        // the worker's next step
        if ( walk->pc == RF_POLL && walk->hand.stopped )
        {
            // Exit, as rocketnet_server_msgx_receive_worker's tail: the
            // bank it fills and a ready bank main has not claimed are
            // counted stopped
            walk->stopped += walk->cnt[walk->hand.worker];
            walk->cnt[walk->hand.worker] = 0;
            if ( walk->hand.ready != ROCKETNET_MSGX_RECEIVE_NO_BANK )
            {
                walk->stopped += walk->cnt[walk->hand.ready];
                walk->cnt[walk->hand.ready] = 0;
            }
            walk->exited = true;
            walk->pc = RF_EXIT;
            rf_push( records, report, walk, "exit", "Exit", "<<>>" );
            continue;
        }
        if ( walk->pc == RF_POLL )
        {
            walk->pc = RF_READ;
            walk->cur = 1;
            rocketnet_msgx_receive_drain_start( &walk->drain, 0 );
            rf_push( records, report, walk, "poll", "Poll", "<<>>" );
            continue;
        }
        if ( walk->pc == RF_HELD )
        {
            walk->pc = RF_POLL;
            walk->cur = 0;
            rf_push( records, report, walk, "sleep", "Sleep", "<<>>" );
            continue;
        }
        if ( walk->pc == RF_END )
        {
            // the round's publish, as the worker loop's: a bank with
            // datagrams or counted reads goes to main when main has taken
            // the last
            const int worker = walk->hand.worker;
            const bool published = ( walk->cnt[worker] > 0 || walk->calls[worker] > 0 ) && rf_publish( walk );
            walk->pc = RF_POLL;
            rf_push( records, report, walk, "round", published ? "RoundPublish" : "RoundKeep", "<<>>" );
            continue;
        }

        // RF_READ: one step of the drain of cur
        const int room = RF_CAPACITY - walk->cnt[walk->hand.worker];
        const int64_t drops_before = walk->full_drops;
        int count = 0;
        const int step = rocketnet_msgx_receive_drain_step( &walk->drain, &io, RF_CAPACITY, RF_BATCH, RF_BUDGET, &count );
        const char * act = rf_step_act( step );
        std::string args = "<<>>";
        if ( step == ROCKETNET_MSGX_DRAIN_STEP_READ )
        {
            args = tla_sequence( { tla_int( count ) } );
        }
        if ( walk->drain.pc == ROCKETNET_MSGX_DRAIN_DONE )
        {
            rf_next_socket( walk );
        }
        else if ( walk->drain.pc == ROCKETNET_MSGX_DRAIN_HELD )
        {
            walk->pc = RF_HELD;
        }
        rf_push( records, report, walk, "step", act, args );
        // the plant's effect: a read asked for more than the bank had room
        // for. It shows as a read past the room (a datagram lost), or as a
        // read of no more than the room that the larger ask makes short, so
        // the drain leaves the socket where the clean one reads on
        if ( step == ROCKETNET_MSGX_DRAIN_STEP_READ && ( walk->last_want > room || count > room || walk->full_drops > drops_before ) )
        {
            report.planted( (int) records.size() );
        }
    }

    report.print();

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    Sockets = %d\n    Capacity = %d\n    Batch = %d\n    Budget = %d\n    Buffer = %d\n"
              "    MaxArrivals = %lld\n    MaxErrors = %lld\n    ReadPastRoom = FALSE\n    PublishOverReady = FALSE\n"
              "    HoldWithoutPublish = FALSE\n    ClaimAfterStop = FALSE\n    ReadPastBudget = FALSE\n",
              RF_SOCKETS, RF_CAPACITY, RF_BATCH, RF_BUDGET, RF_BUFFER, (long long) walk->arrived + 1, (long long) walk->errors + 1 );
    const bool written = tla_write_trace( out, "MCTraceMsgxReceiveFill", seed, "TraceRNMsgxReceiveFill", "", constants, records );
    delete walk;
    return written ? 0 : 1;
}

TLA_TRACE_LAYER( "msgxreceivefill", trace_msgx_receive_fill );
