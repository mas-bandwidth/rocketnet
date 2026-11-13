/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public light-client profile, pinned. A 1000-slot server and a client
// report lifecycle peaks within the figures USAGE.md records, and both
// endpoints create and destroy on the profile without opening a socket.

#include "test_unit_registry.h"
#include "msan_scale_allocator.h"
#include "rocketnet.h"
#include "rocketnet_connect_token.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// the test's own protocol id, backend key and packet filter (never the
// published development values, which a production library refuses), set
// over the profile before a size or a create, so the cases run against the
// development and the production library alike

static const uint8_t light_packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x4C, 0x49, 0x47, 0x48 };

static void light_server_keys( struct rocketnet_server_config_t * config )
{
    config->protocol_id = 0x4C49474854ULL;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x4C ^ i );
    }
    uint8_t backend_public_key[32];
    uint8_t backend_secret_key[64];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    memcpy( config->backend_public_key, backend_public_key, 32 );
    memcpy( config->packet_filter, light_packet_filter, sizeof( config->packet_filter ) );
}

static void light_client_filter( struct rocketnet_client_config_t * config )
{
    memcpy( config->packet_filter, light_packet_filter, sizeof( config->packet_filter ) );
}

// USAGE.md "Server memory footprint" and docs/LOAD.md record these figures.
// required_memory is the lifecycle peak the caller's allocator sees (create,
// connect or start, stop or disconnect, again, destroy) for the largest
// transport the default ladder can grant, so it differs by platform and by
// ROCKETNET_DEV (a development endpoint also owns its link simulator). Each
// bound is the figure this test read, Release, for
// rocketnet_server_required_memory at max_clients 1000 and
// rocketnet_client_required_memory under the profile: on Linux x86-64 with
// io_uring and XDP built, on macOS arm64, and on Windows x64 with MSVC.
// The ladder is the build's (sdk/rocketnet_driver.cpp: the transports
// compiled for the platform), not the host's, so a development build in one
// of those three configurations reads exactly the pin and the check is
// equality: a figure that falls leaves the pin stale-high, and growth back up
// to it would pass unseen, so a fall fails too until the pin is moved to the
// new figure. A build outside them (another architecture, XDP or io_uring not
// built) grants fewer transports and reports less; a production build
// (ROCKETNET_DEV 0) has no link simulator and reports less too; there the
// check is at most, and the case says so (LIGHT_PINNED_CONFIGURATION).
// The case prints its actual= line only with ROCKETNET_TEST_VERBOSE=1 or when
// it fails; the suite captures a passing case's output.
// The server figures include the disconnect ring of 2 * ROCKETNET_MAX_CLIENTS
// bursts (proofs/tla/ServerStopSpread.tla RingSize, 1088 bytes an entry) and,
// on Linux, the xdp plan's per-slot replay records
// (rocketnet_server_replay_marks_allocation_bytes).
// The figures are the planned peak; required_memory adds a fixed 1 MiB of
// slack on top for the caller's heap (sdk/rocketnet_api.cpp
// rn_required_memory_slack_bytes), so each check adds it back.
// On every platform the encryption map index has 2 * capacity uint32_t cells
// (sdk/rocketnet_encryption_map.cpp, index_cells), and the development
// endpoint's link simulator carries the receive_any delivery bound and hint
// and two receive_any scan counters (sdk/rocketnet_network_simulator.h,
// 40 bytes).
#define LIGHT_REQUIRED_MEMORY_SLACK_BYTES ( (size_t) 1024 * 1024 )
#if defined( _WIN32 )
// Windows x64, MSVC, Release.
// The server protocol's cookie reply limit (a 262144-bit bitmap carved with
// the transport, its pointer, word count and key) adds 32832 bytes on macOS
// and 32768 on Linux, measured; this figure adds the larger, computed and
// not measured on Windows.
// Each connection's rn_window_ack_t holds the two resend sent marks
// (resend_sent, chunk_sent), adding 8 bytes a connection, 8000 at 1000
// clients, computed and not measured on Windows.
// Each channel's send state widens its per-priority queued counts to
// uint64_t (rn_channel_send_t, 40 to 64 bytes), adding 24 bytes a channel,
// 192 a light connection (8 channels), 192000 at 1000 clients, computed and
// not measured on Windows.
// The development resend gap record counts its resends in uint64_t
// (rn_resend_gaps_t count), adding 8 bytes a connection, 8000 at 1000
// clients, and at most 8 to the client (macOS and Linux measured 0),
// computed and not measured on Windows.
// Each connection's rn_window_ack_t holds the probe and RTT filter clocks
// (last_progress, last_ack, silence_end, has_last_ack: 32 bytes) and its
// rn_endpoint_t counts send_refused (8 bytes), adding 40 bytes a connection,
// 40000 at 1000 clients, and at most 64 to the client (one 64-byte
// rounding): computed, not measured on Windows, by adding the 40000 and the
// 64 that macOS and Linux both measured to the figures pinned before.
// The server's send pass keeps a writer list, two int32_t a connection
// (sdk/rocketnet_message_server.cpp, send_writers), adding 8000 bytes at 1000
// clients and nothing to the client (one connection keeps its two entries on
// the stack).
// The pins are the figures read with ROCKETNET_TEST_VERBOSE=1 on captain
// (Visual Studio 18 2026, MSVC 19.51, x64, DEV=ON, Release and Debug): the
// actual= lines 1469500735 and 15258748, less the slack. The pins before them,
// 1608857790 and 22972215, stood 140405631 and 8762043 bytes above what the
// build reads; the at-most check passed them and the suite hid the actual=
// lines, so the figures above, computed from them, were never seen to hold.
// v1.2/dead-code removed a client field (session_nonce): the client object is
// one 64-byte rounding smaller, and the client's actual= line is 15258684
// (read on aws-win-1, MSVC 19.52.37029, x64, DEV=ON, Release: 15258748 at the
// tree before that merge, 15258684 after it, same toolset). The server's
// reading did not move.
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1468452159 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 14210108 )
#elif defined( __APPLE__ )
// macOS arm64 (studio), clang, Release.
// The figures are the planned peak: the probe's actual= lines (server
// 1469212319, client 15036170, Release and Debug) less the 1 MiB caller
// slack, which the check adds back. The server figure pinned before this one,
// 1469129231, still held most of that slack: 965488 bytes over the peak.
// The server protocol's cookie reply limit (its bitmap pointer, word count
// and 16-byte key, and the 262144-bit source bitmap carved with the
// transport) adds 32832 bytes; the probe's actual= line is 1469245151.
// Each connection's rn_window_ack_t holds the two resend sent marks
// (resend_sent, chunk_sent: the first number on the wire of each resend
// sequence, where the ordered walk starts its retransmits), adding 8 bytes a
// connection, 8000 at 1000 clients (measured); the probe's actual= line is
// 1469253151.
// Each channel's send state widens its per-priority queued counts to
// uint64_t (sdk/rocketnet_allocator.h, rn_channel_send_t, 40 to 64 bytes),
// adding 24 bytes a channel, 192 a light connection (8 channels), 192000 at
// 1000 clients (measured); the probe's actual= lines are 1469445151 and
// 15036362.
// The development resend gap record counts its resends in uint64_t
// (sdk/rocketnet_window_ack.h, rn_resend_gaps_t count), adding 8 bytes a
// connection, 8000 at 1000 clients; the client's figure is unchanged (its
// object's 64-byte rounding holds the 8 bytes) (measured); the probe's
// actual= line is 1469453151.
// Each connection's rn_window_ack_t holds the probe and RTT filter clocks
// (last_progress, last_ack, silence_end, has_last_ack: 32 bytes) and its
// rn_endpoint_t counts send_refused (8 bytes), adding 40 bytes a connection,
// 40000 at 1000 clients, and 64 to the client (its object's 64-byte
// rounding) (measured); the probe's actual= lines are 1469493151 and
// 15036426.
// The server's send pass keeps a writer list, two int32_t a connection
// (sdk/rocketnet_message_server.cpp, send_writers), adding 8000 bytes at 1000
// clients and nothing to the client (one connection keeps its two entries on
// the stack) (measured); the probe's actual= line is 1469501151.
// The client I/O core counts the datagrams its worker discards at its exit
// (sdk/rocketnet_client_io.h, receive_discarded_at_stop, 8 bytes), adding 64
// to the client (its object's 64-byte rounding) and nothing to the server
// (measured); the probe's actual= line is 15036490.
// The appended counter CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD grows every
// counter block by 8 bytes, adding 64 to the server and 64 to the client
// (their objects' 64-byte rounding) (measured); the probe's actual= lines
// are 1469501215 and 15036554.
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1468452639 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 13987978 )
#else
// Linux x86-64 with io_uring and XDP built (vision), gcc, Release (a Debug build reports the same figure).
// The RTT reader added to the message link crosses the message object's
// 64-byte head-block alignment, as measured on main, adding 64 bytes.
// The xdp worker's two receive frame arrays moved from its thread stack into
// each queue's rocketnet_server_xdp_socket_t, adding 262152 bytes a queue,
// 1835064 for the 7 default queues; the pin then moved by only 872136
// because the pin before it stood 963320 bytes above the planned peak.
// Each xdp queue counts the TX wakeups that failed hard
// (send_wakeup_failures).
// The replay walk's object holds supersede_owed and, in a development build,
// the count of records its scans read, adding 8 bytes
// (sdk/rocketnet_server_replay_marks.cpp, struct rocketnet_server_replay_marks_t).
// The figure is the planned peak, as on the other platforms: the probe's
// actual= line (3111387923, Release and Debug) less the 1 MiB caller slack,
// which the check adds back. The figure pinned before this one, 3111432971,
// was the probe's line with the slack still in it: 1048576 over the peak it
// stood for, which was 45056 bytes of replay tables that are gone again
// (a 2 * 1024 * 20-byte kernel key table and a 2 * 1024 * 2-byte
// pending-forget index).
// The used-token history's two token tables (bindings and claims) each carry
// expire_floor, the bindings' lower bound on a live expiry, adding 16 bytes
// (sdk/rocketnet_token_tables.cpp, struct rocketnet_token_table_t); the probe's
// actual= line is 3111387939.
// The server protocol's cookie reply limit (its bitmap pointer, word count
// and key, and the 262144-bit source bitmap carved with the transport) adds
// 32768 bytes; the probe's actual= line is 3111420707.
// Each xdp queue publishes the frames the kernel dropped on its socket
// (kernel_dropped, sdk/rocketnet_server_socket_linux.cpp), adding 8 bytes;
// computed with the cookie reply limit, the probe's actual= line is
// 3111420715.
// Each connection's rn_window_ack_t holds the two resend sent marks
// (resend_sent, chunk_sent), adding 8 bytes a connection, 8000 at 1000
// clients; the probe's actual= line is 3111428715.
// Each channel's send state widens its per-priority queued counts to
// uint64_t (sdk/rocketnet_allocator.h, rn_channel_send_t, 40 to 64 bytes),
// adding 24 bytes a channel, 192 a light connection (8 channels), 192000 at
// 1000 clients (measured); the probe's actual= lines are 3111620715 and
// 15195762.
// The development resend gap record counts its resends in uint64_t
// (sdk/rocketnet_window_ack.h, rn_resend_gaps_t count), adding 8 bytes a
// connection, 8000 at 1000 clients; the client's figure is unchanged (its
// object's 64-byte rounding holds the 8 bytes) (measured); the probe's
// actual= line is 3111628715.
// Each connection's rn_window_ack_t holds the probe and RTT filter clocks
// (last_progress, last_ack, silence_end, has_last_ack: 32 bytes) and its
// rn_endpoint_t counts send_refused (8 bytes), adding 40 bytes a connection,
// 40000 at 1000 clients, and 64 to the client (its object's 64-byte
// rounding) (measured on a second Linux bench, gcc, Release: the probe's
// actual= lines are 3111668715 and 15186082, the client 64 above that
// bench's own v1.0.1 reading of 15186018).
// The client pin is that reading less the slack, 14137506 (hetzner2, gcc,
// Release and Debug, ROCKETNET_TEST_VERBOSE=1: actual=15186082). The pin
// before it, 14147250, stood 9744 bytes above every build that read it; an
// upper-bound check never said so.
// The server's send pass keeps a writer list, two int32_t a connection
// (sdk/rocketnet_message_server.cpp, send_writers), adding 8000 bytes at 1000
// clients and nothing to the client (one connection keeps its two entries on
// the stack) (measured, gcc Release: the check read exactly this figure plus
// the slack, 3111676715).
// The xdp replay marks count the pending forgets a forget's repeat search
// reads (sdk/rocketnet_xdp_replay_marks.h, comparisons, a uint64_t; the
// kept forget count beside it fills existing padding), adding 8 bytes
// (measured with the writer list: the probe's actual= line is 3111676723).
// v1.1.9 (read-round-1 on the v1.1.8 release): the development server
// transport counts its link-simulator lock misses (link_lock_misses, a
// uint64_t) and the counter block gains CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD,
// together 128 bytes on the server through its 64-byte rounding. The client
// reads 912 bytes lower on the merged tree (stats_counted is gone among
// read-round-1's client changes; the 912 is not split per change). Measured on the merged tree (bench-nyc-6, gcc, Debug and
// Release, DEV ON, ROCKETNET_TEST_VERBOSE=1): actual=3111676851 and 15185170.
#if defined( __aarch64__ )
// Linux arm64 adds 56 bytes to the server: glibc's pthread_mutex_t is 48
// bytes on aarch64 and 40 on x86-64, so the thread record each worker
// allocates (sdk/rocketnet_platform.cpp thread_record_t) is 144 bytes, not
// 136, and the xdp ladder candidate starts one thread for each of its 7
// default queues (7 * 8 = 56). Computed from the x86-64 figure, not read on
// a Graviton; not the pinned configuration, so the check is at most.
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 3110628331ULL )
#else // #if defined( __aarch64__ )
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 3110628275ULL )
#endif // #if defined( __aarch64__ )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 14136594 )
#endif // #if defined( _WIN32 )

// 1 when this build is the configuration the pins were read in, so the
// figure must equal the pin; 0 elsewhere, where it may only be at most.
#if !ROCKETNET_DEV
#define LIGHT_PINNED_CONFIGURATION 0
#elif defined( _WIN32 ) && defined( _M_X64 ) && defined( _MSC_VER ) && !defined( __clang__ )
#define LIGHT_PINNED_CONFIGURATION 1
#elif defined( __APPLE__ ) && defined( __aarch64__ )
#define LIGHT_PINNED_CONFIGURATION 1
#elif defined( __linux__ ) && defined( __x86_64__ ) && defined( ROCKETNET_XDP ) && defined( ROCKETNET_IO_URING )
#if ROCKETNET_XDP && ROCKETNET_IO_URING
#define LIGHT_PINNED_CONFIGURATION 1
#else
#define LIGHT_PINNED_CONFIGURATION 0
#endif
#else
#define LIGHT_PINNED_CONFIGURATION 0
#endif

// Equality in the pinned configuration, at most elsewhere (the header comment).
static void light_check_pin( const char * name, size_t bytes, size_t pin )
{
#if LIGHT_PINNED_CONFIGURATION
    if ( bytes != pin )
    {
        printf( "%s lifecycle bytes: actual=%zu expected=%zu\n", name, bytes, pin );
    }
    check( bytes == pin );
#else
    if ( bytes != pin )
    {
        printf( "%s lifecycle bytes: actual=%zu expected=%zu (not the pinned configuration: at most)\n", name, bytes, pin );
    }
    check( bytes <= pin );
#endif // #if LIGHT_PINNED_CONFIGURATION
}

static void light_message_fields_server( const struct rocketnet_server_config_t * config )
{
    check( config->per_client_send_kbps == 256 );
    check( config->num_channels == ( ROCKETNET_MAX_CHANNELS < 8 ? ROCKETNET_MAX_CHANNELS : 8 ) );
    check( config->reliable_messages_in_flight == 256 );
    check( config->receive_message_buffer_bytes == 142400 );
    check( config->max_message_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 );
    check( config->send_message_buffer_bytes == 141500 );
    check( config->send_queue_messages == 128 );
    check( config->packets_in_flight == 256 );
    check( config->packets_per_update == 4 );
    check( config->unreliable_messages_per_update == 4 );
    check( config->sequenced_messages_held == 100 );
    check( config->max_message_receipts == 128 );
    check( config->reliable_message_timeout_ms == 0 );             // the default: no reliable timeout (m9.3)
}

static void light_message_fields_client( const struct rocketnet_client_config_t * config )
{
    check( config->send_kbps == 256 );
    check( config->num_channels == ( ROCKETNET_MAX_CHANNELS < 8 ? ROCKETNET_MAX_CHANNELS : 8 ) );
    check( config->reliable_messages_in_flight == 256 );
    check( config->receive_message_buffer_bytes == 142400 );
    check( config->max_message_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 );
    check( config->send_message_buffer_bytes == 141500 );
    check( config->send_queue_messages == 128 );
    check( config->packets_in_flight == 256 );
    check( config->packets_per_update == 4 );
    check( config->unreliable_messages_per_update == 4 );
    check( config->sequenced_messages_held == 100 );
    check( config->max_message_receipts == 128 );
    check( config->reliable_message_timeout_ms == 0 );             // the default: no reliable timeout (m9.3)
}

static void test_light_profile_server_1000_required_memory()
{
    struct rocketnet_server_config_t config;
    check( rocketnet_server_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );
    check( config.max_clients == ( ROCKETNET_MAX_CLIENTS < 1000 ? ROCKETNET_MAX_CLIENTS : 1000 ) );
    light_message_fields_server( &config );

    if ( ROCKETNET_MAX_CLIENTS < 1000 )
    {
        return;
    }

    config.max_clients = 1000;
    light_server_keys( &config );

    const size_t bytes = rocketnet_server_required_memory( &config );
    check( bytes > 0 );
    light_check_pin( "light server", bytes, LIGHT_SERVER_1000_MAX_BYTES + LIGHT_REQUIRED_MEMORY_SLACK_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_server_1000_required_memory );

static void test_light_profile_client_required_memory()
{
    struct rocketnet_client_config_t config;
    check( rocketnet_client_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );
    light_message_fields_client( &config );
    light_client_filter( &config );

    const size_t bytes = rocketnet_client_required_memory( &config );
    check( bytes > 0 );
    light_check_pin( "light client", bytes, LIGHT_CLIENT_MAX_BYTES + LIGHT_REQUIRED_MEMORY_SLACK_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_client_required_memory );

static void test_light_profile_configs_create()
{
    // create does not bind, and it takes no clock. update at 1000.0 on a
    // stopped server and a disconnected client opens no socket.

    debug_crt_scale_allocator_scope_t scale_allocator; // tests/msan_scale_allocator.h: a 1000-slot plan
    const double now = 1000.0;

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_light_config( &server_config ) == ROCKETNET_STATUS_OK );
    light_server_keys( &server_config );

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_stopped( server ) );
    rocketnet_server_update( server, now );
    check( rocketnet_server_stopped( server ) );
    rocketnet_server_destroy( server );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_light_config( &client_config ) == ROCKETNET_STATUS_OK );
    light_client_filter( &client_config );

    status = ROCKETNET_TEST_STATUS_UNSET;

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_update( client, now );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_destroy( client );
}

REGISTER_UNIT_TEST( test_light_profile_configs_create );
