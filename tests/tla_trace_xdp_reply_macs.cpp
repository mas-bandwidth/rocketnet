/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the XDP reply MAC table (sdk/rocketnet_xdp_frame.h)
// against proofs/tla/RNXdpReplyMacsLayout.tla, through
// proofs/tla/TraceRNXdpReplyMacsLayout.tla (proofs/tla/README-TraceCheck.md).
//
// The walk binds the smallest table the code accepts (4 *
// ROCKETNET_XDP_REPLY_MAC_PROBES entries) with no gateway, and drives it with
// ipv4 and ipv6 addresses whose home slots (the code's own FNV hash) all fall
// in a few adjacent slots across the table's end, so probe windows fill, wrap
// and evict. The model runs under HomesGiven: each address is named by its
// home slot, rocketnet_xdp_reply_macs_slot, plus a multiple of the table's
// size, and the model takes each home as the name modulo the size. Each step
// is one call of the code:
//
//   learn   rocketnet_xdp_reply_macs_learn of a usable MAC: Learn; of a
//           multicast or zero MAC: Ignore
//   reply   rocketnet_xdp_reply_mac: Answer when it returns the walk's own
//           answered buffer; ReplyLearned or ReplyAnswered, with the MAC,
//           when it returns an entry's MAC, by that entry's state; Refuse
//           when it returns none, with whether the call passed a frame's MAC
//   forget  rocketnet_xdp_reply_macs_forget: Forget
//   reset   rocketnet_xdp_reply_macs_reset with no gateway: Reset
//
// After each call the record carries every entry, learn_count, each
// address's home and the code's own lookup of each address (abs), so the
// model's contract is checked against what the lookups return.
//
// The plant (ROCKETNET_XDP_REPLY_MACS_PLANT) is the reversed witness
// MCRNXdpReplyMacsLayoutBrokenEvictNewest.cfg: learn's eviction takes the
// entry learned last. Its effect is a learn that took a LIVE entry of
// another address that was not the oldest of the address's full window.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#if defined( ROCKETNET_TLA_PLANT )
#include "../sdk/rocketnet_server_socket_linux.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "tla_trace_record.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_xdp_frame.h"

#define REPLY_MACS_ENTRIES      ( 4 * ROCKETNET_XDP_REPLY_MAC_PROBES )
#define REPLY_MACS_ADDRESSES    24
#define REPLY_MACS_MACS         3
#define REPLY_MACS_HOMES        4

struct reply_macs_walk_t
{
    struct rocketnet_xdp_reply_macs_t table;
    struct rocketnet_xdp_reply_mac_entry_t entries[REPLY_MACS_ENTRIES];
    rocketnet_address_t address[REPLY_MACS_ADDRESSES];
    std::string name[REPLY_MACS_ADDRESSES];
    uint8_t mac[REPLY_MACS_MACS][6];
};

static std::string reply_macs_mac_name( int i )
{
    return tla_string( "m" + tla_int( i + 1 ) );
}

// an entry's address as the model names it: NOADDR for the zeroed address
// of a never-used slot, the walk's own name for a client, else a number the
// model's addresses never are
static std::string reply_macs_entry_address( const reply_macs_walk_t & walk, const struct rocketnet_xdp_reply_mac_entry_t & entry )
{
    if ( entry.state == ROCKETNET_XDP_REPLY_MAC_EMPTY )
    {
        rocketnet_address_t zero;
        memset( &zero, 0, sizeof( zero ) );
        return memcmp( &entry.address, &zero, sizeof( zero ) ) == 0 ? "NOADDR" : tla_int( -2 );
    }
    for ( int i = 0; i < REPLY_MACS_ADDRESSES; i++ )
    {
        if ( rocketnet_address_equal( &entry.address, &walk.address[i] ) )
        {
            return walk.name[i];
        }
    }
    return tla_int( -1 );
}

// a MAC as the model names it: the walk's own MACs by name; the zero bytes a
// reset leaves in a never-used slot are NOMAC
static std::string reply_macs_mac_value( const reply_macs_walk_t & walk, const uint8_t * mac, bool empty )
{
    for ( int i = 0; i < REPLY_MACS_MACS; i++ )
    {
        if ( memcmp( mac, walk.mac[i], 6 ) == 0 )
        {
            return reply_macs_mac_name( i );
        }
    }
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };
    if ( empty && memcmp( mac, zero_mac, 6 ) == 0 )
    {
        return "NOMAC";
    }
    return tla_string( "unknown mac" );
}

static std::string reply_macs_state_name( uint8_t state )
{
    if ( state == ROCKETNET_XDP_REPLY_MAC_EMPTY )
    {
        return tla_string( "empty" );
    }
    if ( state == ROCKETNET_XDP_REPLY_MAC_LIVE )
    {
        return tla_string( "live" );
    }
    if ( state == ROCKETNET_XDP_REPLY_MAC_ANSWERED )
    {
        return tla_string( "answered" );
    }
    return tla_string( "unknown state" );
}

// the code's lookup of one address: its learned MAC, else its answered MAC,
// else none (the model's Lookup)
static std::string reply_macs_lookup( reply_macs_walk_t & walk, int i )
{
    const struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find_state( &walk.table, &walk.address[i], ROCKETNET_XDP_REPLY_MAC_LIVE );
    const char * kind = "live";
    if ( !entry )
    {
        entry = rocketnet_xdp_reply_macs_find_state( &walk.table, &walk.address[i], ROCKETNET_XDP_REPLY_MAC_ANSWERED );
        kind = "answered";
    }
    if ( !entry )
    {
        return "[kind |-> \"none\"]";
    }
    return "[kind |-> " + tla_string( kind ) + ", mac |-> " + reply_macs_mac_value( walk, entry->mac, false ) + "]";
}

static tla_record_t reply_macs_state( reply_macs_walk_t & walk )
{
    std::vector<std::string> addresses, homes, lookups;
    for ( int i = 0; i < REPLY_MACS_ADDRESSES; i++ )
    {
        addresses.push_back( walk.name[i] );
        homes.push_back( tla_int( rocketnet_xdp_reply_macs_slot( &walk.table, &walk.address[i] ) ) );
        lookups.push_back( reply_macs_lookup( walk, i ) );
    }

    std::vector<std::string> slots, states, entry_addresses, macs, stamps;
    for ( int s = 0; s < REPLY_MACS_ENTRIES; s++ )
    {
        const struct rocketnet_xdp_reply_mac_entry_t & entry = walk.entries[s];
        slots.push_back( tla_int( s ) );
        states.push_back( reply_macs_state_name( entry.state ) );
        entry_addresses.push_back( reply_macs_entry_address( walk, entry ) );
        macs.push_back( reply_macs_mac_value( walk, entry.mac, entry.state == ROCKETNET_XDP_REPLY_MAC_EMPTY ) );
        stamps.push_back( tla_int( (int64_t) entry.learned ) );
    }

    tla_record_t state;
    state.add( "home", tla_function( addresses, homes ) );
    state.add( "st", tla_function( slots, states ) );
    state.add( "addr", tla_function( slots, entry_addresses ) );
    state.add( "mac", tla_function( slots, macs ) );
    state.add( "stamp", tla_function( slots, stamps ) );
    state.add( "count", tla_int( (int64_t) walk.table.learn_count ) );
    state.add( "abs", tla_function( addresses, lookups ) );
    state.add( "badEviction", tla_bool( false ) );
    return state;
}

// the planted effect, read from the entries before and after a learn: the
// learn took a LIVE entry of another address, and another LIVE entry of the
// address's window was learned before it
static bool reply_macs_evicted_newer( const reply_macs_walk_t & walk, const struct rocketnet_xdp_reply_mac_entry_t * before, int i )
{
    const uint32_t home = rocketnet_xdp_reply_macs_slot( &walk.table, &walk.address[i] );
    for ( int s = 0; s < REPLY_MACS_ENTRIES; s++ )
    {
        const bool taken = before[s].state == ROCKETNET_XDP_REPLY_MAC_LIVE && !rocketnet_address_equal( &before[s].address, &walk.address[i] ) &&
                           rocketnet_address_equal( &walk.entries[s].address, &walk.address[i] );
        if ( !taken )
        {
            continue;
        }
        for ( int p = 0; p < ROCKETNET_XDP_REPLY_MAC_PROBES; p++ )
        {
            const struct rocketnet_xdp_reply_mac_entry_t & other = before[( home + (uint32_t) p ) & walk.table.mask];
            if ( other.state == ROCKETNET_XDP_REPLY_MAC_LIVE && other.learned < before[s].learned )
            {
                return true;
            }
        }
    }
    return false;
}

static void reply_macs_addresses( reply_macs_walk_t * walk, uint64_t seed )
{
    // REPLY_MACS_ADDRESSES distinct addresses, ipv4 and ipv6 in turn, whose
    // home slots are the REPLY_MACS_HOMES slots ending at slot 1, across the
    // table's end; the search starts where the seed puts it and is
    // deterministic. Address i is named home + N * (i + 1): distinct, and
    // the model's home of it (the name modulo N) is the code's hash slot
    int count = 0;
    for ( uint32_t n = (uint32_t) ( seed * 7919 ) & 0xFFFF; count < REPLY_MACS_ADDRESSES; n++ )
    {
        rocketnet_address_t candidate;
        memset( &candidate, 0, sizeof( candidate ) );
        if ( count % 2 == 0 )
        {
            candidate.type = ROCKETNET_ADDRESS_IPV4;
            candidate.data.ipv4[0] = 10;
            candidate.data.ipv4[1] = (uint8_t) ( n >> 16 );
            candidate.data.ipv4[2] = (uint8_t) ( n >> 8 );
            candidate.data.ipv4[3] = (uint8_t) n;
        }
        else
        {
            candidate.type = ROCKETNET_ADDRESS_IPV6;
            candidate.data.ipv6[0] = 0xfd00;
            candidate.data.ipv6[6] = (uint16_t) ( n >> 16 );
            candidate.data.ipv6[7] = (uint16_t) n;
        }
        candidate.port = 40000;
        const uint32_t home = rocketnet_xdp_reply_macs_slot( &walk->table, &candidate );
        if ( ( ( home + REPLY_MACS_HOMES - 2 ) & walk->table.mask ) < REPLY_MACS_HOMES )
        {
            walk->address[count] = candidate;
            walk->name[count] = tla_int( (int64_t) home + (int64_t) REPLY_MACS_ENTRIES * ( count + 1 ) );
            count++;
        }
    }
}

// the slot whose MAC field a reply returned, -1 for none
static int reply_macs_returned_slot( const reply_macs_walk_t & walk, const uint8_t * reply )
{
    for ( int s = 0; s < REPLY_MACS_ENTRIES; s++ )
    {
        if ( reply == walk.entries[s].mac )
        {
            return s;
        }
    }
    return -1;
}

static int trace_xdp_reply_macs( uint64_t seed, int steps, const char * out )
{
    static reply_macs_walk_t walk;
    memset( &walk.table, 0, sizeof( walk.table ) );
    memset( walk.entries, 0, sizeof( walk.entries ) );
    if ( !rocketnet_xdp_reply_macs_bind( &walk.table, walk.entries, REPLY_MACS_ENTRIES ) )
    {
        fprintf( stderr, "tla_trace: the reply MAC table refused %d entries\n", REPLY_MACS_ENTRIES );
        return 1;
    }
    reply_macs_addresses( &walk, seed );
    for ( int i = 0; i < REPLY_MACS_MACS; i++ )
    {
        const uint8_t mac[6] = { 0x02, 0x00, 0x5e, 0x00, 0x00, (uint8_t) ( i + 1 ) };
        memcpy( walk.mac[i], mac, 6 );
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    static struct rocketnet_xdp_reply_mac_entry_t before[REPLY_MACS_ENTRIES];

    records.push_back( tla_step( "init", "init", "<<>>", reply_macs_state( walk ) ) );
    while ( (int) records.size() < steps + 1 )
    {
        const int roll = random.below( 100 );
        const int a = random.below( REPLY_MACS_ADDRESSES );
        const int m = random.below( REPLY_MACS_MACS );
        std::vector<std::string> args;
        args.push_back( walk.name[a] );
        // one call in sixteen passes a MAC the code must not keep: a learn of
        // a multicast or zero MAC, a reply with one as the answered MAC
        const int unusable = random.below( 16 ) == 0 ? 1 + random.below( 2 ) : 0;
        if ( roll < 60 )
        {
            if ( unusable )
            {
                const uint8_t ignored[6] = { (uint8_t) ( unusable == 1 ? 0x01 : 0x00 ), 0x00, (uint8_t) ( unusable == 1 ? 0x5e : 0x00 ), 0x00, 0x00, 0x00 };
                rocketnet_xdp_reply_macs_learn( &walk.table, &walk.address[a], ignored );
                report.act( "Ignore" );
                records.push_back( tla_step( "learn", "Ignore", tla_sequence( args ), reply_macs_state( walk ) ) );
                continue;
            }
            memcpy( before, walk.entries, sizeof( before ) );
            rocketnet_xdp_reply_macs_learn( &walk.table, &walk.address[a], walk.mac[m] );
            report.act( "Learn" );
            args.push_back( reply_macs_mac_name( m ) );
            records.push_back( tla_step( "learn", "Learn", tla_sequence( args ), reply_macs_state( walk ) ) );
            if ( reply_macs_evicted_newer( walk, before, a ) )
            {
                report.planted( (int) records.size() );
            }
        }
        else if ( roll < 90 )
        {
            // the answered MAC from a buffer of the walk's own, so the return
            // says which branch the code took: the answered MAC kept
            // (Answer), an entry's MAC (ReplyLearned, ReplyAnswered by the
            // entry's state), or none (Refuse); one reply in eight has no
            // answered MAC at all
            uint8_t answered[6];
            memcpy( answered, walk.mac[m], 6 );
            if ( unusable )
            {
                memset( answered, 0, sizeof( answered ) );
                answered[0] = unusable == 1 ? 0x01 : 0x00;
            }
            const bool none = !unusable && random.below( 8 ) == 0;
            uint64_t refused = 0;
            const uint8_t * reply = rocketnet_xdp_reply_mac( &walk.table, &walk.address[a], none ? NULL : answered, &refused );
            const char * act = "Refuse";
            if ( reply == NULL )
            {
                args.push_back( tla_bool( !none ) );
            }
            else if ( reply == answered )
            {
                act = "Answer";
                args.push_back( reply_macs_mac_name( m ) );
            }
            else
            {
                const int s = reply_macs_returned_slot( walk, reply );
                act = s >= 0 && walk.entries[s].state == ROCKETNET_XDP_REPLY_MAC_ANSWERED ? "ReplyAnswered" : "ReplyLearned";
                args.push_back( reply_macs_mac_value( walk, reply, false ) );
            }
            report.act( act );
            records.push_back( tla_step( "reply", act, tla_sequence( args ), reply_macs_state( walk ) ) );
        }
        else if ( roll < 99 )
        {
            rocketnet_xdp_reply_macs_forget( &walk.table, &walk.address[a] );
            report.act( "Forget" );
            records.push_back( tla_step( "forget", "Forget", tla_sequence( args ), reply_macs_state( walk ) ) );
        }
        else
        {
            rocketnet_xdp_reply_macs_reset( &walk.table, NULL );
            report.act( "Reset" );
            records.push_back( tla_step( "reset", "Reset", "<<>>", reply_macs_state( walk ) ) );
        }
    }

    report.print();

    std::string addresses;
    for ( int i = 0; i < REPLY_MACS_ADDRESSES; i++ )
    {
        addresses += ( i > 0 ? ", " : "" ) + walk.name[i];
    }
    std::string macs;
    for ( int i = 0; i < REPLY_MACS_MACS; i++ )
    {
        macs += ( i > 0 ? ", " : "" ) + reply_macs_mac_name( i );
    }
    const std::string constants = "    N = " + tla_int( REPLY_MACS_ENTRIES ) + "\n    P = " + tla_int( ROCKETNET_XDP_REPLY_MAC_PROBES ) +
                                  "\n    Addrs = {" + addresses + "}\n    Macs = {" + macs + "}\n    NOADDR = NOADDR\n    NOMAC = NOMAC\n" +
                                  "    HomesGiven = TRUE\n    SkipOtherEntries = TRUE\n    EvictOldest = TRUE\n    InsertWindowExact = TRUE\n" +
                                  "    LearnTakesEmptyFirst = TRUE\n";
    return tla_write_trace( out, "MCTraceXdpReplyMacs", seed, "TraceRNXdpReplyMacsLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "xdp-reply-macs", trace_xdp_reply_macs );
