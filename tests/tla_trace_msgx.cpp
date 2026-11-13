/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The msgx layer of the TLA+ trace check (proofs/tla/README-TraceCheck.md):
// the macOS per-client socket table, sdk/rocketnet_msgx_socket_table.h, and
// its model proofs/tla/RNMsgxSocketTable.tla, bound by
// proofs/tla/TraceRNMsgxSocketTable.tla. A seeded random walk drives the real
// table through the calls the model names, over a fake kernel (open hands out
// the lowest free descriptor of three, refuses a second socket connected to
// the same peer, and maps each descriptor to its peer; close frees it), a
// fake receive poller, fake fill threads and fake holders, all stepped on this
// one thread. Each step is one call of the table or one step of a fake, and
// writes one record: the op, the model action it took, its arguments, and
// every model variable after it, read from the table's own fields and the
// fakes. No socket, no clock, no thread.
//
// A call of main's (forget, sweep) is one record: the walk sees the table
// between calls, so the model action is the call-level ForgetCall or
// SweepCall. The model's fine-grained steps of those calls (Forget, Retire,
// ForgetDereg, SweepBegin, RetireDead, Close) let holders run inside a call
// and are listed untraced in tools/tla_trace_check.py.
//
// The plant (ROCKETNET_MSGX_SOCKET_TABLE_PLANT, in
// sdk/rocketnet_msgx_socket_table.cpp, the tla_trace_plant_msgx build): a
// retire closes the socket at once instead of after the holders' passes.
// PLANT-FIRED counts the closes of a slot still open, which the clean table
// never makes: its closes are the sweep's, of retired slots.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_MSGX_SOCKET_TABLE_PLANT 1
#include "../sdk/rocketnet_msgx_socket_table.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>
#include <vector>

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_msgx_socket_table.h"
#include "tla_trace_record.h"

// the instance: the model's checked shape, with the code's index (2048
// entries) and cooldown table (64 entries) and four addresses whose index
// homes collide and wrap past the last entry
#define MSGX_SLOTS 3
#define MSGX_ADDRS 4
#define MSGX_FDS 3
#define MSGX_TX 2
#define MSGX_HOLDERS 3      // holders 1..2 transmit, 3 receive (the code's 0..2)
#define MSGX_RX 3
#define MSGX_BUDGET 2
#define MSGX_MAX_ENV 1

// ---------------------------------------------------------------------------
// the walk's world: the fake kernel, poller, packets and holders
// ---------------------------------------------------------------------------

struct msgx_packet_t
{
    int a;
    int s;
    uint32_t g;
    int h;
    const char * st;
    int fd;
};

struct msgx_event_t
{
    int f;
    int s;
    uint32_t g;
    int a;
};

struct msgx_walk_t
{
    struct rocketnet_msgx_socket_table_t * table;
    struct rocketnet_address_t address[MSGX_ADDRS + 1];    // addresses 1..MSGX_ADDRS
    int kfd[MSGX_FDS];          // -1 closed, 0 another descriptor of the process, else the peer
    bool kdead[MSGX_FDS];
    std::vector<msgx_event_t> poller;
    std::vector<msgx_packet_t> pkts;
    int cur[MSGX_TX + 1];       // the packet (1-based) a transmit holder resolved, or 0
    bool rep_set[MSGX_HOLDERS + 1];
    int rep_s[MSGX_HOLDERS + 1];
    uint32_t rep_g[MSGX_HOLDERS + 1];
    bool rx_set;
    msgx_event_t rx_ev;
    int rx_fd;
    bool crossed;
    bool on_closed;
    int attempts;
    int early_closes;           // closes of a slot still open: the plant firing
};

static int msgx_address_id( const msgx_walk_t & walk, const struct rocketnet_address_t * address )
{
    for ( int a = 1; a <= MSGX_ADDRS; a++ )
    {
        if ( rocketnet_address_equal( &walk.address[a], address ) )
        {
            return a;
        }
    }
    return 0;
}

static int msgx_lowest_free( const msgx_walk_t & walk )
{
    for ( int f = 0; f < MSGX_FDS; f++ )
    {
        if ( walk.kfd[f] == -1 )
        {
            return f;
        }
    }
    return -1;
}

// the kernel's socket, bind and connect
static int msgx_fake_open( void * user, const struct rocketnet_address_t * address )
{
    msgx_walk_t * walk = (msgx_walk_t *) user;
    walk->attempts++;
    const int a = msgx_address_id( *walk, address );
    for ( int f = 0; f < MSGX_FDS; f++ )
    {
        if ( walk->kfd[f] == a )
        {
            return -EADDRINUSE;
        }
    }
    const int f = msgx_lowest_free( *walk );
    if ( f < 0 )
    {
        return -EMFILE;
    }
    walk->kfd[f] = a;
    walk->kdead[f] = false;
    return f;
}

static void msgx_fake_close( void * user, int fd )
{
    msgx_walk_t * walk = (msgx_walk_t *) user;
    if ( fd < 0 || fd >= MSGX_FDS )
    {
        return;
    }
    for ( int s = 0; s < MSGX_SLOTS; s++ )
    {
        const struct rocketnet_msgx_socket_slot_t * slot = &walk->table->slots[s];
        if ( slot->fd == fd && slot->state == ROCKETNET_MSGX_SLOT_OPEN )
        {
            walk->early_closes++;
        }
    }
    walk->kfd[fd] = -1;
    walk->kdead[fd] = false;
}

// the caller deregisters a descriptor from its receive poller
static void msgx_deregister( msgx_walk_t & walk, int fd )
{
    std::vector<msgx_event_t> kept;
    for ( size_t i = 0; i < walk.poller.size(); i++ )
    {
        if ( walk.poller[i].f != fd )
        {
            kept.push_back( walk.poller[i] );
        }
    }
    walk.poller = kept;
}

static void msgx_retire_hook( void * user, int fd )
{
    msgx_deregister( *(msgx_walk_t *) user, fd );
}

// ---------------------------------------------------------------------------
// the record: every variable of RNMsgxSocketTable
// ---------------------------------------------------------------------------

static std::vector<std::string> msgx_slot_keys()
{
    std::vector<std::string> keys;
    for ( int s = 0; s < MSGX_SLOTS; s++ )
    {
        keys.push_back( tla_int( s ) );
    }
    return keys;
}

static std::vector<std::string> msgx_holder_keys()
{
    std::vector<std::string> keys;
    for ( int h = 1; h <= MSGX_HOLDERS; h++ )
    {
        keys.push_back( tla_int( h ) );
    }
    return keys;
}

static std::string msgx_event_text( const msgx_event_t & e )
{
    return tla_sequence( { tla_int( e.f ), tla_int( e.s ), tla_int( e.g ), tla_int( e.a ) } );
}

static tla_record_t msgx_state( const msgx_walk_t & walk )
{
    const struct rocketnet_msgx_socket_table_t * table = walk.table;
    const std::vector<std::string> slot_keys = msgx_slot_keys();
    const std::vector<std::string> holder_keys = msgx_holder_keys();

    std::vector<std::string> word, dead_rep, state, fd, gen, addr, snap, free_slots, retired;
    for ( int s = 0; s < MSGX_SLOTS; s++ )
    {
        const struct rocketnet_msgx_socket_slot_t * slot = &table->slots[s];
        const uint64_t w = slot->word.load();
        word.push_back( tla_sequence( { tla_int( rocketnet_msgx_word_generation( w ) ), tla_int( (int64_t) ( w & 0xFFFFFFFFULL ) ) } ) );
        dead_rep.push_back( tla_int( (int64_t) slot->dead_report.load() ) );
        state.push_back( tla_int( slot->state ) );
        fd.push_back( tla_int( slot->fd ) );
        gen.push_back( tla_int( slot->generation ) );
        addr.push_back( tla_int( msgx_address_id( walk, &slot->address ) ) );
        std::vector<std::string> holder_snap;
        for ( int h = 1; h <= MSGX_HOLDERS; h++ )
        {
            holder_snap.push_back( tla_int( (int64_t) slot->snapshot[h - 1] ) );
        }
        snap.push_back( tla_function( holder_keys, holder_snap ) );
        free_slots.push_back( tla_int( table->free_slots[s] ) );
        retired.push_back( tla_int( table->retired[s] ) );
    }

    std::vector<std::string> pass;
    for ( int h = 1; h <= MSGX_HOLDERS; h++ )
    {
        pass.push_back( tla_int( (int64_t) table->pass[h - 1].load() ) );
    }

    // the entries in use: the model's index is empty where the code holds -1
    std::vector<std::string> index_keys, index_values;
    for ( int e = 0; e < ROCKETNET_MSGX_TABLE_INDEX_SIZE; e++ )
    {
        if ( table->index[e] >= 0 )
        {
            index_keys.push_back( tla_int( e ) );
            index_values.push_back( tla_int( table->index[e] ) );
        }
    }

    std::vector<std::string> cool_keys, cool_addr, cool_until;
    for ( int e = 0; e < ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES; e++ )
    {
        // the entries in use: the model's cooldown table is empty where the code holds zero
        const int a = msgx_address_id( walk, &table->cooldown_address[e] );
        if ( a != 0 || table->cooldown_until[e] != 0 )
        {
            cool_keys.push_back( tla_int( e ) );
            cool_addr.push_back( tla_int( a ) );
            cool_until.push_back( tla_int( (int64_t) table->cooldown_until[e] ) );
        }
    }

    std::vector<std::string> counter_keys, counters;
    for ( int c = 0; c < ROCKETNET_MSGX_TABLE_NUM_COUNTERS; c++ )
    {
        counter_keys.push_back( tla_int( c ) );
        counters.push_back( tla_int( (int64_t) table->counters[c] ) );
    }

    std::vector<std::string> fd_keys, kfd, kdead;
    for ( int f = 0; f < MSGX_FDS; f++ )
    {
        fd_keys.push_back( tla_int( f ) );
        kfd.push_back( tla_int( walk.kfd[f] ) );
        kdead.push_back( tla_bool( walk.kdead[f] ) );
    }

    std::string poller = "{";
    for ( size_t i = 0; i < walk.poller.size(); i++ )
    {
        poller += ( i > 0 ? ", " : "" ) + msgx_event_text( walk.poller[i] );
    }
    poller += "}";

    std::vector<std::string> pkts;
    for ( size_t i = 0; i < walk.pkts.size(); i++ )
    {
        const msgx_packet_t & p = walk.pkts[i];
        pkts.push_back( "[a |-> " + tla_int( p.a ) + ", s |-> " + tla_int( p.s ) + ", g |-> " + tla_int( p.g ) + ", h |-> " + tla_int( p.h ) +
                        ", st |-> " + tla_string( p.st ) + ", fd |-> " + tla_int( p.fd ) + "]" );
    }

    std::vector<std::string> tx_keys, cur;
    for ( int h = 1; h <= MSGX_TX; h++ )
    {
        tx_keys.push_back( tla_int( h ) );
        cur.push_back( tla_int( walk.cur[h] ) );
    }

    std::vector<std::string> rep;
    for ( int h = 1; h <= MSGX_HOLDERS; h++ )
    {
        rep.push_back( walk.rep_set[h] ? tla_sequence( { tla_int( walk.rep_s[h] ), tla_int( walk.rep_g[h] ) } ) : "<<>>" );
    }

    tla_record_t record;
    record.add( "word", tla_function( slot_keys, word ) );
    record.add( "deadRep", tla_function( slot_keys, dead_rep ) );
    record.add( "state", tla_function( slot_keys, state ) );
    record.add( "fd", tla_function( slot_keys, fd ) );
    record.add( "gen", tla_function( slot_keys, gen ) );
    record.add( "addr", tla_function( slot_keys, addr ) );
    record.add( "snap", tla_function( slot_keys, snap ) );
    record.add( "pass", tla_function( holder_keys, pass ) );
    record.add( "deadReports", tla_int( table->dead_reports.load() ) );
    record.add( "freeSlots", tla_function( slot_keys, free_slots ) );
    record.add( "numFree", tla_int( table->num_free ) );
    record.add( "retired", tla_function( slot_keys, retired ) );
    record.add( "numRetired", tla_int( table->num_retired ) );
    record.add( "numOpen", tla_int( table->num_open ) );
    record.add( "index", tla_function( index_keys, index_values ) );
    record.add( "coolAddr", tla_function( cool_keys, cool_addr ) );
    record.add( "coolUntil", tla_function( cool_keys, cool_until ) );
    record.add( "counters", tla_function( counter_keys, counters ) );
    record.add( "update", tla_int( (int64_t) table->update ) );
    record.add( "opensLeft", tla_int( table->opens_left ) );
    record.add( "mpc", tla_string( "idle" ) );
    record.add( "ms", "0" );
    record.add( "mi", "0" );
    record.add( "mk", "0" );
    record.add( "kfd", tla_function( fd_keys, kfd ) );
    record.add( "kdead", tla_function( fd_keys, kdead ) );
    record.add( "poller", poller );
    record.add( "pkts", tla_sequence( pkts ) );
    record.add( "cur", tla_function( tx_keys, cur ) );
    record.add( "rep", tla_function( holder_keys, rep ) );
    record.add( "rxEv", walk.rx_set ? msgx_event_text( walk.rx_ev ) : "<<>>" );
    record.add( "rxFd", tla_int( walk.rx_fd ) );
    record.add( "crossed", tla_bool( walk.crossed ) );
    record.add( "onClosed", tla_bool( walk.on_closed ) );
    record.add( "attempts", tla_int( walk.attempts ) );
    return record;
}

// ---------------------------------------------------------------------------
// the calls, each the model action its op names
// ---------------------------------------------------------------------------

struct msgx_op_t
{
    const char * n;
    int a;
    int h;
    int i;
    int f;
};

// the op's model action
static const char * msgx_act( const char * op )
{
    static const char * const table[][2] = {
        { "mark", "Mark" }, { "forget", "ForgetCall" }, { "update", "BeginUpdate" }, { "sweep", "SweepCall" }, { "stamp", "Stamp" }, { "holder_begin", "HolderBegin" }, { "holder_end", "HolderEnd" }, { "report", "ReportDead" }, { "resolve", "TxResolve" }, { "send", "TxSend" }, { "poll", "RxPoll" }, { "rx_resolve", "RxResolve" }, { "rx_read", "RxRead" }, { "env_open", "EnvOpen" }, { "env_close", "EnvClose" }, { "disconnect", "Disconnect" }
    };
    for ( size_t k = 0; k < sizeof( table ) / sizeof( table[0] ); k++ )
    {
        if ( strcmp( table[k][0], op ) == 0 )
        {
            return table[k][1];
        }
    }
    return "";
}

static bool msgx_inside( const msgx_walk_t & walk, int h )
{
    return ( walk.table->pass[h - 1].load() & 1 ) == 1;
}

// a send or a read on descriptor fd for the client at address a
static bool msgx_use( msgx_walk_t & walk, int fd, int a, bool * dead )
{
    const int k = walk.kfd[fd];
    if ( k != -1 && k != a )
    {
        walk.crossed = true;
    }
    if ( k == -1 )
    {
        walk.on_closed = true;
    }
    *dead = k == a && walk.kdead[fd];
    return k != -1;
}

// runs the call; returns its arguments, a TLA+ sequence, in the order of the model action's parameters
static std::string msgx_do( msgx_walk_t & walk, const msgx_op_t & op, tla_random_t & random )
{
    struct rocketnet_msgx_socket_table_t * table = walk.table;
    if ( strcmp( op.n, "mark" ) == 0 )
    {
        const uint64_t opened = table->counters[ROCKETNET_MSGX_TABLE_COUNTER_OPENED];
        (void) rocketnet_msgx_socket_table_mark( table, &walk.address[op.a] );
        if ( table->counters[ROCKETNET_MSGX_TABLE_COUNTER_OPENED] != opened )
        {
            // the caller registers the new socket with its receive poller
            const int s = rocketnet_msgx_socket_table_lookup( table, &walk.address[op.a] );
            const msgx_event_t e = { table->slots[s].fd, s, rocketnet_msgx_word_generation( table->slots[s].word.load() ), op.a };
            walk.poller.push_back( e );
        }
        return tla_sequence( { tla_int( op.a ) } );
    }
    if ( strcmp( op.n, "forget" ) == 0 )
    {
        int retired_fd = -1;
        (void) rocketnet_msgx_socket_table_forget( table, &walk.address[op.a], &retired_fd );
        if ( retired_fd >= 0 )
        {
            msgx_deregister( walk, retired_fd );
        }
        return tla_sequence( { tla_int( op.a ) } );
    }
    if ( strcmp( op.n, "update" ) == 0 )
    {
        rocketnet_msgx_socket_table_begin_update( table, table->update + 1 );
        return "<<>>";
    }
    if ( strcmp( op.n, "sweep" ) == 0 )
    {
        (void) rocketnet_msgx_socket_table_sweep( table, msgx_retire_hook, &walk );
        return "<<>>";
    }
    if ( strcmp( op.n, "stamp" ) == 0 )
    {
        uint32_t g = 0;
        const int s = rocketnet_msgx_socket_table_stamp( table, &walk.address[op.a], &g );
        const msgx_packet_t p = { op.a, s, g, op.h, "queued", -1 };
        walk.pkts.push_back( p );
        return tla_sequence( { tla_int( op.a ), tla_int( op.h ) } );
    }
    if ( strcmp( op.n, "holder_begin" ) == 0 )
    {
        rocketnet_msgx_socket_table_holder_begin( table, op.h - 1 );
        return tla_sequence( { tla_int( op.h ) } );
    }
    if ( strcmp( op.n, "holder_end" ) == 0 )
    {
        rocketnet_msgx_socket_table_holder_end( table, op.h - 1 );
        return tla_sequence( { tla_int( op.h ) } );
    }
    if ( strcmp( op.n, "report" ) == 0 )
    {
        rocketnet_msgx_socket_table_report_dead( table, walk.rep_s[op.h], walk.rep_g[op.h] );
        walk.rep_set[op.h] = false;
        walk.rep_s[op.h] = 0;
        walk.rep_g[op.h] = 0;
        return tla_sequence( { tla_int( op.h ) } );
    }
    if ( strcmp( op.n, "resolve" ) == 0 )
    {
        msgx_packet_t & p = walk.pkts[(size_t) op.i - 1];
        const int f = rocketnet_msgx_socket_table_resolve( table, p.s, p.g );
        if ( f < 0 )
        {
            p.st = "shared";
        }
        else
        {
            p.st = "resolved";
            p.fd = f;
            walk.cur[op.h] = op.i;
        }
        return tla_sequence( { tla_int( op.h ), tla_int( op.i ) } );
    }
    if ( strcmp( op.n, "send" ) == 0 )
    {
        msgx_packet_t & p = walk.pkts[(size_t) walk.cur[op.h] - 1];
        bool dead = false;
        const bool open = msgx_use( walk, p.fd, p.a, &dead );
        if ( dead )
        {
            p.st = "counted";
            walk.rep_set[op.h] = true;
            walk.rep_s[op.h] = p.s;
            walk.rep_g[op.h] = p.g;
        }
        else
        {
            p.st = open ? "conn" : "counted";
        }
        walk.cur[op.h] = 0;
        return tla_sequence( { tla_int( op.h ) } );
    }
    if ( strcmp( op.n, "poll" ) == 0 )
    {
        walk.rx_ev = walk.poller[(size_t) random.below( (int) walk.poller.size() )];
        walk.rx_set = true;
        return tla_sequence( { msgx_event_text( walk.rx_ev ) } );
    }
    if ( strcmp( op.n, "rx_resolve" ) == 0 )
    {
        const int f = rocketnet_msgx_socket_table_resolve( table, walk.rx_ev.s, walk.rx_ev.g );
        if ( f < 0 )
        {
            walk.rx_set = false;
            memset( &walk.rx_ev, 0, sizeof( walk.rx_ev ) );
        }
        else
        {
            walk.rx_fd = f;
        }
        return "<<>>";
    }
    if ( strcmp( op.n, "rx_read" ) == 0 )
    {
        bool dead = false;
        (void) msgx_use( walk, walk.rx_fd, walk.rx_ev.a, &dead );
        if ( dead )
        {
            walk.rep_set[MSGX_RX] = true;
            walk.rep_s[MSGX_RX] = walk.rx_ev.s;
            walk.rep_g[MSGX_RX] = walk.rx_ev.g;
        }
        walk.rx_set = false;
        memset( &walk.rx_ev, 0, sizeof( walk.rx_ev ) );
        walk.rx_fd = -1;
        return "<<>>";
    }
    if ( strcmp( op.n, "env_open" ) == 0 )
    {
        walk.kfd[msgx_lowest_free( walk )] = 0;
        return "<<>>";
    }
    if ( strcmp( op.n, "env_close" ) == 0 )
    {
        walk.kfd[op.f] = -1;
        return tla_sequence( { tla_int( op.f ) } );
    }
    // disconnect
    walk.kdead[op.f] = true;
    return tla_sequence( { tla_int( op.f ) } );
}

// the ops the model's guards enable now
static void msgx_enabled( const msgx_walk_t & walk, std::vector<msgx_op_t> & ops, tla_random_t & random )
{
    ops.clear();
    // main and the fill threads: weighted so a walk forgets and sweeps often
    const int a = 1 + random.below( MSGX_ADDRS );
    ops.push_back( { "mark", a, 0, 0, 0 } );
    ops.push_back( { "mark", 1 + random.below( MSGX_ADDRS ), 0, 0, 0 } );
    ops.push_back( { "forget", 1 + random.below( MSGX_ADDRS ), 0, 0, 0 } );
    ops.push_back( { "update", 0, 0, 0, 0 } );
    ops.push_back( { "sweep", 0, 0, 0, 0 } );
    ops.push_back( { "sweep", 0, 0, 0, 0 } );
    ops.push_back( { "stamp", 1 + random.below( MSGX_ADDRS ), 1 + random.below( MSGX_TX ), 0, 0 } );

    for ( int h = 1; h <= MSGX_HOLDERS; h++ )
    {
        const bool inside = msgx_inside( walk, h );
        if ( !inside )
        {
            ops.push_back( { "holder_begin", 0, h, 0, 0 } );
            continue;
        }
        if ( walk.rep_set[h] )
        {
            ops.push_back( { "report", 0, h, 0, 0 } );
            continue;
        }
        if ( h <= MSGX_TX )
        {
            if ( walk.cur[h] != 0 )
            {
                ops.push_back( { "send", 0, h, 0, 0 } );
                ops.push_back( { "send", 0, h, 0, 0 } );
                continue;
            }
            std::vector<int> queued;
            for ( size_t i = 0; i < walk.pkts.size(); i++ )
            {
                if ( walk.pkts[i].h == h && strcmp( walk.pkts[i].st, "queued" ) == 0 )
                {
                    queued.push_back( (int) i + 1 );
                }
            }
            if ( !queued.empty() )
            {
                ops.push_back( { "resolve", 0, h, queued[(size_t) random.below( (int) queued.size() )], 0 } );
                ops.push_back( { "resolve", 0, h, queued[(size_t) random.below( (int) queued.size() )], 0 } );
            }
            ops.push_back( { "holder_end", 0, h, 0, 0 } );
        }
        else
        {
            if ( walk.rx_set && walk.rx_fd < 0 )
            {
                ops.push_back( { "rx_resolve", 0, h, 0, 0 } );
                continue;
            }
            if ( walk.rx_fd >= 0 )
            {
                ops.push_back( { "rx_read", 0, h, 0, 0 } );
                continue;
            }
            if ( !walk.poller.empty() )
            {
                ops.push_back( { "poll", 0, h, 0, 0 } );
            }
            ops.push_back( { "holder_end", 0, h, 0, 0 } );
        }
    }

    int env = 0;
    for ( int f = 0; f < MSGX_FDS; f++ )
    {
        if ( walk.kfd[f] == 0 )
        {
            env++;
            ops.push_back( { "env_close", 0, 0, 0, f } );
        }
        if ( walk.kfd[f] > 0 && !walk.kdead[f] && random.below( 4 ) == 0 )
        {
            ops.push_back( { "disconnect", 0, 0, 0, f } );
        }
    }
    if ( env < MSGX_MAX_ENV && msgx_lowest_free( walk ) >= 0 )
    {
        ops.push_back( { "env_open", 0, 0, 0, 0 } );
    }
}

// four ipv4 addresses with index homes 2046, 2046, 2047 and 0: a run of
// three that collides and wraps past the last entry
static bool msgx_pick_addresses( msgx_walk_t & walk )
{
    const uint32_t mask = ROCKETNET_MSGX_TABLE_INDEX_SIZE - 1;
    const uint32_t want[MSGX_ADDRS + 1] = { 0, mask - 1, mask - 1, mask, 0 };
    int a = 1;
    for ( int port = 1; port < 65536 && a <= MSGX_ADDRS; port++ )
    {
        struct rocketnet_address_t candidate;
        memset( &candidate, 0, sizeof( candidate ) );
        candidate.type = ROCKETNET_ADDRESS_IPV4;
        candidate.data.ipv4[0] = 10;
        candidate.data.ipv4[3] = 9;
        candidate.port = (uint16_t) port;
        if ( ( rocketnet_msgx_address_hash( &candidate ) & mask ) == want[a] )
        {
            walk.address[a++] = candidate;
        }
    }
    return a > MSGX_ADDRS;
}

static int trace_msgx( uint64_t seed, int steps, const char * out )
{
    msgx_walk_t walk;
    walk.table = (struct rocketnet_msgx_socket_table_t *) calloc( 1, sizeof( struct rocketnet_msgx_socket_table_t ) );
    if ( !walk.table )
    {
        return 1;
    }
    memset( walk.address, 0, sizeof( walk.address ) );
    for ( int f = 0; f < MSGX_FDS; f++ )
    {
        walk.kfd[f] = -1;
        walk.kdead[f] = false;
    }
    memset( walk.cur, 0, sizeof( walk.cur ) );
    memset( walk.rep_set, 0, sizeof( walk.rep_set ) );
    memset( walk.rep_s, 0, sizeof( walk.rep_s ) );
    memset( walk.rep_g, 0, sizeof( walk.rep_g ) );
    walk.rx_set = false;
    memset( &walk.rx_ev, 0, sizeof( walk.rx_ev ) );
    walk.rx_fd = -1;
    walk.crossed = false;
    walk.on_closed = false;
    walk.attempts = 0;
    walk.early_closes = 0;
    if ( !msgx_pick_addresses( walk ) )
    {
        fprintf( stderr, "tla_trace msgx: no addresses with the wanted homes\n" );
        free( walk.table );
        return 1;
    }
    rocketnet_msgx_socket_table_init( walk.table, MSGX_SLOTS, MSGX_HOLDERS, MSGX_BUDGET, msgx_fake_open, msgx_fake_close, &walk );

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", msgx_state( walk ) ) );
    std::vector<msgx_op_t> ops;
    for ( int step = 0; step < steps; step++ )
    {
        msgx_enabled( walk, ops, random );
        const msgx_op_t op = ops[(size_t) random.below( (int) ops.size() )];
        const int early = walk.early_closes;
        const std::string args = msgx_do( walk, op, random );
        const char * act = msgx_act( op.n );
        report.act( act );
        records.push_back( tla_step( op.n, act, args, msgx_state( walk ) ) );
        // the plant's effect, where it shows: a socket closed while its slot is still open
        if ( walk.early_closes != early )
        {
            report.planted( (int) records.size() );
        }
    }
    report.print();

    // HomeAt and CoolAt: a * size + entry for address a on that entry
    std::string home_at = "{";
    std::string cool_at = "{";
    for ( int a = 1; a <= MSGX_ADDRS; a++ )
    {
        const uint32_t hash = rocketnet_msgx_address_hash( &walk.address[a] );
        home_at += ( a > 1 ? ", " : "" ) + tla_int( (int64_t) a * ROCKETNET_MSGX_TABLE_INDEX_SIZE + ( hash & ( ROCKETNET_MSGX_TABLE_INDEX_SIZE - 1 ) ) );
        cool_at += ( a > 1 ? ", " : "" ) +
                   tla_int( (int64_t) a * ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES + rocketnet_msgx_socket_table_cooldown_entry( &walk.address[a] ) );
    }
    home_at += "}";
    cool_at += "}";

    // GenWrap is 2^30: the code's generation wraps at 2^32, past TLC's integers, and the walk's generations stay below its steps
    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    Cap = %d\n    Addrs = {1, 2, 3, 4}\n    NFd = %d\n    TxHolders = {1, 2}\n    RxHolder = 3\n"
              "    IS = %d\n    HomeAt = %s\n    CoolEntries = %d\n    CoolAt = %s\n    Cooldown = %d\n"
              "    Budget = %d\n    GenWrap = 1073741824\n    PassLimit = 1000000000\n"
              "    MaxUpdates = 1000000000\n    MaxPackets = 1000000000\n    MaxEnv = %d\n    Icmp = TRUE\n"
              "    Quarantine = TRUE\n    GenCheck = TRUE\n    SnapshotFirst = FALSE\n    RefusedOpenFree = TRUE\n",
              MSGX_SLOTS, MSGX_FDS, ROCKETNET_MSGX_TABLE_INDEX_SIZE, home_at.c_str(), ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES, cool_at.c_str(),
              ROCKETNET_MSGX_TABLE_DEAD_COOLDOWN_UPDATES, walk.table->opens_per_update, MSGX_MAX_ENV );
    const bool written = tla_write_trace( out, "MCTraceMsgx", seed, "TraceRNMsgxSocketTable", "", constants, records );
    free( walk.table );
    return written ? 0 : 1;
}

TLA_TRACE_LAYER( "msgx", trace_msgx );
