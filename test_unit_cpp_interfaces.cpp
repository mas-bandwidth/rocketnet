/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_cpp_interfaces.cpp -- the ABI layout queries of sdk/rocketnet.h,
// called from C++, checked against the compiled transport types.
//
//   * test_abi_queries_match_the_compiled_layout: every answer equals sizeof and
//     offsetof of the compiled types.
//
//   * test_abi_queries_refuse_unknown_names: unknown types and fields are refused.
//
// No socket, no clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// test_abi_queries_match_the_compiled_layout
// ---------------------------------------------------------------------------
//
// The ABI layout queries (sdk/rocketnet_abi.cpp, find_type and find_field)
// answer the compiled layout: the size of each type and, for packet_context
// and client_transport_stats, the offset and size of every field. A consumer
// binds against these answers, so they must equal sizeof/offsetof of the
// compiled types. Field counts are the lengths of the fields arrays in
// sdk/rocketnet_abi.cpp at 7ffb924.

static void test_abi_queries_match_the_compiled_layout()
{
    rocketnet_unit_check( rocketnet_abi_version() == ROCKETNET_ABI_VERSION );

    struct abi_type_check_t { const char * name; size_t size; };
    static const abi_type_check_t types_[] =
    {
        { "rocketnet_client_transport_config_t", sizeof( rocketnet_client_transport_config_t ) },
        { "rocketnet_server_transport_config_t", sizeof( rocketnet_server_transport_config_t ) },
        { "rocketnet_client_transport_stats_t", sizeof( rocketnet_client_transport_stats_t ) },
        { "rocketnet_server_transport_stats_t", sizeof( rocketnet_server_transport_stats_t ) },
        { "rocketnet_server_transport_client_stats_t", sizeof( rocketnet_server_transport_client_stats_t ) },
        { "rocketnet_packet_context_t", sizeof( rocketnet_packet_context_t ) },
        { "rocketnet_frame_sync_stats_t", sizeof( rocketnet_frame_sync_stats_t ) },
        { "rocketnet_message_config_t", sizeof( rocketnet_message_config_t ) },
    };
    for ( const auto & t : types_ )
    {
        rocketnet_unit_check( rocketnet_abi_type_size( t.name ) == t.size );
        rocketnet_unit_check( rocketnet_abi_field_count( t.name ) > 0 );
    }

    rocketnet_unit_check( rocketnet_abi_field_count( "rocketnet_packet_context_t" ) == 6 );
    rocketnet_unit_check( rocketnet_abi_field_count( "rocketnet_client_transport_stats_t" ) == 7 );
    rocketnet_unit_check( rocketnet_abi_field_count( "rocketnet_server_transport_stats_t" ) == 4 );
    rocketnet_unit_check( rocketnet_abi_field_count( "rocketnet_server_transport_client_stats_t" ) == 6 );
    rocketnet_unit_check( rocketnet_abi_field_count( "rocketnet_frame_sync_stats_t" ) == 6 );

    struct abi_field_check_t { const char * type; const char * field; size_t offset; size_t size; };
    static const abi_field_check_t fields_[] =
    {
        { "rocketnet_packet_context_t", "frame", offsetof( rocketnet_packet_context_t, frame ), sizeof( ((rocketnet_packet_context_t*)0)->frame ) },
        { "rocketnet_packet_context_t", "payload_index", offsetof( rocketnet_packet_context_t, payload_index ), sizeof( ((rocketnet_packet_context_t*)0)->payload_index ) },
        { "rocketnet_packet_context_t", "payload_capacity", offsetof( rocketnet_packet_context_t, payload_capacity ), sizeof( ((rocketnet_packet_context_t*)0)->payload_capacity ) },
        { "rocketnet_packet_context_t", "packet_id", offsetof( rocketnet_packet_context_t, packet_id ), sizeof( ((rocketnet_packet_context_t*)0)->packet_id ) },
        { "rocketnet_packet_context_t", "sequence", offsetof( rocketnet_packet_context_t, sequence ), sizeof( ((rocketnet_packet_context_t*)0)->sequence ) },
        { "rocketnet_packet_context_t", "client_index", offsetof( rocketnet_packet_context_t, client_index ), sizeof( ((rocketnet_packet_context_t*)0)->client_index ) },
        { "rocketnet_client_transport_stats_t", "rtt", offsetof( rocketnet_client_transport_stats_t, rtt ), sizeof( ((rocketnet_client_transport_stats_t*)0)->rtt ) },
        { "rocketnet_client_transport_stats_t", "jitter", offsetof( rocketnet_client_transport_stats_t, jitter ), sizeof( ((rocketnet_client_transport_stats_t*)0)->jitter ) },
        { "rocketnet_client_transport_stats_t", "packet_loss", offsetof( rocketnet_client_transport_stats_t, packet_loss ), sizeof( ((rocketnet_client_transport_stats_t*)0)->packet_loss ) },
        { "rocketnet_client_transport_stats_t", "packets_sent_per_second", offsetof( rocketnet_client_transport_stats_t, packets_sent_per_second ), sizeof( ((rocketnet_client_transport_stats_t*)0)->packets_sent_per_second ) },
        { "rocketnet_client_transport_stats_t", "packets_received_per_second", offsetof( rocketnet_client_transport_stats_t, packets_received_per_second ), sizeof( ((rocketnet_client_transport_stats_t*)0)->packets_received_per_second ) },
        { "rocketnet_client_transport_stats_t", "bandwidth_sent_kbps", offsetof( rocketnet_client_transport_stats_t, bandwidth_sent_kbps ), sizeof( ((rocketnet_client_transport_stats_t*)0)->bandwidth_sent_kbps ) },
        { "rocketnet_client_transport_stats_t", "bandwidth_received_kbps", offsetof( rocketnet_client_transport_stats_t, bandwidth_received_kbps ), sizeof( ((rocketnet_client_transport_stats_t*)0)->bandwidth_received_kbps ) },
    };
    for ( const auto & f : fields_ )
    {
        rocketnet_unit_check( rocketnet_abi_field_offset( f.type, f.field ) == f.offset );
        rocketnet_unit_check( rocketnet_abi_field_size( f.type, f.field ) == f.size );
    }
}

// ---------------------------------------------------------------------------
// test_abi_queries_refuse_unknown_names
// ---------------------------------------------------------------------------
//
// The oneBI layout queries (sdk/rocketnet_abi.cpp) refuse unknown names: an
// unknown type or field returns SIZE_MAX for sizes and offsets and -1 for a
// field count.

static void test_abi_queries_refuse_unknown_names()
{
    rocketnet_unit_check( rocketnet_abi_type_size( "rocketnet_nothing_t" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_count( "rocketnet_nothing_t" ) == -1 );
    rocketnet_unit_check( rocketnet_abi_field_offset( "rocketnet_packet_context_t", "nothing" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_size( "rocketnet_packet_context_t", "nothing" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_offset( "rocketnet_nothing_t", "frame" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_size( "rocketnet_nothing_t", "frame" ) == SIZE_MAX );
}

REGISTER_UNIT_TEST( test_abi_queries_match_the_compiled_layout );
REGISTER_UNIT_TEST( test_abi_queries_refuse_unknown_names );
