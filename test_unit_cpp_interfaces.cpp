/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_cpp_interfaces.cpp -- the C++ transport interfaces of
// sdk/rocketnet.hpp.
//
//   * cpp_transport_interfaces_map_one_to_one: compile-time checks that every
//     pure virtual method of ClientTransport and ServerTransport has exactly
//     the signature of its rocketnet_client_transport_* / rocketnet_server_transport_*
//     C function with the transport pointer dropped. The C function pointer
//     type is stripped of its first (transport) parameter and compared to the
//     member function pointer type normalized to a free function pointer. A
//     drift on either side fails the build.
//
//   * cpp_transport_fake_implements_the_interface: drives a fake subclass
//     through the abstract interface with no socket, proving the interface is
//     usable by a plain concrete class.
//
// No socket, no clock.

#include "test_unit_registry.h"

#include "rocketnet.hpp"

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// ---------------------------------------------------------------------------
// compile-time machinery for the one-to-one mapping checks
// ---------------------------------------------------------------------------

// a member function pointer type -> the free function pointer with the same
// parameter types (the class is dropped).

template <typename MFP>
struct rocketnet_mfp_to_fp;

template <typename C, typename R, typename... Args>
struct rocketnet_mfp_to_fp<R (C::*)(Args...)>
{
    typedef R (*type)(Args...);
};

// a C function pointer type -> the same signature with the first parameter
// (the transport pointer) dropped.

template <typename FP>
struct rocketnet_strip_transport;

template <typename R, typename T1, typename... Args>
struct rocketnet_strip_transport<R (*)(T1, Args...)>
{
    typedef R (*type)(Args...);
};

// Assert that the member function's normalized free signature equals the C
// function's signature without the transport pointer.

#define ROCKETNET_CHECK_MAPPED( cls, member, cfunc )                                   \
    static_assert( std::is_same< rocketnet_mfp_to_fp< decltype( &cls::member ) >::type, \
                                  rocketnet_strip_transport< decltype( &cfunc ) >::type >::value, \
                   #cls "::" #member " must map " #cfunc )

// ---------------------------------------------------------------------------
// ClientTransport maps one-to-one onto rocketnet_client_transport_*
// ---------------------------------------------------------------------------

ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, update, rocketnet_client_transport_update );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, send_packet, rocketnet_client_transport_send_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, receive_packet, rocketnet_client_transport_receive_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, max_payload_bytes, rocketnet_client_transport_max_payload_bytes );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, num_payloads, rocketnet_client_transport_num_payloads );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, payload, rocketnet_client_transport_payload );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, payloads, rocketnet_client_transport_payloads );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, start_packet, rocketnet_client_transport_start_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, packet_payload, rocketnet_client_transport_packet_payload );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, finish_packet, rocketnet_client_transport_finish_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, abort_packet, rocketnet_client_transport_abort_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, send_packets, rocketnet_client_transport_send_packets );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, state, rocketnet_client_transport_state );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, driver, rocketnet_client_transport_driver );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, client_index, rocketnet_client_transport_client_index );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, disconnect, rocketnet_client_transport_disconnect );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, counters, rocketnet_client_transport_counters );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, stats, rocketnet_client_transport_stats );
ROCKETNET_CHECK_MAPPED( rocketnet::ClientTransport, acked_packets, rocketnet_client_transport_acked_packets );

// ---------------------------------------------------------------------------
// ServerTransport maps one-to-one onto rocketnet_server_transport_*
// ---------------------------------------------------------------------------

ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, stop, rocketnet_server_transport_stop );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, state, rocketnet_server_transport_state );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, port, rocketnet_server_transport_port );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, driver, rocketnet_server_transport_driver );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, update, rocketnet_server_transport_update );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, set_admission_callback, rocketnet_server_transport_set_admission_callback );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, num_payloads, rocketnet_server_transport_num_payloads );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, payload, rocketnet_server_transport_payload );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, payloads, rocketnet_server_transport_payloads );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, start_packet, rocketnet_server_transport_start_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, packet_payload, rocketnet_server_transport_packet_payload );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, finish_packet, rocketnet_server_transport_finish_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, abort_packet, rocketnet_server_transport_abort_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, send_packets, rocketnet_server_transport_send_packets );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, send_packet, rocketnet_server_transport_send_packet );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, disconnect_client, rocketnet_server_transport_disconnect_client );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, num_connected_clients, rocketnet_server_transport_num_connected_clients );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, is_client_connected, rocketnet_server_transport_is_client_connected );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, client_id, rocketnet_server_transport_client_id );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, counters, rocketnet_server_transport_counters );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, max_payload_bytes, rocketnet_server_transport_max_payload_bytes );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, stats, rocketnet_server_transport_stats );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, client_stats, rocketnet_server_transport_client_stats );
ROCKETNET_CHECK_MAPPED( rocketnet::ServerTransport, acked_packets, rocketnet_server_transport_acked_packets );

// ---------------------------------------------------------------------------
// cpp_transport_interfaces_map_one_to_one
// ---------------------------------------------------------------------------

static void cpp_transport_interfaces_map_one_to_one()
{
    // every mapping is asserted at compile time above; nothing to run at
    // runtime, the test exists to name the contract and stay green
    rocketnet_unit_check( true );
}

// ---------------------------------------------------------------------------
// a fake subclass driven through the abstract interface, with no socket
// ---------------------------------------------------------------------------

class FakeClientTransport : public rocketnet::ClientTransport
{
public:
    FakeClientTransport() : last_time_( 0.0 ) {}

    void update( double time ) { last_time_ = time; }
    void send_packet( const uint8_t *, int ) {}
    int receive_packet( uint8_t *, int ) { return 0; }
    int max_payload_bytes() { return 0; }
    int num_payloads() { return 0; }
    uint8_t * payload( int, int * ) { return NULL; }
    int payloads( uint8_t * const **, const int ** ) { return 0; }
    uint8_t * start_packet( int, struct rocketnet_packet_context_t * ) { return NULL; }
    uint8_t * packet_payload( const struct rocketnet_packet_context_t *, int * ) { return NULL; }
    int finish_packet( struct rocketnet_packet_context_t *, int ) { return 0; }
    void abort_packet( struct rocketnet_packet_context_t * ) {}
    void send_packets() {}
    int state() { return 0; }
    const char * driver() { return "fake"; }
    int client_index() { return 0; }
    void disconnect() {}
    const struct rocketnet_counters_t * counters() { return NULL; }
    void stats( struct rocketnet_client_transport_stats_t * ) {}
    int acked_packets( const uint64_t ** ) { return 0; }

    double last_time_;
};

static void cpp_transport_fake_implements_the_interface()
{
    FakeClientTransport client;
    rocketnet::ClientTransport & iface = client;
    iface.update( 1000.0 );
    rocketnet_unit_check( client.last_time_ == 1000.0 );
    rocketnet_unit_check( iface.state() == 0 );
    rocketnet_unit_check( iface.driver() != NULL );
    rocketnet_unit_check( iface.max_payload_bytes() == 0 );
    rocketnet_unit_check( iface.num_payloads() == 0 );
}

// ---------------------------------------------------------------------------
// abi_queries_match_the_compiled_layout
// ---------------------------------------------------------------------------
//
// The ABI layout queries (sdk/rocketnet_abi.cpp, find_type and find_field)
// answer the compiled layout: the size of each type and, for packet_context
// and client_transport_stats, the offset and size of every field. A consumer
// binds against these answers, so they must equal sizeof/offsetof of the
// compiled types. Field counts are the lengths of the fields arrays in
// sdk/rocketnet_abi.cpp at 7ffb924.

static void abi_queries_match_the_compiled_layout()
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
// abi_queries_refuse_unknown_names
// ---------------------------------------------------------------------------
//
// The oneBI layout queries (sdk/rocketnet_abi.cpp) refuse unknown names: an
// unknown type or field returns SIZE_MAX for sizes and offsets and -1 for a
// field count.

static void abi_queries_refuse_unknown_names()
{
    rocketnet_unit_check( rocketnet_abi_type_size( "rocketnet_nothing_t" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_count( "rocketnet_nothing_t" ) == -1 );
    rocketnet_unit_check( rocketnet_abi_field_offset( "rocketnet_packet_context_t", "nothing" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_size( "rocketnet_packet_context_t", "nothing" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_offset( "rocketnet_nothing_t", "frame" ) == SIZE_MAX );
    rocketnet_unit_check( rocketnet_abi_field_size( "rocketnet_nothing_t", "frame" ) == SIZE_MAX );
}

REGISTER_UNIT_TEST( cpp_transport_interfaces_map_one_to_one );
REGISTER_UNIT_TEST( cpp_transport_fake_implements_the_interface );
REGISTER_UNIT_TEST( abi_queries_match_the_compiled_layout );
REGISTER_UNIT_TEST( abi_queries_refuse_unknown_names );
