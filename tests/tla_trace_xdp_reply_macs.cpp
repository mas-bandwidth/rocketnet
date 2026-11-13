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
// ipv4 and ipv6 addresses whose home slots (the code's own FNV hash) all fall in a few
// adjacent slots across the table's end, so probe windows fill, wrap and
// evict. Each step is one call of the code:
//
//   learn   rocketnet_xdp_reply_macs_learn of a usable MAC: Learn; of a
//           multicast or zero MAC: Ignored, which changes nothing
//   reply   rocketnet_xdp_reply_mac: Answer when it returns the answered MAC
//           (no learned entry); Skip when it returns another MAC the table
//           has (a learned address, or an ANSWERED entry when the call
//           passes no answered MAC); Refused when it returns none. Skip and
//           Refused change nothing; the call's refusal count is an argument
//   forget  rocketnet_xdp_reply_macs_forget: Forget
//   reset   rocketnet_xdp_reply_macs_reset with no gateway: Reset
//
// After each call the record carries every entry, learn_count, each
// address's home and the code's own lookup of each address (abs), so the
// model's contract is checked against what the lookups return.
//
// The plant (ROCKETNET_TLA_PLANT_XDP_REPLY_MACS) is the reversed witness
// MCRNXdpReplyMacsLayoutBrokenEvictNewest.cfg: learn's eviction takes the
// entry learned last. Its effect is a learn that took a LIVE entry of
// another address that was not the oldest of the address's full window.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_TLA_PLANT_XDP_REPLY_MACS 1
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

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
    uint8_t mac[REPLY_MACS_MACS][6];
};

static std::string reply_macs_address_name( int i )
{
    return tla_string( "a" + tla_int( i + 1 ) );
}

static std::string reply_macs_mac_name( int i )
{
    return tla_string( "m" + tla_int( i + 1 ) );
}

// an entry's address as the model names it: NOADDR for the zeroed address
// of a never-used slot, the walk's own name for a client, else a value the
// model never takes
static std::string reply_macs_entry_address( const reply_macs_walk_t & walk, const struct rocketnet_xdp_reply_mac_entry_t & entry )
{
    if ( entry.state == ROCKETNET_XDP_REPLY_MAC_EMPTY )
    {
        rocketnet_address_t zero;
        memset( &zero, 0, sizeof( zero ) );
        return memcmp( &entry.address, &zero, sizeof( zero ) ) == 0 ? "NOADDR" : tla_string( "stale address" );
    }
    for ( int i = 0; i < REPLY_MACS_ADDRESSES; i++ )
    {
        if ( rocketnet_address_equal( &entry.address, &walk.address[i] ) )
        {
            return reply_macs_address_name( i );
        }
    }
    return tla_string( "unknown address" );
}

// a MAC as the model names it: the walk's own MACs by name; the zero bytes a
// reset leaves in a never-used slot are EmptyMac, the model's free choice
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
        return "EmptyMac";
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
        addresses.push_back( reply_macs_address_name( i ) );
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
    // deterministic
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
            walk->address[count++] = candidate;
        }
    }
}

static int trace_xdp_reply_macs( uint64_t seed, int steps, const char * out )
{
    static reply_macs_walk_t walk;
    memset( &walk, 0, sizeof( walk ) );
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

    records.push_back( tla_step( "init", "init", tla_sequence( std::vector<std::string>() ), reply_macs_state( walk ) ) );
    while ( (int) records.size() < steps + 1 )
    {
        const int roll = random.below( 100 );
        const int a = random.below( REPLY_MACS_ADDRESSES );
        const int m = random.below( REPLY_MACS_MACS );
        std::vector<std::string> args;
        // one call in eight passes a MAC the code must not keep: a learn of
        // a multicast or zero MAC, a reply with one as the answered MAC
        const int unusable = random.below( 8 ) == 0 ? 1 + random.below( 2 ) : 0;
        if ( roll < 55 )
        {
            if ( unusable )
            {
                const uint8_t ignored[6] = { (uint8_t) ( unusable == 1 ? 0x01 : 0x00 ), 0x00, (uint8_t) ( unusable == 1 ? 0x5e : 0x00 ), 0x00, 0x00, 0x00 };
                rocketnet_xdp_reply_macs_learn( &walk.table, &walk.address[a], ignored );
                args.push_back( reply_macs_address_name( a ) );
                args.push_back( tla_string( "unusable" ) );
                records.push_back( tla_step( "learn", "Ignored", tla_sequence( args ), reply_macs_state( walk ) ) );
                continue;
            }
            memcpy( before, walk.entries, sizeof( before ) );
            rocketnet_xdp_reply_macs_learn( &walk.table, &walk.address[a], walk.mac[m] );
            report.act( "Learn" );
            args.push_back( reply_macs_address_name( a ) );
            args.push_back( reply_macs_mac_name( m ) );
            records.push_back( tla_step( "learn", "Learn", tla_sequence( args ), reply_macs_state( walk ) ) );
            if ( reply_macs_evicted_newer( walk, before, a ) )
            {
                report.planted( (int) records.size() );
            }
        }
        else if ( roll < 85 )
        {
            // the answered MAC from a buffer of the walk's own, so the return
            // says which branch the code took: the answered MAC kept
            // (Answer), another MAC the table already had (Skip), or none
            // (Refused); one reply in eight has no answered MAC at all
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
            const char * act = reply == NULL ? "Refused" : ( reply == answered ? "Answer" : "Skip" );
            // the census counts the model's actions: Skip, Refused and
            // Ignored change nothing and are no action of Next
            if ( strcmp( act, "Answer" ) == 0 )
            {
                report.act( act );
            }
            args.push_back( reply_macs_address_name( a ) );
            args.push_back( none ? tla_string( "none" ) : ( unusable ? tla_string( "unusable" ) : reply_macs_mac_name( m ) ) );
            args.push_back( tla_int( (int64_t) refused ) );
            records.push_back( tla_step( "reply", act, tla_sequence( args ), reply_macs_state( walk ) ) );
        }
        else if ( roll < 99 )
        {
            rocketnet_xdp_reply_macs_forget( &walk.table, &walk.address[a] );
            report.act( "Forget" );
            args.push_back( reply_macs_address_name( a ) );
            records.push_back( tla_step( "forget", "Forget", tla_sequence( args ), reply_macs_state( walk ) ) );
        }
        else
        {
            rocketnet_xdp_reply_macs_reset( &walk.table, NULL );
            report.act( "Reset" );
            records.push_back( tla_step( "reset", "Reset", tla_sequence( args ), reply_macs_state( walk ) ) );
        }
    }

    report.print();

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceXdpReplyMacsPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceXdpReplyMacsSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    std::vector<std::string> addresses, macs;
    for ( int i = 0; i < REPLY_MACS_ADDRESSES; i++ )
    {
        addresses.push_back( reply_macs_address_name( i ) );
    }
    for ( int i = 0; i < REPLY_MACS_MACS; i++ )
    {
        macs.push_back( reply_macs_mac_name( i ) );
    }
    std::string definitions = "TraceAddrs == {";
    for ( size_t i = 0; i < addresses.size(); i++ )
    {
        definitions += ( i > 0 ? ", " : "" ) + addresses[i];
    }
    definitions += "}\nTraceMacs == {";
    for ( size_t i = 0; i < macs.size(); i++ )
    {
        definitions += ( i > 0 ? ", " : "" ) + macs[i];
    }
    definitions += "}\n";

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    N = %d\n    P = %d\n    Addrs <- TraceAddrs\n    Macs <- TraceMacs\n    NOADDR = NOADDR\n"
              "    SkipOtherEntries = TRUE\n    EvictOldest = TRUE\n    InsertWindowExact = TRUE\n",
              REPLY_MACS_ENTRIES, ROCKETNET_XDP_REPLY_MAC_PROBES );
    return tla_write_trace( out, name, "TraceRNXdpReplyMacsLayout", definitions, constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "xdp-reply-macs", trace_xdp_reply_macs );
