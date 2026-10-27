/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client/server layer says messages, not packets. The word "packet"
// belongs only where the code builds a packet for the transport to send or
// parses a packet received from the transport. This case is a source scan
// over the sdk client/server layer: it reads the files through
// ROCKETNET_SOURCE_DIR the way test_unit_counter_classes.cpp does, opens no
// socket, uses no clock and no network. It fails on any line that mentions
// "packet" that no allowlist entry covers. The allowlist table below names
// every legitimate transport/wire "packet" site (the class (a) sites); an
// entry that matches no line fails, so the list cannot rot. A use of the
// word where the user's message is meant must be renamed to message
// vocabulary with a review of the table. The public client and server
// declarations in sdk/rocketnet.h (every rocketnet_client_*,
// rocketnet_server_* and rocketnet_message_* declaration) hold no
// allowlist-free "packet": the line scan over sdk/rocketnet.h already
// covers them, so they are not coded twice here.

#include "test_unit_registry.h"
#include "portable_crt.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

// The example send-safety audit lives in its own translation unit; it runs
// as part of this case so the registered source-audit binary executes it
// without a second manifest of case names.
void rocketnet_example_send_safety_audit_run();

static std::string ascii_lower( const std::string & text )
{
    std::string out = text;
    for ( size_t i = 0; i < out.size(); i++ )
        out[i] = (char) tolower( (unsigned char) out[i] );
    return out;
}

static bool read_source_file( const std::string & relative_path, std::string & contents )
{
    std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative_path;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    if ( !file )
        return false;
    char buffer[4096];
    size_t n;
    contents.clear();
    while ( ( n = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
        contents.append( buffer, n );
    fclose( file );
    return true;
}

// One allowlist row: a file, a phrase (exact identifier or phrase) that
// covers one or more class (a) lines, and the reason the word is correct
// there. A row matches a line when the phrase appears verbatim in it.

struct allowlist_row_t
{
    const char * file;
    const char * phrase;
    const char * reason;
};

#define F_H "sdk/rocketnet.h"
#define F_API "sdk/rocketnet_api.cpp"
#define F_C "sdk/rocketnet_message_client.cpp"
#define F_S "sdk/rocketnet_message_server.cpp"
#define F_O "sdk/rocketnet_message_object.h"
#define F_M "sdk/rocketnet_message.cpp"

static const allowlist_row_t allowlist[] = {
    // TRANSPORT SIZE: the UDP payload of one datagram, a transport quantity.
    { F_H, "ROCKETNET_MAX_PACKET_BYTES", "TRANSPORT SIZE" },
    { F_H, "UDP payload bytes per packet", "TRANSPORT SIZE" },
    { F_H, "application payload bytes per packet", "TRANSPORT SIZE" },
    { F_H, "fit in one packet", "TRANSPORT SIZE" },
    { F_H, "Packet-filter bytes", "PACKET FILTER AND ACK TRACKING" },
    { F_H, "is the title packet filter", "PACKET FILTER AND ACK TRACKING" },
    { F_H, "test packet filter", "PACKET FILTER AND ACK TRACKING" },
    { F_H, "ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION", "PACKET FILTER AND ACK TRACKING" },
    { F_API, "ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION", "PACKET FILTER AND ACK TRACKING" },
    { F_H, "Delivered at most once; a lost packet loses the message", "FRAGMENT PACKING" },
    { F_H, "Packets from the server that opened", "TRANSPORT COUNTERS AND STATS" },
    { F_H, "Packets from clients that opened", "TRANSPORT COUNTERS AND STATS" },
    { F_H, "Outbound session packets not yet acknowledged, as a percentage of", "TRANSPORT COUNTERS AND STATS" },
    { F_H, "Simulates outgoing server packets", "NETWORK SIMULATOR" },
    { F_H, "then send packets", "SEND PACKETS" },
    { F_H, "queued messages in packets", "SEND PACKETS" },
    { F_H, "ROCKETNET_MAX_PACKET_PAYLOAD_BYTES", "TRANSPORT SIZE" },
    { F_H, "ROCKETNET_PACKET_FILTER_BYTES", "PACKET FILTER AND ACK TRACKING" },
    { F_API, "ROCKETNET_PACKET_FILTER_BYTES", "PACKET FILTER AND ACK TRACKING" },
    { F_H, "latency, jitter, packet", "NETWORK SIMULATOR" },
    { F_H, "PACKET_LOSS_PERCENT", "NETWORK SIMULATOR" },
    { F_API, "PACKET_LOSS_PERCENT", "NETWORK SIMULATOR" },
    { F_S, "ROCKETNET_MAX_PACKET_PAYLOAD_BYTES", "TRANSPORT SIZE" },
    { F_M, "ROCKETNET_MAX_PACKET_PAYLOAD_BYTES", "TRANSPORT SIZE" },
    { F_H, "packet_filter", "PACKET FILTER AND ACK TRACKING" },
    { F_API, "packet_filter", "PACKET FILTER AND ACK TRACKING" },
    { F_H, "Packets per second", "TRANSPORT SIZE" },
    { F_H, "PACKETS_IN_FLIGHT", "TRANSPORT SIZE" },
    { F_H, "unacknowledged packets tracked for their acks", "TRANSPORT SIZE" },
    { F_H, "one number to a packet never evicts", "TRANSPORT SIZE" },
    { F_H, "PACKETS_PER_UPDATE", "TRANSPORT SIZE" },

    // FRAGMENT PACKING: how messages are packed into packets.
    { F_O, "rn_message_packet_build_t", "FRAGMENT PACKING" },
    { F_O, "One open outgoing packet per connection", "FRAGMENT PACKING" },
    { F_S, "rn_message_packet_build_t", "FRAGMENT PACKING" },
    { F_S, "whole-packet send cannot overtake", "FRAGMENT PACKING" },
    { F_O, "one record in one packet", "FRAGMENT PACKING" },
    { F_M, "the record ends this packet", "FRAGMENT PACKING" },
    { "sdk/rocketnet_message.h", "message-to-packet", "FRAGMENT PACKING" },
    { F_S, "in the packet", "FRAGMENT PACKING" },

    // HANDSHAKE: the handshake's own packet.
    { F_H, "before the first packet was sent", "HANDSHAKE" },

    // TRANSPORT COUNTERS AND STATS: datagrams on the wire.
    { F_H, "packets_sent", "TRANSPORT COUNTERS AND STATS" },
    { F_H, "packets_received", "TRANSPORT COUNTERS AND STATS" },
    { F_H, "packet_loss", "TRANSPORT COUNTERS AND STATS" },
    { F_H, "of sent packets", "TRANSPORT COUNTERS AND STATS" },
    // These fields describe the held transport connection's packet and byte
    // counters; "packet" here names a UDP datagram, not an application message.
    { F_H, "Packet, byte, payload, resend, duplicate, receipt and budget counts are", "TRANSPORT COUNTERS AND STATS" },
    { F_H, "Callable in every client state. Packet, byte, payload, resend, duplicate,", "TRANSPORT COUNTERS AND STATS" },
    { F_API, "PACKETS_SENT", "TRANSPORT COUNTERS AND STATS" },
    { F_API, "PACKETS_RECEIVED", "TRANSPORT COUNTERS AND STATS" },
    { F_API, "packets_sent", "TRANSPORT COUNTERS AND STATS" },
    { F_API, "packets_received", "TRANSPORT COUNTERS AND STATS" },
    { F_API, "packet_loss", "TRANSPORT COUNTERS AND STATS" },
    // These implementation comments describe per-connection transport totals
    // and their publication lifetime; they do not call application messages packets.
    { F_API, "The counts each public stats struct carries. A client connection's packet", "TRANSPORT COUNTERS AND STATS" },
    { F_API, "The retiring connection is destroyed. Its packet, byte and message totals", "TRANSPORT COUNTERS AND STATS" },
    { F_API, "packets, bytes and messages are not published. Not-connected drops", "TRANSPORT COUNTERS AND STATS" },

    // PACKET FILTER AND ACK TRACKING: per-datagram transport state.
    { F_H, "packets_in_flight", "PACKET FILTER AND ACK TRACKING" },
    { F_API, "packets_in_flight", "PACKET FILTER AND ACK TRACKING" },

    // PACKET BUILD CAP: how many packets the message layer builds per update.
    { F_H, "packets_per_update", "PACKET BUILD CAP" },
    { F_API, "packets_per_update", "PACKET BUILD CAP" },

    // LINK CALLBACK TO THE TRANSPORT: hands a frame to the transport.
    { F_C, "packet_ids", "LINK CALLBACK TO THE TRANSPORT" },
    { F_C, "rocketnet_client_transport_acked_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_C, "start_packet", "LINK CALLBACK TO THE TRANSPORT" },
    { F_C, "finish_packet", "LINK CALLBACK TO THE TRANSPORT" },
    { F_C, "abort_packet", "LINK CALLBACK TO THE TRANSPORT" },
    { F_C, "rn_client_link_send_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_C, "rocketnet_client_transport_send_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_C, "link->send_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_S, "rocketnet_server_transport_acked_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_S, "rn_server_link_send_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_S, "rocketnet_server_transport_send_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_S, "link->send_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_S, "object->link.send_packets", "LINK CALLBACK TO THE TRANSPORT" },
    { F_O, "packet_ids", "LINK CALLBACK TO THE TRANSPORT" },
    { F_O, "packet ids", "LINK CALLBACK TO THE TRANSPORT" },
    { F_O, "start_packet", "LINK CALLBACK TO THE TRANSPORT" },
    { F_O, "finish_packet", "LINK CALLBACK TO THE TRANSPORT" },
    { F_O, "abort_packet", "LINK CALLBACK TO THE TRANSPORT" },
    { F_O, "(*send_packets)", "LINK CALLBACK TO THE TRANSPORT" },

    // PACKET BUILDER AND ENDPOINT PACKET PLANNING: assembles the packet the
    // transport sends and reads acks of packets the transport sent.
    { F_S, "each packet write copies", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "start_packet returned", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "rn_packet_builder_write", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "rocketnet_packet_builder.h", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "acked packet ids", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "packet_ids", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "OpenPacket", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "WritePacket", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "next_packet_bytes", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "packet id (RNSlots", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "packet_context_t", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "packet_failed", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "write_packet", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "packet_sent", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "abort_packet", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "finish_packet", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "start_packet", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_O, "rocketnet_packet_context_t", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "context.packet_id", "PACKET BUILDER AND ENDPOINT PACKET PLANNING" },
    { F_S, "packet allowance", "PACKET BUILD CAP" },
    { F_S, "one packet of it", "PACKET BUILD CAP" },
    { F_H, "send_packets", "SEND PACKETS" },
    { F_H, "rocketnet_client_send_packets", "SEND PACKETS" },
    { F_H, "rocketnet_server_send_packets", "SEND PACKETS" },
    { F_H, "disconnect packets", "SEND PACKETS" },
    { F_API, "rocketnet_client_send_packets", "SEND PACKETS" },
    { F_API, "rocketnet_message_client_send_packets", "SEND PACKETS" },
    { F_API, "rocketnet_server_send_packets", "SEND PACKETS" },
    { F_API, "rocketnet_message_server_send_packets", "SEND PACKETS" },
    { F_API, "rocketnet_server_transport_send_packets_shutdown", "SEND PACKETS" },
    { F_API, "packets, then the per-slot connection reset", "SEND PACKETS" },
    { F_C, "rocketnet_message_client_send_packets", "SEND PACKETS" },
    { F_C, "rn_message_object_send_packets", "SEND PACKETS" },
    { F_S, "rocketnet_message_server_send_packets", "SEND PACKETS" },
    { F_S, "rn_message_object_send_packets", "SEND PACKETS" },
    { F_S, "send_packets", "SEND PACKETS" },
    { F_O, "rn_message_object_send_packets", "SEND PACKETS" },
};

static const size_t allowlist_count = sizeof( allowlist ) / sizeof( allowlist[0] );

static bool scan_file( const char * relative_path, std::vector<std::string> & failures,
                       std::vector<size_t> & used_rows, std::vector<int> & per_reason_hits )
{
    std::string text;
    if ( !read_source_file( relative_path, text ) )
    {
        failures.push_back( std::string( "cannot read source file " ) + relative_path );
        return false;
    }

    size_t line_start = 0;
    size_t line_no = 1;
    for ( size_t i = 0; i <= text.size(); i++ )
    {
        if ( i == text.size() || text[i] == '\n' )
        {
            std::string line = text.substr( line_start, i - line_start );
            if ( !line.empty() && line[line.size() - 1] == '\r' )
                line.erase( line.size() - 1 );

            std::string lower = ascii_lower( line );
            if ( lower.find( "packet" ) != std::string::npos )
            {
                bool covered = false;
                for ( size_t r = 0; r < allowlist_count; r++ )
                {
                    if ( strcmp( allowlist[r].file, relative_path ) != 0 )
                        continue;
                    if ( line.find( allowlist[r].phrase ) != std::string::npos )
                    {
                        covered = true;
                        used_rows[r] = 1;
                        per_reason_hits[r]++;
                    }
                }
                if ( !covered )
                {
                    char buf[32];
                    snprintf( buf, sizeof( buf ), "%zu", line_no );
                    failures.push_back( std::string( relative_path ) + ":" + buf + ": " + line );
                }
            }

            line_start = i + 1;
            line_no++;
        }
    }
    return true;
}

static void test_client_server_say_messages_not_packets()
{
    rocketnet_example_send_safety_audit_run();
    std::vector<std::string> failures;
    std::vector<size_t> used_rows( allowlist_count, 0 );
    std::vector<int> per_reason_hits( allowlist_count, 0 );

    const char * files[] = { F_H, F_API, F_C, F_S, F_O, F_M, "sdk/rocketnet_message.h" };
    for ( size_t f = 0; f < sizeof( files ) / sizeof( files[0] ); f++ )
        scan_file( files[f], failures, used_rows, per_reason_hits );

    for ( size_t r = 0; r < allowlist_count; r++ )
    {
        if ( !used_rows[r] )
        {
            char buf[64];
            snprintf( buf, sizeof( buf ), "%llu", (unsigned long long) ( r + 1 ) );
            failures.push_back( std::string( "allowlist entry " ) + buf + " ( " + allowlist[r].file +
                                " : " + allowlist[r].phrase + " ) matched no line" );
        }
    }

    if ( !failures.empty() )
    {
        for ( size_t i = 0; i < failures.size(); i++ )
            printf( "%s\n", failures[i].c_str() );
        printf( "test_client_server_say_messages_not_packets: %zu unallowlisted or unused line(s)\n", failures.size() );
        fflush( stdout );
        exit( 1 );
    }

    // a passing run prints its name only; ROCKETNET_TEST_VERBOSE adds the
    // per-reason report
    char verbose[4096];
    if ( !rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", verbose, sizeof( verbose ) ) )
        return;

    std::vector<std::string> reasons;
    std::vector<int> counts;
    for ( size_t r = 0; r < allowlist_count; r++ )
    {
        size_t existing = reasons.size();
        for ( size_t i = 0; i < existing; i++ )
        {
            if ( reasons[i] == allowlist[r].reason )
            {
                counts[i] += per_reason_hits[r];
                break;
            }
        }
        if ( existing == reasons.size() )
        {
            reasons.push_back( allowlist[r].reason );
            counts.push_back( per_reason_hits[r] );
        }
    }
    printf( "test_client_server_say_messages_not_packets: pass; allowlisted packet lines per reason:\n" );
    for ( size_t i = 0; i < reasons.size(); i++ )
        printf( "  %s: %d\n", reasons[i].c_str(), counts[i] );
}

REGISTER_UNIT_TEST( test_client_server_say_messages_not_packets );
