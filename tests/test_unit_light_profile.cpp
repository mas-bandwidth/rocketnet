/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public light-client profile, pinned. A 1000-slot server and a client
// report lifecycle peaks within the figures USAGE.md records, and both
// endpoints create and destroy on the profile without opening a socket.

#include "test_unit_registry.h"
#include "rocketnet.h"

#include <stddef.h>
#include <stdio.h>

#define check rocketnet_unit_check

// USAGE.md "Server memory footprint" and docs/LOAD.md record these figures.
// required_memory is the lifecycle peak the caller's allocator sees (create,
// connect or start, stop or disconnect, again, destroy) for the largest
// transport the default ladder can grant, so it differs by platform and by
// ROCKETNET_DEV (a development endpoint also owns its link simulator). Each
// bound is the figure this test read, Release, for
// rocketnet_server_required_memory at max_clients 1000 and
// rocketnet_client_required_memory under the profile: on Linux x86-64 with
// io_uring and XDP built, on macOS arm64, and on Windows x64 with MSVC. A
// host whose ladder grants fewer transports reports less; the check is at
// most.
// 2026-10-29: the per-slot transport end reason adds 4 bytes a slot;
// measured +4087 bytes on the macOS DEV server at 1000 slots, so every server
// figure below is raised by 4096.
// 2026-10-31: the server transport's disconnect ring holds
// 2 * ROCKETNET_MAX_CLIENTS bursts, one for every client the stop disconnects
// beside the bursts a running server keeps waiting (proofs/tla/ServerStopSpread.tla
// RingSize); measured +1088048 bytes on the macOS DEV server, 1088 bytes a
// ring entry, so every server figure below is raised by 1088048.
#if defined( _WIN32 )
// Windows adds, on the server, over the figures before them: the TX banks
// sized at 16 packets per client (3 banks x 15,000 more packets x 1500 bytes
// at 1000 clients: 67,500,000) and 231 bytes of the send handoff's per-path
// drop counts and allocation rounding. Its sockets keep no per-errno send
// counts, so the client figures are unchanged.
// Re-measured 2026-10-27 on a Windows x64 host, Windows x64, MSVC 19.51.36260, Release.
// DEV ON server was 1591206044, now 1603276694. DEV ON client was 22968879,
// now 22967743. DEV OFF server was 1584809680, now 1596895498. DEV OFF client
// was 22183075, now 22182883. A production build refuses the published test
// filter and required_memory then returns 0, so the DEV OFF peaks were read
// with a non-test filter on the light config. The filter is not sized.
// Re-measured 2026-10-29 on captain at 0e32e225 (unreliable ring 16Q, resend
// link 12W, descriptor 72 bytes): DEV ON client was 22967743, now 22971807;
// DEV ON server was 1603276694, now 1607276560.
// Re-measured 2026-10-29 by the windows Release (a) and (c) CI legs:
// the client config's loopback_hub pointer and its rounding add 32 bytes to
// the DEV ON client, which was 22971807 and is now 22971871; the contract
// audit adds 64 more, 22971935 by the same legs. The DEV OFF leg
// stayed within its figure.
// The thread record holds its sleep timer: 8 bytes per thread, and 8 more
// for the timers ledger generation in a development build. The clients one
// network thread makes the DEV ON client 22971951 (the windows Release (a)
// and (c) legs) and the DEV OFF client at most 22182891.
#if ROCKETNET_DEV
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1608368704 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22971951 )
#else
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1597987642 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22182891 )
#endif // #if ROCKETNET_DEV
#elif defined( __APPLE__ )
// macOS adds, over the figures before them: per socket, the send failure
// counts by call and errno (3 calls x 13 kinds x 8 bytes, plus the first
// other errno: 320 bytes; the client opens 2 sockets, so 640); on the server,
// the TX banks sized at 16 packets per client (3 banks x 15,000 more packets
// x 1500 bytes at 1000 clients: 67,500,000), its socket's 320 bytes, and 231
// bytes of the send handoff's per-path drop counts and allocation rounding.
// Re-measured 2026-10-27 on macOS arm64 (studio.local), Release, clang 21,
// ROCKETNET_DEV=ON, after merging origin/main (descriptor 80 bytes with
// span_next plus schedule ticket/epoch): server was 1591206436, now
// 1603277086 (grew by 12070650). Generation, owner epoch, enqueue ticket
// and hole bound 64 stayed.
// Re-measured 2026-10-28 on mini-m5 at fd3cc227 (unreliable ring 16Q, resend
// link 12W): client was 22746605, now 22754581. At 0e32e225 (descriptor
// 72 bytes, span links) server was 1603277086, now 1607276904; client unchanged.
#if ROCKETNET_DEV
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1608369048 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22754581 )
#else
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1585902216 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 21960801 )
#endif // #if ROCKETNET_DEV
#else
// Re-measured 2026-10-27 on hetzner2, Release, clang++ 18.1.3,
// ROCKETNET_DEV=ON, head d00cf56e517dad7477ce41059d48e276c95aa970.
// Server was 3238578316 (now 3243578316, grew by 5000000).
// Client was 22890053 (now 22895109, grew by 5056).
// Re-measured 2026-10-28 on hetzner2 at fd3cc227 (unreliable ring 16Q,
// resend link 12W): client was 22895109, now 22904141. At 0e32e225
// (descriptor 72 bytes, span links) server was 3243578316, now 3247578126.
// Generation, owner epoch, enqueue ticket and hole bound 64 stayed.
// Re-measured 2026-10-29 by the ubuntu-24.04 Debug CI leg: the
// client config's loopback_hub pointer adds 8 bytes to the client, which was
// 22904141 and is now 22904149; the server figure is unchanged.
// The non-dev figures below were not re-measured: this file is not linked
// when ROCKETNET_DEV=OFF.
#if ROCKETNET_DEV
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 3248670270ULL )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22904149 )
#else
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 3226203446ULL )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22110441 )
#endif // #if ROCKETNET_DEV
#endif // #if defined( _WIN32 )

static void light_message_fields_server( const struct rocketnet_server_config_t * config )
{
    check( config->max_fragmented_messages == 1 );
    check( config->per_client_send_kbps == 256 );
    check( config->num_channels == 8 );
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
    check( config->reliable_message_timeout_ms == 10000 );
}

static void light_message_fields_client( const struct rocketnet_client_config_t * config )
{
    check( config->max_fragmented_messages == 1 );
    check( config->send_kbps == 256 );
    check( config->num_channels == 8 );
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
    check( config->reliable_message_timeout_ms == 10000 );
}

static void test_light_profile_server_1000_required_memory()
{
    struct rocketnet_server_config_t config;
    check( rocketnet_server_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );
    check( config.max_clients == 1000 );
    light_message_fields_server( &config );

    config.max_clients = 1000;

    const size_t bytes = rocketnet_server_required_memory( &config );
    check( bytes > 0 );
    if ( bytes != LIGHT_SERVER_1000_MAX_BYTES )
    {
        printf( "light server lifecycle bytes: actual=%zu expected=%zu\n", bytes, LIGHT_SERVER_1000_MAX_BYTES );
    }
    check( bytes <= LIGHT_SERVER_1000_MAX_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_server_1000_required_memory );

static void test_light_profile_client_required_memory()
{
    struct rocketnet_client_config_t config;
    check( rocketnet_client_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );
    light_message_fields_client( &config );

    const size_t bytes = rocketnet_client_required_memory( &config );
    check( bytes > 0 );
    if ( bytes != LIGHT_CLIENT_MAX_BYTES )
    {
        printf( "light client lifecycle bytes: actual=%zu expected=%zu\n", bytes, LIGHT_CLIENT_MAX_BYTES );
    }
    check( bytes <= LIGHT_CLIENT_MAX_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_client_required_memory );

static void test_light_profile_configs_create()
{
    // create does not bind, and it takes no clock. update at 1000.0 on a
    // stopped server and a disconnected client opens no socket.

    const double now = 1000.0;

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_light_config( &server_config ) == ROCKETNET_STATUS_OK );

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
