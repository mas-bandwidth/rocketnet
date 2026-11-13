/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of kernel replay protection (proofs/tla/RNXdpReplay.tla,
// TraceRNXdpReplay.tla, README-TraceCheck.md). The walk plays the clients,
// the attacker, the network, the link that holds replies and the kernel's
// answers to the map calls, and drives the code the model describes:
//
//   connect      rocketnet_encryption_map_add, rocketnet_client_slots_assign and
//                rocketnet_xdp_reply_macs_learn (the accept): Connect
//   send         a genuine packet of the live session: ClientSend
//   forge        a packet spoofing an address at any sequence: Forge
//   duplicate    a second copy of a packet on the wire: Duplicate
//   kernel       the shipped XDP filter (sdk/xdp/server_xdp.c, compiled as host
//                C, tests/xdp_host) over the packet's frame and the map the
//                commit writes: KernelFilter
//   open         rocketnet_replay_already_received and rocketnet_replay_advance
//                on the mapping's replay_session, for a genuine packet of the
//                live session (a forged one fails the AEAD open): UserOpen
//   release      rocketnet_client_slots_release and
//                rocketnet_encryption_map_remove_current: Release
//   walk         rocketnet_client_slots_publish, then
//                rocketnet_server_replay_marks_walk with the socket's sink
//                (rocketnet_xdp_replay_sink_forget and _sink_mark): Walk
//   commit       rocketnet_xdp_replay_commit over the walk's map calls, then
//                each live session's greeting as
//                rocketnet_server_socket_hold_greetings decides it: Commit
//   answer       rocketnet_xdp_reply_mac with the frame's MAC (a handshake from
//                an address with no session), held by the link: Answer
//   hold_session a live session's reply held by the link: HoldSessionReply
//   hold_stray   a reply to any address held by the link: HoldStray
//   drain        rocketnet_xdp_reply_mac with no frame in hand: Drain
//
// The map is the host filter's replay table: the commit's delete and update
// calls write it, the filter reads it. The walk's kernel refuses deletes and
// updates at random, by batch or one call an entry.
//
// The filter is built for the trace at a replay window of
// ROCKETNET_TLA_XDP_REPLAY_WINDOW sequences (sdk/rocketnet_xdp_limits.h), not
// the shipped ROCKETNET_HISTORY_PACKETS, so stale copies and forgeries are
// dropped within the walk's few hundred steps; the cfg's W is that window. The
// model's UserOpen reads no window, so the userspace replay window
// (ROCKETNET_REPLAY_WINDOW_BITS, a size of the library's structures) stays
// as the library builds it and is not bound here.
//
// The model's ghosts are the walk's: the session ids, the packets in flight,
// the drops, the failure counts and the replies held and sent. The kernel
// entry's and the pending mark's session are the session whose mark the
// sink queued. Every other variable is read from the code after the call:
// the replay windows' highest_opened, the walk's slot records, the pending
// buffer, the kernel map and the reply MAC table. TraceRNXdpReplay.tla maps
// the code's reply MAC table to the model's (the code drops a queued forget's
// learned MAC in the walk, the model in the commit after it).
//
// The model identifies a slot with its address. The walk keeps the code to
// that: a client connects only when no slot is seen with no connection (so
// the slot it takes is never another address's owed forget) and none is seen
// at its address; a session is released only when no forget of its address
// is pending (a second forget of a pending address coalesces in the buffer,
// which the model's walk does not describe); and a client connects only when
// every live session is greeted (a zero mark the buffer refuses holds every
// keep-alive, the model only that session's).
//
// The plant (ROCKETNET_TLA_PLANT_XDP_REPLAY, sdk/rocketnet_xdp_replay_sink.h)
// is the reversed witness MCRNXdpReplayBrokenFailedForgetDropped.cfg: the
// commit drops every forget the kernel refused. Its effect is a commit after
// which a forget the kernel refused, with no applied mark of its key, is not
// pending; the walk reports it at that record. No socket, no clock.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_TLA_PLANT_XDP_REPLAY 1
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_replay.h"
#include "../sdk/rocketnet_server_replay_marks.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_frame.h"
#include "../sdk/rocketnet_xdp_limits.h"
#include "../sdk/rocketnet_xdp_replay_marks.h"
#include "../sdk/rocketnet_xdp_replay_sink.h"

#if defined( ROCKETNET_XDP_FILTER_HOST )
#include "xdp_host/server_xdp_host.h"
#endif // #if defined( ROCKETNET_XDP_FILTER_HOST )

#if defined( ROCKETNET_XDP_FILTER_HOST )

// the filter and the cfg's W at the trace's narrow window (CMakeLists.txt,
// rocketnet_xdp_filter_host_trace), so the walk reaches the drop rule
#if !defined( ROCKETNET_TLA_XDP_REPLAY_WINDOW )
#error "the xdp-replay walk needs ROCKETNET_TLA_XDP_REPLAY_WINDOW, the window of the host filter"
#endif // #if !defined( ROCKETNET_TLA_XDP_REPLAY_WINDOW )

namespace
{
    const int RP_ADDRESSES = 3;                 // the model's Addresses
    const int RP_SLOTS = 2 * RP_ADDRESSES;      // client slots, with room for the ones a release holds
    const uint32_t RP_CAPACITY = 2;             // the pending buffer: ForgetCapacity, and the mark list's room
    const int RP_SESSIONS = 24;                 // Sessions: the connections over the run
    const int RP_MAX_IN_FLIGHT = 4;             // MaxInFlight
    const int RP_MAX_SEQ = 100000;              // MaxSeq
    const int RP_REPLY_ENTRIES = 4 * ROCKETNET_XDP_REPLY_MAC_PROBES;
    const int RP_PACKET_BYTES = 100;            // a payload packet: the prefilter header, the sequence, a zero body
    const int RP_VERDICT_DROP = 1;              // XDP_DROP
    const int RP_VERDICT_REDIRECT = 4;          // XDP_REDIRECT
    const uint8_t RP_SERVER_IPV4[4] = { 192, 0, 2, 10 };
    const uint8_t RP_CLIENT_IPV4[4] = { 198, 51, 100, 7 };
    const uint16_t RP_SERVER_PORT = 40000;
    const uint16_t RP_CLIENT_PORT = 50000;
    const uint8_t RP_FILTER[4] = { 0xb7, 0x59, 0x01, 0x00 };

    struct rp_packet_t
    {
        int addr;
        int seq;
        int session;
        bool genuine;
        bool user;          // past the kernel
        int copy;
    };

    bool rp_packet_equal( const rp_packet_t & a, const rp_packet_t & b )
    {
        return a.addr == b.addr && a.seq == b.seq && a.session == b.session && a.genuine == b.genuine && a.user == b.user && a.copy == b.copy;
    }

    struct rp_drop_t
    {
        int addr;
        int seq;
        int session;
        int against;
    };

    struct rp_walk_t
    {
        // the code
        rocketnet_encryption_map_t * map;
        rocketnet_client_slots_t * slots;
        rocketnet_server_replay_marks_t * replay;
        rocketnet_xdp_replay_marks_t marks;
        rocketnet_server_xdp_replay_key_t forgets[RP_CAPACITY];
        rocketnet_server_xdp_replay_key_t mark_keys[RP_CAPACITY];
        rocketnet_server_xdp_replay_value_t mark_values[RP_CAPACITY];
        bool forget_failed[RP_CAPACITY];
        bool mark_applied[RP_CAPACITY];
        bool zero_mark_refused;
        bool hold_all_greetings;
        rocketnet_xdp_reply_macs_t macs;
        rocketnet_xdp_reply_mac_entry_t entries[RP_REPLY_ENTRIES];
        rocketnet_address_t address[RP_ADDRESSES];
        rocketnet_server_xdp_replay_key_t key[RP_ADDRESSES];

        // the walk's world and the model's ghosts
        int session[RP_ADDRESSES];
        int client[RP_ADDRESSES];
        int mapping[RP_ADDRESSES];
        int next_session;
        int sent[RP_ADDRESSES];
        int reached[RP_SESSIONS + 1];
        bool seen_before_release[RP_ADDRESSES];
        bool greeted[RP_ADDRESSES];
        int kernel_sess[RP_ADDRESSES];
        int mark_sess[RP_ADDRESSES];
        bool mac_forget[RP_ADDRESSES];
        bool flush;
        std::vector<rp_packet_t> net;
        std::vector<rp_drop_t> dropped;
        int forget_failures;
        int mark_failures;
        bool heard[RP_ADDRESSES];
        bool held[RP_ADDRESSES];
        int held_replies;
        bool sent_to[RP_ADDRESSES][2];
        bool refused_held[RP_ADDRESSES][2];

        // the kernel's answers for the commit in progress
        bool refuse_forget[RP_ADDRESSES];
        bool refuse_mark[RP_ADDRESSES];
        bool batches;
        bool kernel_refused_forget[RP_ADDRESSES];
    };

    rp_walk_t walk;

    std::string rp_name( int a )
    {
        return tla_string( "a" + tla_int( a + 1 ) );
    }

    int rp_address_of_key( const rocketnet_server_xdp_replay_key_t & key )
    {
        for ( int a = 0; a < RP_ADDRESSES; a++ )
        {
            if ( memcmp( &walk.key[a], &key, sizeof( key ) ) == 0 )
            {
                return a;
            }
        }
        fprintf( stderr, "tla_trace: xdp-replay: a key the walk never made\n" );
        tla_value_errors()++;
        return 0;
    }

    int rp_address_of( const rocketnet_address_t & address )
    {
        for ( int a = 0; a < RP_ADDRESSES; a++ )
        {
            if ( rocketnet_address_equal( &walk.address[a], &address ) )
            {
                return a;
            }
        }
        fprintf( stderr, "tla_trace: xdp-replay: an address the walk never made\n" );
        tla_value_errors()++;
        return 0;
    }

    // the walk's slot record for an address: the slot seen at it (seen false
    // when no slot is)

    struct rp_record_t
    {
        bool seen;
        uint32_t marked;
        bool owed;
    };

    rp_record_t rp_record( int a )
    {
        rp_record_t record = { false, 0, false };
        int found = 0;
        for ( int i = 0; i < RP_SLOTS; i++ )
        {
            bool has_mark = false;
            uint32_t marked_value = 0;
            bool seen = false;
            rocketnet_address_t seen_address;
            bool mark_owed = false;
            if ( !rocketnet_server_replay_marks_slot_for_test( walk.replay, i, &has_mark, &marked_value, &seen, &seen_address, &mark_owed ) )
            {
                fprintf( stderr, "tla_trace: xdp-replay: the walk's slot record refused slot %d\n", i );
                tla_value_errors()++;
            }
            if ( seen && rocketnet_address_equal( &seen_address, &walk.address[a] ) )
            {
                record.seen = true;
                record.marked = has_mark ? marked_value : 0;
                record.owed = mark_owed;
                found++;
            }
        }
        if ( found > 1 )
        {
            fprintf( stderr, "tla_trace: xdp-replay: two slots seen at one address\n" );
            tla_value_errors()++;
        }
        return record;
    }

    // a slot seen with no connection: a forget the walk still owes

    bool rp_owed_slot()
    {
        for ( int i = 0; i < RP_SLOTS; i++ )
        {
            bool has_mark = false;
            uint32_t marked_value = 0;
            bool seen = false;
            rocketnet_address_t seen_address;
            bool mark_owed = false;
            if ( !rocketnet_server_replay_marks_slot_for_test( walk.replay, i, &has_mark, &marked_value, &seen, &seen_address, &mark_owed ) )
            {
                fprintf( stderr, "tla_trace: xdp-replay: the walk's slot record refused slot %d\n", i );
                tla_value_errors()++;
            }
            if ( seen && !rocketnet_client_slots_connected( walk.slots, i ) )
            {
                return true;
            }
        }
        return false;
    }

    bool rp_forget_pending( int a )
    {
        for ( uint32_t i = 0; i < walk.marks.forget_count; i++ )
        {
            if ( memcmp( &walk.marks.forgets[i], &walk.key[a], sizeof( walk.key[a] ) ) == 0 )
            {
                return true;
            }
        }
        return false;
    }

    bool rp_mark_pending( int a )
    {
        for ( uint32_t i = 0; i < walk.marks.mark_count; i++ )
        {
            if ( memcmp( &walk.marks.mark_keys[i], &walk.key[a], sizeof( walk.key[a] ) ) == 0 )
            {
                return true;
            }
        }
        return false;
    }

    uint64_t rp_opened( int a )
    {
        if ( walk.session[a] == 0 )
        {
            return 0;
        }
        return rocketnet_encryption_map_entry( walk.map, walk.mapping[a] )->replay_session.highest_opened;
    }

    std::string rp_packet_text( const rp_packet_t & p )
    {
        return "[addr |-> " + rp_name( p.addr ) + ", seq |-> " + tla_int( p.seq ) + ", session |-> " + tla_int( p.session ) +
               ", genuine |-> " + tla_bool( p.genuine ) + ", stage |-> " + tla_string( p.user ? "user" : "wire" ) + ", copy |-> " + tla_int( p.copy ) + "]";
    }

    std::string rp_set( const std::vector<std::string> & items )
    {
        std::string text = "{";
        for ( size_t i = 0; i < items.size(); i++ )
        {
            text += ( i > 0 ? ", " : "" ) + items[i];
        }
        return text + "}";
    }

    std::string rp_address_function( const std::vector<std::string> & values )
    {
        std::vector<std::string> keys;
        for ( int a = 0; a < RP_ADDRESSES; a++ )
        {
            keys.push_back( rp_name( a ) );
        }
        return tla_function( keys, values );
    }

    tla_record_t rp_state()
    {
        std::vector<std::string> session, sent, opened, marked, seen, before, reply_mac, kmap, greeted, owed, answered, heard;
        std::vector<std::string> pending_forget, pending_mark, mac_forget, held, sent_to, refused_held;
        for ( int a = 0; a < RP_ADDRESSES; a++ )
        {
            const rp_record_t record = rp_record( a );
            session.push_back( tla_int( walk.session[a] ) );
            sent.push_back( tla_int( walk.sent[a] ) );
            opened.push_back( tla_int( (int64_t) rp_opened( a ) ) );
            marked.push_back( tla_int( record.marked ) );
            seen.push_back( tla_bool( record.seen ) );
            before.push_back( tla_bool( walk.seen_before_release[a] ) );
            owed.push_back( tla_bool( record.owed ) );

            // the learned MAC names the session that learned it
            const rocketnet_xdp_reply_mac_entry_t * learned = rocketnet_xdp_reply_macs_find( &walk.macs, &walk.address[a] );
            reply_mac.push_back( tla_int( learned ? learned->mac[5] : 0 ) );
            answered.push_back( tla_bool( rocketnet_xdp_reply_macs_find_state( &walk.macs, &walk.address[a], ROCKETNET_XDP_REPLY_MAC_ANSWERED ) != NULL ) );

            uint32_t high_water = 0;
            if ( rocketnet_xdp_host_get_replay_entry( &walk.key[a], &high_water ) )
            {
                kmap.push_back( "[hw |-> " + tla_int( high_water ) + ", sess |-> " + tla_int( walk.kernel_sess[a] ) + "]" );
            }
            else
            {
                kmap.push_back( "[hw |-> 0, sess |-> 0]" );
            }

            greeted.push_back( tla_bool( walk.greeted[a] ) );
            heard.push_back( tla_bool( walk.heard[a] ) );
            if ( walk.mac_forget[a] )
            {
                mac_forget.push_back( rp_name( a ) );
            }
            if ( walk.held[a] )
            {
                held.push_back( rp_name( a ) );
            }
            for ( int h = 0; h < 2; h++ )
            {
                const std::string entry = "[addr |-> " + rp_name( a ) + ", heard |-> " + tla_bool( h != 0 ) + "]";
                if ( walk.sent_to[a][h] )
                {
                    sent_to.push_back( entry );
                }
                if ( walk.refused_held[a][h] )
                {
                    refused_held.push_back( entry );
                }
            }
        }
        for ( uint32_t i = 0; i < walk.marks.forget_count; i++ )
        {
            pending_forget.push_back( rp_name( rp_address_of_key( walk.marks.forgets[i] ) ) );
        }
        for ( uint32_t i = 0; i < walk.marks.mark_count; i++ )
        {
            const int a = rp_address_of_key( walk.marks.mark_keys[i] );
            pending_mark.push_back( "[addr |-> " + rp_name( a ) + ", hw |-> " + tla_int( walk.marks.mark_values[i].high_water ) + ", sess |-> " +
                                    tla_int( walk.mark_sess[a] ) + "]" );
        }

        std::vector<std::string> reached_keys, reached_values;
        for ( int s = 1; s <= RP_SESSIONS; s++ )
        {
            reached_keys.push_back( tla_int( s ) );
            reached_values.push_back( tla_int( walk.reached[s] ) );
        }

        std::vector<std::string> net;
        for ( size_t i = 0; i < walk.net.size(); i++ )
        {
            net.push_back( rp_packet_text( walk.net[i] ) );
        }
        std::vector<std::string> dropped;
        for ( size_t i = 0; i < walk.dropped.size(); i++ )
        {
            const rp_drop_t & d = walk.dropped[i];
            dropped.push_back( "[addr |-> " + rp_name( d.addr ) + ", seq |-> " + tla_int( d.seq ) + ", session |-> " + tla_int( d.session ) +
                               ", against |-> " + tla_int( d.against ) + "]" );
        }

        tla_record_t state;
        state.add( "session", rp_address_function( session ) );
        state.add( "nextSession", tla_int( walk.next_session ) );
        state.add( "sent", rp_address_function( sent ) );
        state.add( "opened", rp_address_function( opened ) );
        state.add( "reached", tla_function( reached_keys, reached_values ) );
        state.add( "marked", rp_address_function( marked ) );
        state.add( "seen", rp_address_function( seen ) );
        state.add( "seenBeforeRelease", rp_address_function( before ) );
        state.add( "replyMac", rp_address_function( reply_mac ) );
        state.add( "kmap", rp_address_function( kmap ) );
        state.add( "net", rp_set( net ) );
        state.add( "pendingForget", rp_set( pending_forget ) );
        state.add( "pendingMark", rp_set( pending_mark ) );
        state.add( "pendingMacForget", rp_set( mac_forget ) );
        state.add( "phase", tla_string( walk.flush ? "flush" : "run" ) );
        state.add( "dropped", rp_set( dropped ) );
        state.add( "forgetFailures", tla_int( walk.forget_failures ) );
        state.add( "greeted", rp_address_function( greeted ) );
        state.add( "markFailures", tla_int( walk.mark_failures ) );
        state.add( "markOwed", rp_address_function( owed ) );
        state.add( "answered", rp_address_function( answered ) );
        state.add( "heard", rp_address_function( heard ) );
        state.add( "held", rp_set( held ) );
        state.add( "heldReplies", tla_int( walk.held_replies ) );
        state.add( "sentTo", rp_set( sent_to ) );
        state.add( "refusedHeld", rp_set( refused_held ) );
        return state;
    }

    // the sink the transport's walk calls: the socket's own forget and mark

    bool rp_sink_forget( void * context, const rocketnet_address_t * address )
    {
        rp_walk_t * w = (rp_walk_t *) context;
        const bool queued = rocketnet_xdp_replay_sink_forget( &w->marks, &w->macs, address );
        if ( queued )
        {
            w->mac_forget[rp_address_of( *address )] = true;
        }
        return queued;
    }

    bool rp_sink_mark( void * context, const rocketnet_address_t * address, uint32_t high_water )
    {
        rp_walk_t * w = (rp_walk_t *) context;
        const bool queued = rocketnet_xdp_replay_sink_mark( &w->marks, address, high_water, &w->zero_mark_refused );
        const int a = rp_address_of( *address );
        if ( queued )
        {
            w->mark_sess[a] = w->session[a];
        }
        else
        {
            w->mark_failures++;
        }
        return queued;
    }

    // the kernel's map calls: a batch stops at a refused entry or, for a
    // delete, a missing key; a single delete of a missing key succeeds
    // (ENOENT); with batches off the batch command is missing and every entry
    // takes a single call

    bool rp_delete_single( void * context, uint32_t index )
    {
        rp_walk_t * w = (rp_walk_t *) context;
        const int a = rp_address_of_key( w->marks.forgets[index] );
        if ( w->refuse_forget[a] )
        {
            w->kernel_refused_forget[a] = true;
            w->forget_failures++;
            return false;
        }
        rocketnet_xdp_host_delete_replay_entry( &w->marks.forgets[index] );
        return true;
    }

    bool rp_delete_batch( void * context, uint32_t first, uint32_t * batch_count )
    {
        rp_walk_t * w = (rp_walk_t *) context;
        if ( !w->batches )
        {
            return false;
        }
        const uint32_t count = *batch_count;
        for ( uint32_t i = 0; i < count; i++ )
        {
            const int a = rp_address_of_key( w->marks.forgets[first + i] );
            uint32_t high_water = 0;
            if ( w->refuse_forget[a] || !rocketnet_xdp_host_get_replay_entry( &w->marks.forgets[first + i], &high_water ) )
            {
                *batch_count = i;
                return false;
            }
            rocketnet_xdp_host_delete_replay_entry( &w->marks.forgets[first + i] );
        }
        return true;
    }

    bool rp_update_single( void * context, uint32_t index )
    {
        rp_walk_t * w = (rp_walk_t *) context;
        const int a = rp_address_of_key( w->marks.mark_keys[index] );
        if ( w->refuse_mark[a] )
        {
            w->mark_failures++;
            return false;
        }
        rocketnet_xdp_host_set_replay_entry( &w->marks.mark_keys[index], w->marks.mark_values[index].high_water );
        w->kernel_sess[a] = w->mark_sess[a];
        return true;
    }

    bool rp_update_batch( void * context, uint32_t first, uint32_t * batch_count )
    {
        rp_walk_t * w = (rp_walk_t *) context;
        if ( !w->batches )
        {
            return false;
        }
        const uint32_t count = *batch_count;
        for ( uint32_t i = 0; i < count; i++ )
        {
            const int a = rp_address_of_key( w->marks.mark_keys[first + i] );
            if ( w->refuse_mark[a] )
            {
                *batch_count = i;
                return false;
            }
            rocketnet_xdp_host_set_replay_entry( &w->marks.mark_keys[first + i], w->marks.mark_values[first + i].high_water );
            w->kernel_sess[a] = w->mark_sess[a];
        }
        return true;
    }

    // the frame of a packet as the client sends it: Ethernet, IPv4, UDP to
    // the server's address and port, a payload packet of the server's
    // filter epoch and value carrying the sequence

    std::vector<uint8_t> rp_frame( const rp_packet_t & p )
    {
        std::vector<uint8_t> out;
        const uint8_t server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
        const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x01, (uint8_t) p.addr };
        out.insert( out.end(), server_mac, server_mac + 6 );
        out.insert( out.end(), client_mac, client_mac + 6 );
        out.push_back( 0x08 );
        out.push_back( 0x00 );

        const int udp_bytes = 8 + RP_PACKET_BYTES;
        const int ip_bytes = 20 + udp_bytes;
        const uint8_t ip[12] = { 0x45, 0x00, (uint8_t) ( ip_bytes >> 8 ), (uint8_t) ( ip_bytes & 0xff ), 0x00, 0x00, 0x40, 0x00, 64, 17, 0x00, 0x00 };
        out.insert( out.end(), ip, ip + 12 );
        out.insert( out.end(), RP_CLIENT_IPV4, RP_CLIENT_IPV4 + 4 );
        out.insert( out.end(), RP_SERVER_IPV4, RP_SERVER_IPV4 + 4 );

        const uint16_t source_port = (uint16_t) ( RP_CLIENT_PORT + p.addr );
        const uint8_t udp[8] = { (uint8_t) ( source_port >> 8 ), (uint8_t) ( source_port & 0xff ), (uint8_t) ( RP_SERVER_PORT >> 8 ),
                                 (uint8_t) ( RP_SERVER_PORT & 0xff ), (uint8_t) ( udp_bytes >> 8 ), (uint8_t) ( udp_bytes & 0xff ), 0x00, 0x00 };
        out.insert( out.end(), udp, udp + 8 );

        const uint32_t sequence = (uint32_t) p.seq;
        const uint8_t header[10] = { ROCKETNET_CONNECTION_PAYLOAD_PACKET, 0, RP_FILTER[0], RP_FILTER[1], RP_FILTER[2], RP_FILTER[3],
                                     (uint8_t) ( sequence & 0xff ), (uint8_t) ( ( sequence >> 8 ) & 0xff ), (uint8_t) ( ( sequence >> 16 ) & 0xff ),
                                     (uint8_t) ( ( sequence >> 24 ) & 0xff ) };
        for ( int i = 0; i < RP_PACKET_BYTES; i++ )
        {
            out.push_back( i < 10 ? header[i] : 0x00 );
        }
        return out;
    }

    bool rp_in_net( const rp_packet_t & p )
    {
        for ( size_t i = 0; i < walk.net.size(); i++ )
        {
            if ( rp_packet_equal( walk.net[i], p ) )
            {
                return true;
            }
        }
        return false;
    }

    bool rp_all_greeted()
    {
        for ( int a = 0; a < RP_ADDRESSES; a++ )
        {
            if ( walk.session[a] != 0 && !walk.greeted[a] )
            {
                return false;
            }
        }
        return true;
    }

    void rp_reset()
    {
        if ( walk.map )
        {
            rocketnet_encryption_map_destroy( walk.map );
        }
        if ( walk.slots )
        {
            rocketnet_client_slots_destroy( walk.slots );
        }
        if ( walk.replay )
        {
            rocketnet_server_replay_marks_destroy( walk.replay );
        }
        walk.map = rocketnet_encryption_map_create( NULL, RP_SLOTS );
        walk.slots = rocketnet_client_slots_create( NULL, RP_SLOTS );
        walk.replay = rocketnet_server_replay_marks_create( NULL, RP_SLOTS );

        rocketnet_xdp_replay_marks_bind( &walk.marks, walk.forgets, walk.mark_keys, walk.mark_values, RP_CAPACITY );
        memset( walk.forget_failed, 0, sizeof( walk.forget_failed ) );
        memset( walk.mark_applied, 0, sizeof( walk.mark_applied ) );
        walk.zero_mark_refused = false;
        walk.hold_all_greetings = false;
        rocketnet_xdp_reply_macs_bind( &walk.macs, walk.entries, RP_REPLY_ENTRIES );

        for ( int a = 0; a < RP_ADDRESSES; a++ )
        {
            rocketnet_address_t * address = &walk.address[a];
            memset( address, 0, sizeof( *address ) );
            address->type = ROCKETNET_ADDRESS_IPV4;
            memcpy( address->data.ipv4, RP_CLIENT_IPV4, 4 );
            address->port = (uint16_t) ( RP_CLIENT_PORT + a );
            rocketnet_xdp_replay_key_from_address( address, &walk.key[a] );
            walk.session[a] = 0;
            walk.client[a] = -1;
            walk.mapping[a] = -1;
            walk.sent[a] = 0;
            walk.seen_before_release[a] = false;
            walk.greeted[a] = false;
            walk.kernel_sess[a] = 0;
            walk.mark_sess[a] = 0;
            walk.mac_forget[a] = false;
            walk.heard[a] = false;
            walk.held[a] = false;
            walk.sent_to[a][0] = walk.sent_to[a][1] = false;
            walk.refused_held[a][0] = walk.refused_held[a][1] = false;
        }
        for ( int s = 0; s <= RP_SESSIONS; s++ )
        {
            walk.reached[s] = 0;
        }
        walk.next_session = 1;
        walk.flush = false;
        walk.net.clear();
        walk.dropped.clear();
        walk.forget_failures = 0;
        walk.mark_failures = 0;
        walk.held_replies = 0;

        // the filter: the server's address, port, epoch 0 and filter value,
        // and a socket for queue 0, so a packet it passes is redirected

        rocketnet_xdp_host_reset();
        struct rocketnet_server_xdp_config_t config;
        memset( &config, 0, sizeof( config ) );
        memcpy( &config.public_address, RP_SERVER_IPV4, 4 );
        const uint8_t port_bytes[2] = { (uint8_t) ( RP_SERVER_PORT >> 8 ), (uint8_t) ( RP_SERVER_PORT & 0xff ) };
        memcpy( &config.public_port, port_bytes, 2 );
        memcpy( config.packet_filter, RP_FILTER, 4 );
        rocketnet_xdp_host_set_config( &config );
        rocketnet_xdp_host_set_socket_slot( 0, 1 );
    }

    std::string rp_args( int a )
    {
        std::vector<std::string> args;
        args.push_back( rp_name( a ) );
        return tla_sequence( args );
    }

    std::string rp_args( int a, const std::string & second )
    {
        std::vector<std::string> args;
        args.push_back( rp_name( a ) );
        args.push_back( second );
        return tla_sequence( args );
    }

    std::string rp_args_packet( const rp_packet_t & p )
    {
        std::vector<std::string> args;
        args.push_back( rp_packet_text( p ) );
        return tla_sequence( args );
    }

    std::string rp_args_packet( const rp_packet_t & p, const std::string & second )
    {
        std::vector<std::string> args;
        args.push_back( rp_packet_text( p ) );
        args.push_back( second );
        return tla_sequence( args );
    }
}

static int trace_xdp_replay( uint64_t seed, int steps, const char * out )
{
    if ( rocketnet_xdp_host_replay_window() != (uint32_t) ROCKETNET_XDP_REPLAY_WINDOW )
    {
        fprintf( stderr, "tla_trace: xdp-replay: the host filter's replay window is %u, the walk's %u\n", (unsigned) rocketnet_xdp_host_replay_window(),
                 (unsigned) ROCKETNET_XDP_REPLAY_WINDOW );
        return 1;
    }

    rp_reset();

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    std::vector<int> planted;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", tla_sequence( std::vector<std::string>() ), rp_state() ) );
    int idle = 0;
    while ( (int) records.size() < steps + 1 )
    {
        if ( idle > 10000 )
        {
            fprintf( stderr, "tla_trace: xdp-replay: the walk found no step to take\n" );
            return 1;
        }
        idle++;

        const int a = random.below( RP_ADDRESSES );
        const int choice = random.below( 100 );

        if ( !walk.flush && choice < 8 )
        {
            // connect: a free slot never another address's owed forget, no
            // slot seen at this address, every live session greeted
            if ( walk.session[a] != 0 || walk.next_session > RP_SESSIONS || rp_owed_slot() || rp_record( a ).seen || !rp_all_greeted() )
            {
                continue;
            }
            const int s = walk.next_session;
            const int mapping = rocketnet_encryption_map_add( walk.map, &walk.address[a], 0.0 );
            if ( mapping < 0 )
            {
                fprintf( stderr, "tla_trace: xdp-replay: the encryption map refused a mapping\n" );
                return 1;
            }
            uint8_t user[ROCKETNET_USER_DATA_BYTES];
            memset( user, 0, sizeof( user ) );
            const int client = rocketnet_client_slots_assign( walk.slots, (uint64_t) s, mapping, rocketnet_encryption_map_owner_generation( walk.map, mapping ), user );
            if ( client < 0 )
            {
                fprintf( stderr, "tla_trace: xdp-replay: no client slot\n" );
                return 1;
            }
            const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, (uint8_t) s };
            rocketnet_xdp_reply_macs_learn( &walk.macs, &walk.address[a], mac );
            walk.session[a] = s;
            walk.client[a] = client;
            walk.mapping[a] = mapping;
            walk.next_session++;
            walk.sent[a] = 0;
            walk.greeted[a] = false;
            walk.heard[a] = true;
            records.push_back( tla_step( "connect", "Connect", rp_args( a ), rp_state() ) );
        }
        else if ( choice < 30 )
        {
            // a genuine packet of the live session, once greeted
            if ( walk.session[a] == 0 || !walk.greeted[a] || walk.sent[a] >= RP_MAX_SEQ || (int) walk.net.size() >= RP_MAX_IN_FLIGHT )
            {
                continue;
            }
            rp_packet_t p = { a, walk.sent[a] + 1, walk.session[a], true, false, 1 };
            walk.sent[a]++;
            walk.net.push_back( p );
            records.push_back( tla_step( "send", "ClientSend", rp_args( a ), rp_state() ) );
        }
        else if ( choice < 36 )
        {
            // a forgery: behind, at or ahead of what the session sent; three
            // times in four, where the kernel holds a mark more than the window
            // ahead at some address, a replay to that address the window puts
            // behind the mark (so the walk reaches the drop rule)
            if ( (int) walk.net.size() >= RP_MAX_IN_FLIGHT )
            {
                continue;
            }
            int forged = a;
            int seq = 1 + random.below( walk.sent[a] + 3 );
            if ( random.below( 4 ) != 0 )
            {
                std::vector<int> behind;
                std::vector<uint32_t> behind_high_water;
                for ( int b = 0; b < RP_ADDRESSES; b++ )
                {
                    uint32_t kernel_high_water = 0;
                    if ( rocketnet_xdp_host_get_replay_entry( &walk.key[b], &kernel_high_water ) && kernel_high_water > (uint32_t) ROCKETNET_XDP_REPLAY_WINDOW )
                    {
                        behind.push_back( b );
                        behind_high_water.push_back( kernel_high_water );
                    }
                }
                if ( !behind.empty() )
                {
                    const size_t k = (size_t) random.below( (int) behind.size() );
                    forged = behind[k];
                    seq = 1 + random.below( (int) ( behind_high_water[k] - (uint32_t) ROCKETNET_XDP_REPLAY_WINDOW ) );
                }
            }
            rp_packet_t p = { forged, seq, walk.session[forged], false, false, 1 };
            if ( rp_in_net( p ) )
            {
                continue;
            }
            walk.net.push_back( p );
            records.push_back( tla_step( "forge", "Forge", rp_args( forged, tla_int( seq ) ), rp_state() ) );
        }
        else if ( choice < 40 )
        {
            if ( walk.net.empty() || (int) walk.net.size() >= RP_MAX_IN_FLIGHT )
            {
                continue;
            }
            const rp_packet_t p = walk.net[(size_t) random.below( (int) walk.net.size() )];
            rp_packet_t copy = p;
            copy.copy = 2;
            if ( p.user || p.copy != 1 || rp_in_net( copy ) )
            {
                continue;
            }
            walk.net.push_back( copy );
            records.push_back( tla_step( "duplicate", "Duplicate", rp_args_packet( p ), rp_state() ) );
        }
        else if ( choice < 58 )
        {
            // the kernel: the shipped filter over the packet's frame
            std::vector<size_t> wire;
            for ( size_t i = 0; i < walk.net.size(); i++ )
            {
                if ( !walk.net[i].user )
                {
                    wire.push_back( i );
                }
            }
            if ( wire.empty() )
            {
                continue;
            }
            const size_t index = wire[(size_t) random.below( (int) wire.size() )];
            const rp_packet_t p = walk.net[index];
            rp_packet_t passed = p;
            passed.user = true;
            if ( rp_in_net( passed ) )
            {
                continue;
            }
            const std::vector<uint8_t> frame = rp_frame( p );
            const int verdict = rocketnet_xdp_host_run( frame.data(), (int) frame.size(), 0 );
            if ( verdict == RP_VERDICT_DROP )
            {
                walk.net.erase( walk.net.begin() + (std::ptrdiff_t) index );
                if ( p.genuine )
                {
                    uint32_t high_water = 0;
                    const int against = rocketnet_xdp_host_get_replay_entry( &walk.key[p.addr], &high_water ) ? walk.kernel_sess[p.addr] : 0;
                    rp_drop_t d = { p.addr, p.seq, p.session, against };
                    bool known = false;
                    for ( size_t i = 0; i < walk.dropped.size(); i++ )
                    {
                        known = known || ( walk.dropped[i].addr == d.addr && walk.dropped[i].seq == d.seq && walk.dropped[i].session == d.session &&
                                           walk.dropped[i].against == d.against );
                    }
                    if ( !known )
                    {
                        walk.dropped.push_back( d );
                    }
                }
            }
            else if ( verdict == RP_VERDICT_REDIRECT )
            {
                walk.net[index] = passed;
            }
            else
            {
                fprintf( stderr, "tla_trace: xdp-replay: the filter answered %d, neither drop nor redirect\n", verdict );
                return 1;
            }
            records.push_back( tla_step( "kernel", "KernelFilter", rp_args_packet( p, tla_string( verdict == RP_VERDICT_DROP ? "drop" : "pass" ) ), rp_state() ) );
        }
        else if ( choice < 74 )
        {
            if ( walk.flush )
            {
                continue;
            }
            std::vector<size_t> user;
            for ( size_t i = 0; i < walk.net.size(); i++ )
            {
                if ( walk.net[i].user )
                {
                    user.push_back( i );
                }
            }
            if ( user.empty() )
            {
                continue;
            }
            const size_t index = user[(size_t) random.below( (int) user.size() )];
            const rp_packet_t p = walk.net[index];
            walk.net.erase( walk.net.begin() + (std::ptrdiff_t) index );

            // a genuine packet of the live session opens; a forged one fails
            // the AEAD open and one of an ended session has no mapping
            if ( p.genuine && p.session != 0 && p.session == walk.session[p.addr] )
            {
                struct rocketnet_replay_protection_t * replay = &rocketnet_encryption_map_entry( walk.map, walk.mapping[p.addr] )->replay_session;
                if ( !rocketnet_replay_already_received( replay, (uint64_t) p.seq ) )
                {
                    rocketnet_replay_advance( replay, (uint64_t) p.seq );
                }
                if ( (int) replay->highest_opened > walk.reached[p.session] )
                {
                    walk.reached[p.session] = (int) replay->highest_opened;
                }
            }
            records.push_back( tla_step( "open", "UserOpen", rp_args_packet( p ), rp_state() ) );
        }
        else if ( choice < 80 )
        {
            // release: never while a forget of the address is pending
            if ( walk.flush || walk.session[a] == 0 || rp_forget_pending( a ) )
            {
                continue;
            }
            walk.seen_before_release[a] = rp_record( a ).seen;
            rocketnet_client_slots_release( walk.slots, walk.client[a] );
            rocketnet_encryption_map_remove_current( walk.map, walk.mapping[a] );
            walk.session[a] = 0;
            walk.client[a] = -1;
            walk.mapping[a] = -1;
            walk.sent[a] = 0;
            walk.greeted[a] = false;
            records.push_back( tla_step( "release", "Release", rp_args( a ), rp_state() ) );
        }
        else if ( choice < 88 )
        {
            if ( walk.flush )
            {
                // the commit: the kernel refuses each delete one time in
                // two and each update one time in four, by batch or not
                walk.batches = random.below( 2 ) == 0;
                for ( int b = 0; b < RP_ADDRESSES; b++ )
                {
                    walk.refuse_forget[b] = random.below( 2 ) == 0;
                    walk.refuse_mark[b] = random.below( 4 ) == 0;
                    walk.kernel_refused_forget[b] = false;
                }
                bool applied_mark[RP_ADDRESSES];
                for ( int b = 0; b < RP_ADDRESSES; b++ )
                {
                    applied_mark[b] = rp_mark_pending( b ) && !walk.refuse_mark[b];
                }

                rocketnet_xdp_replay_commit( &walk.marks, walk.forget_failed, walk.mark_applied, rp_delete_batch, rp_delete_single, rp_update_batch,
                                             rp_update_single, &walk, &walk.zero_mark_refused, &walk.hold_all_greetings );

                // the publish after the commit holds a keep-alive as
                // rocketnet_server_socket_hold_greetings does: every one when
                // a zero mark was not queued, else one whose kept mark is 0
                for ( int b = 0; b < RP_ADDRESSES; b++ )
                {
                    if ( walk.session[b] == 0 )
                    {
                        walk.greeted[b] = false;
                        continue;
                    }
                    const bool held = walk.hold_all_greetings || rocketnet_xdp_replay_marks_holds_greeting( &walk.marks, &walk.address[b] );
                    walk.greeted[b] = walk.greeted[b] || !held;
                }
                for ( int b = 0; b < RP_ADDRESSES; b++ )
                {
                    walk.mac_forget[b] = false;
                }
                walk.flush = false;
                records.push_back( tla_step( "commit", "Commit", tla_sequence( std::vector<std::string>() ), rp_state() ) );

                // the plant's effect: a forget the kernel refused, with no
                // applied mark of its key, is no longer pending
                for ( int b = 0; b < RP_ADDRESSES; b++ )
                {
                    if ( walk.kernel_refused_forget[b] && !applied_mark[b] && !rp_forget_pending( b ) )
                    {
                        planted.push_back( (int) records.size() );
                        break;
                    }
                }
            }
            else
            {
                rocketnet_client_slots_publish( walk.slots );
                struct rocketnet_server_replay_sink_t sink;
                sink.context = &walk;
                sink.forget = rp_sink_forget;
                sink.mark = rp_sink_mark;
                rocketnet_server_replay_marks_walk( walk.replay, walk.slots, walk.map, &sink );
                walk.flush = true;
                records.push_back( tla_step( "walk", "Walk", tla_sequence( std::vector<std::string>() ), rp_state() ) );
            }
        }
        else if ( choice < 91 )
        {
            // a handshake from an address with no session: the direct response
            if ( walk.flush || walk.session[a] != 0 || walk.held[a] )
            {
                continue;
            }
            const uint8_t frame_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x01, (uint8_t) a };
            uint64_t refused = 0;
            const uint8_t * mac = rocketnet_xdp_reply_mac( &walk.macs, &walk.address[a], frame_mac, &refused );
            if ( !mac )
            {
                fprintf( stderr, "tla_trace: xdp-replay: a direct response found no MAC\n" );
                return 1;
            }
            walk.held[a] = true;
            walk.held_replies++;
            walk.heard[a] = true;
            records.push_back( tla_step( "answer", "Answer", rp_args( a, tla_bool( mac != frame_mac ) ), rp_state() ) );
        }
        else if ( choice < 94 )
        {
            if ( walk.flush || walk.held[a] )
            {
                continue;
            }
            const bool session_reply = walk.session[a] != 0 && random.below( 2 ) == 0;
            walk.held[a] = true;
            walk.held_replies++;
            records.push_back( session_reply ? tla_step( "hold_session", "HoldSessionReply", rp_args( a ), rp_state() )
                                             : tla_step( "hold_stray", "HoldStray", rp_args( a ), rp_state() ) );
        }
        else
        {
            // the link lets a held reply go, with no frame in hand
            if ( walk.flush || !walk.held[a] )
            {
                continue;
            }
            uint64_t refused = 0;
            const uint8_t * mac = rocketnet_xdp_reply_mac( &walk.macs, &walk.address[a], NULL, &refused );
            walk.held[a] = false;
            if ( mac )
            {
                walk.sent_to[a][walk.heard[a] ? 1 : 0] = true;
            }
            else
            {
                walk.refused_held[a][walk.heard[a] ? 1 : 0] = true;
            }
            records.push_back( tla_step( "drain", "Drain", rp_args( a, tla_bool( mac != NULL ) ), rp_state() ) );
        }
        idle = 0;
    }

    for ( size_t i = 1; i < records.size(); i++ )
    {
        // the act field is the second; its text is the quoted name
        const std::string & quoted = records[i].values[1];
        report.act( quoted.substr( 1, quoted.size() - 2 ).c_str() );
    }
    for ( size_t i = 0; i < planted.size(); i++ )
    {
        report.planted( planted[i] );
    }
    report.print();

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceXdpReplayPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceXdpReplaySeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    std::string addresses = "{";
    for ( int a = 0; a < RP_ADDRESSES; a++ )
    {
        addresses += ( a > 0 ? ", " : "" ) + rp_name( a );
    }
    addresses += "}";

    const std::string definitions = "TraceAddresses == " + addresses + "\n" + "TraceBudget == " + tla_int( steps + 1 ) + "\n";
    const std::string constants = "    Addresses <- TraceAddresses\n"
                                  "    MaxSeq = " +
                                  tla_int( RP_MAX_SEQ ) + "\n" + "    W = " + tla_int( ROCKETNET_XDP_REPLAY_WINDOW ) + "\n" + "    Sessions = " +
                                  tla_int( RP_SESSIONS ) + "\n" + "    MaxInFlight = " + tla_int( RP_MAX_IN_FLIGHT ) + "\n" + "    ForgetCapacity = " +
                                  tla_int( RP_CAPACITY ) + "\n" +
                                  "    ForgetFailureBudget <- TraceBudget\n"
                                  "    MarkFailureBudget <- TraceBudget\n"
                                  "    HeldReplyBudget <- TraceBudget\n";
    return tla_write_trace( out, name, "TraceRNXdpReplay", definitions, constants, records ) ? 0 : 1;
}

#else // #if defined( ROCKETNET_XDP_FILTER_HOST )

// the walk runs packets through the XDP filter built as host C, which needs
// GCC or Clang on a non-Windows host (CMakeLists.txt)
static int trace_xdp_replay( uint64_t seed, int steps, const char * out )
{
    (void) seed;
    (void) steps;
    (void) out;
    fprintf( stderr, "tla_trace: xdp-replay needs the XDP filter built as host C (GCC or Clang, not Windows)\n" );
    return 1;
}

#endif // #if defined( ROCKETNET_XDP_FILTER_HOST )

TLA_TRACE_LAYER( "xdp-replay", trace_xdp_replay );
