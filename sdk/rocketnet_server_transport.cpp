/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_fragment.h"
#include "rocketnet_config.h"

#include <sodium.h>
#include <atomic>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// The transport driver is selected at RUNTIME (#106): one build carries
// every driver its platform has, and create walks the config's ordered
// candidate list -- config->driver, overridden by ROCKETNET_DRIVER --
// left to right, first grant wins. On linux that is the runtime ladder:
// probe XDP (privilege and NIC support), fall to the non-root GSO driver
// (SO_REUSEPORT sharding + sendmmsg/recvmmsg + UDP GSO/GRO, per the #81
// measurements), floor at portable. "auto" walks the ladder; ANY explicit
// list is exactly that list, so a one-element list is a pin -- that driver
// or loud failure, never a silent fall that would mask a provisioning
// error. The granted tier is reported request-vs-granted style:
// rocketnet_server_transport_driver plus one log line naming the grant and
// why each earlier candidate refused. The GSO driver's own feature probing
// and degradation remains runtime inside the driver, below this ladder.

#if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )
#error driver selection is runtime now (#106): drop ROCKETNET_SERVER_XDP / ROCKETNET_SERVER_GSO and select with config driver or ROCKETNET_DRIVER
#endif // #if defined( ROCKETNET_SERVER_XDP ) || defined( ROCKETNET_SERVER_GSO )

// The staged drivers share one shape behind one set of names: packets stage
// into the driver from any thread between flushes (start/finish/abort), the
// main thread flushes with send_packets and drains with receive_packets/
// process_packets. The XDP socket and the GSO driver both satisfy it; the
// portable socket keeps its own per-packet path below. Both staged drivers
// are linux; everywhere else the staged paths compile out and the portable
// path is the whole story.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#define ROCKETNET_SERVER_HAS_STAGED 1
#include "rocketnet_server_socket.h"
#include "rocketnet_server_gso.h"
#include <sys/types.h>
#include <unistd.h>
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#define ROCKETNET_SERVER_HAS_STAGED 0
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#define ROCKETNET_SERVER_MAX_RECEIVE_PACKETS 256
#define ROCKETNET_SERVER_MAX_SEND_PACKETS    256

struct rocketnet_server_send_buffer_t
{
    int num_packets;
    rocketnet_address_t to[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    int packet_bytes[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_SERVER_MAX_SEND_PACKETS];
};

// the receive batch on the non-xdp drivers: one pump call fills these, then
// processing walks them. the stride is the receive ceiling, so any packet
// the wire admits fits a slot; the pump moves the batch in as few syscalls
// as the platform earned (#100) and every platform serves the same call.

struct rocketnet_server_receive_buffer_t
{
    rocketnet_address_t from[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS];
    int packet_bytes[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS];
    uint8_t data[ROCKETNET_RECEIVE_CEILING*ROCKETNET_SERVER_MAX_RECEIVE_PACKETS];
};

// the portable payload ring: what the acquire/commit surface fills between
// update and the flush, from any number of game threads. slot acquisition is
// one atomic add; each slot's info sits alone on its cache line so
// concurrent finishes never share one; the frame stride keeps every root
// 16-aligned. the response ring above stays single-threaded under update
// and is deliberately not this.

#define ROCKETNET_SERVER_PAYLOAD_RING_SIZE      ROCKETNET_SERVER_MAX_SEND_PACKETS

struct alignas(64) rocketnet_server_payload_slot_info_t
{
    rocketnet_address_t to;
    int wire_bytes;
    int client_index;
};

struct rocketnet_server_payload_ring_t
{
    uint8_t padding_0[4096];

    volatile uint64_t num_packets;

    uint8_t padding_1[4096];

    struct rocketnet_server_payload_slot_info_t info[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];

    uint8_t padding_2[4096];

    alignas(16) uint8_t data[ROCKETNET_FRAME_STRIDE*ROCKETNET_SERVER_PAYLOAD_RING_SIZE];

    uint8_t padding_3[4096];
};

static_assert( ROCKETNET_FRAME_STRIDE % 16 == 0, "frame stride must keep every slot root 16-aligned" );
static_assert( ROCKETNET_FRAME_STRIDE >= ROCKETNET_FRAME_WIRE_OFFSET + ROCKETNET_RECEIVE_CEILING, "a slot must hold the slack and a full wire packet" );
static_assert( ROCKETNET_FRAME_PAYLOAD_OFFSET % 16 == 0, "the payload pointer handed to the caller must be 16-aligned" );

// the stats bucket ring, the space game's pattern: one bucket per frame,
// accumulation riding the walks the transport already makes, published
// per-second numbers summing only buckets younger than one second. the ring
// is deep enough that a bucket outlives the second it contributes to at any
// sane frame rate; a bucket with frame zero has never been written.

#define ROCKETNET_NUM_SERVER_STATS_FRAMES 256

struct alignas(64) rocketnet_server_stats_frame_t
{
    double time;
    uint64_t frame;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

// the payload batch from the last update -- capacity ROCKETNET_MAX_PAYLOADS_PER_UPDATE
// covers a full drain. the public surface iterates by index and never sees this layout.

struct rocketnet_server_payloads_t
{
    int num_payloads;
    int client_index[ROCKETNET_MAX_PAYLOADS_PER_UPDATE];
    uint8_t * payload_data[ROCKETNET_MAX_PAYLOADS_PER_UPDATE];
    int payload_bytes[ROCKETNET_MAX_PAYLOADS_PER_UPDATE];
};

struct rocketnet_server_transport_t
{
    void * context;
    int state;
    rocketnet_address_t bind_address;
    rocketnet_address_t public_address;

    // the granted driver (ROCKETNET_DRIVER_*), decided by the ladder at
    // create and constant for the transport's life -- the runtime fork
    // every seam function below branches on, one predictable branch where
    // the compile-time build used to fork

    int driver;

    rocketnet_platform_socket_t * socket;               // the portable path's socket, live on the portable and msgx grants

#if ROCKETNET_SERVER_HAS_STAGED
    struct rocketnet_server_socket_t * xdp_socket;      // live on the xdp grant
    struct rocketnet_server_gso_t * gso_socket;         // live on the gso grant
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    struct rocketnet_counters_t counters;
    struct rocketnet_server_protocol_t protocol;

    // the portable path's buffers ride the struct on every platform now
    // that the driver is runtime: about a megabyte a staged grant never
    // touches, priced once per server against carrying the runtime fork
    // without allocation branches

    struct rocketnet_server_send_buffer_t send_buffer;
    struct rocketnet_server_receive_buffer_t receive_buffer;
    struct rocketnet_server_payload_ring_t payload_ring;

    double time;                                        // the game's clock, last seen
    uint64_t frame;                                     // advanced once per update; indexes the stats ring

    // the packet id counter behind the acquire/commit surface on the
    // portable driver: unique and monotonic, the join key the acked-packet
    // batch surfaces. the xdp driver's own packet id is already a global
    // monotonic fetch_add and is used directly.

    volatile uint64_t next_packet_id;

    // the fragmentation shape from the config, and the send arena for
    // over-wire-capacity requests -- allocated once at create, to the
    // configured maximum. the reassembly set lives on the protocol.

    int max_payload_bytes;
    struct rocketnet_send_arena_t * send_arena;

    struct rocketnet_server_transport_stats_t server_stats;
    struct rocketnet_server_stats_frame_t stats_frames[ROCKETNET_NUM_SERVER_STATS_FRAMES];

#if ROCKETNET_SERVER_HAS_STAGED
    volatile uint64_t send_staged;                      // bumped by every producer; fills use the atomic add
    uint64_t send_flushed;                              // main thread only

    // payload sends on the staged drivers seal straight into the driver's
    // own staging from any game thread, so there is no transport-side ring
    // for the flush walk to read; each finish adds its packet here instead
    // -- the same relaxed atomic the path already pays for send_staged --
    // and send_packets folds the pair into the current bucket on the main
    // thread.
    volatile uint64_t stats_pending_packets_sent;
    volatile uint64_t stats_pending_bytes_sent;
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    struct rocketnet_server_payloads_t payloads;
    // +8: serialize's BitReader loads 8-byte windows at byte granularity, so
    // any buffer a ReadStream is bound over must extend at least 8 bytes past
    // the end of its data -- this is the last member, so the slack is real.
    uint8_t payload_storage[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES*ROCKETNET_MAX_PAYLOADS_PER_UPDATE + 8];
};

// The runtime fork's inner seam: the staged drivers share one shape, so
// every staged call dispatches on which staged socket is live. One branch
// per call, resolved the same way for a transport's whole life -- the
// per-batch calls pay it once per batch, and the per-packet staging calls
// pay one predicted branch where the compile-time build paid none.

#if ROCKETNET_SERVER_HAS_STAGED

static bool rocketnet_server_transport_staged( struct rocketnet_server_transport_t * server_transport )
{
    return server_transport->driver == ROCKETNET_DRIVER_XDP || server_transport->driver == ROCKETNET_DRIVER_GSO;
}

static uint8_t * rocketnet_server_driver_start_packet( struct rocketnet_server_transport_t * server_transport, const rocketnet_address_t * to, uint64_t * packet_id )
{
    if ( server_transport->xdp_socket )
        return rocketnet_server_socket_start_packet( server_transport->xdp_socket, to, packet_id );
    return rocketnet_server_gso_start_packet( server_transport->gso_socket, to, packet_id );
}

static void rocketnet_server_driver_finish_packet( struct rocketnet_server_transport_t * server_transport, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    if ( server_transport->xdp_socket )
        rocketnet_server_socket_finish_packet( server_transport->xdp_socket, packet_id, packet_data, packet_bytes );
    else
        rocketnet_server_gso_finish_packet( server_transport->gso_socket, packet_id, packet_data, packet_bytes );
}

static void rocketnet_server_driver_abort_packet( struct rocketnet_server_transport_t * server_transport, uint64_t packet_id, uint8_t * packet_data )
{
    if ( server_transport->xdp_socket )
        rocketnet_server_socket_abort_packet( server_transport->xdp_socket, packet_id, packet_data );
    else
        rocketnet_server_gso_abort_packet( server_transport->gso_socket, packet_id, packet_data );
}

static void rocketnet_server_driver_send_packets( struct rocketnet_server_transport_t * server_transport )
{
    if ( server_transport->xdp_socket )
        rocketnet_server_socket_send_packets( server_transport->xdp_socket );
    else
        rocketnet_server_gso_send_packets( server_transport->gso_socket );
}

static void rocketnet_server_driver_receive_packets( struct rocketnet_server_transport_t * server_transport )
{
    if ( server_transport->xdp_socket )
        rocketnet_server_socket_receive_packets( server_transport->xdp_socket );
    else
        rocketnet_server_gso_receive_packets( server_transport->gso_socket );
}

static struct rocketnet_server_socket_process_packets_t * rocketnet_server_driver_process_packets( struct rocketnet_server_transport_t * server_transport )
{
    if ( server_transport->xdp_socket )
        return rocketnet_server_socket_process_packets( server_transport->xdp_socket );
    return rocketnet_server_gso_process_packets( server_transport->gso_socket );
}

#endif // #if ROCKETNET_SERVER_HAS_STAGED

// The send frame seam every driver serves: reserve hands back a frame to
// seal complete wire bytes into, commit stamps its length, abort returns it
// unused. The portable driver batches frames and flushes them through the
// socket at the end of update; the staged drivers seal straight into their
// own send staging -- the af_xdp umem path or the GSO sendmmsg batch --
// zero copies between the seal and the flush.

struct rocketnet_server_frame_t
{
    uint8_t * data;
    uint64_t packet_id;             // the staged drivers' bookkeeping id; unused on the portable path
};

static uint8_t * rocketnet_server_reserve_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                                const rocketnet_address_t * to,
                                                                struct rocketnet_server_frame_t * frame )
{
#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        frame->data = rocketnet_server_driver_start_packet( server_transport, to, &frame->packet_id );
        if ( !frame->data )
        {
            rocketnet_spam( "server transport send staging full. packet dropped" );
        }
        return frame->data;
    }
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;

    if ( send_buffer->num_packets >= ROCKETNET_SERVER_MAX_SEND_PACKETS )
    {
        rocketnet_spam( "server transport send buffer full. packet dropped" );
        frame->data = NULL;
        return NULL;
    }

    const int index = send_buffer->num_packets;
    send_buffer->to[index] = *to;
    send_buffer->packet_bytes[index] = 0;
    frame->data = send_buffer->data + index * ROCKETNET_FRAME_SIZE;
    return frame->data;
}

static void rocketnet_server_commit_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                          struct rocketnet_server_frame_t * frame,
                                                          int packet_bytes )
{
    rocketnet_assert( frame->data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    // every commit through this seam -- responses, keep-alives, disconnect
    // bursts -- runs on the main thread, so it lands in the current stats
    // bucket directly

    const int stats_index = (int) ( server_transport->frame % ROCKETNET_NUM_SERVER_STATS_FRAMES );
    server_transport->stats_frames[stats_index].packets_sent++;
    server_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( packet_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_driver_finish_packet( server_transport, frame->packet_id, frame->data, packet_bytes );
        return;
    }
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;
    send_buffer->packet_bytes[send_buffer->num_packets] = packet_bytes;
    send_buffer->num_packets++;
}

static void rocketnet_server_abort_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                         struct rocketnet_server_frame_t * frame )
{
    if ( !frame->data )
        return;

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
        rocketnet_server_driver_abort_packet( server_transport, frame->packet_id, frame->data );
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    // on the portable path the reserved slot is reused by the next
    // reserve; nothing to undo

    frame->data = NULL;
}

// rocketnet_server_transport_default_config lives in rocketnet_config.cpp,
// written from the config table so every default is declared exactly once

void rocketnet_server_transport_dev_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_assert( config );
    rocketnet_server_transport_default_config( config );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( config->server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( config->server_secret_key, rocketnet_dev_server_secret_key, 32 );
}

static int rocketnet_server_config_has_keys( const struct rocketnet_server_transport_config_t * config )
{
    uint8_t zero[32];
    memset( zero, 0, sizeof(zero) );
    return memcmp( config->server_secret_key, zero, 32 ) != 0 &&
           memcmp( config->server_public_key, zero, 32 ) != 0 &&
           memcmp( config->backend_public_key, zero, 32 ) != 0;
}

// the pre-#106 env spellings kept working under the ROCKETNET_PUMP pattern:
// a deprecated alias is honored only where its successor is unset, strict-
// parsed by the same rules, and every use is logged loudly naming the
// successor; set beside the successor it yields, loudly. these two used to
// be silent raw getenv reads inside the xdp driver.

static bool rocketnet_server_apply_env_alias( const char * alias_name, const char * successor_name, int * field )
{
    const char * value = rocketnet_platform_getenv( alias_name );
    if ( !value || value[0] == '\0' )
        return true;

    const char * successor_value = rocketnet_platform_getenv( successor_name );
    if ( successor_value && successor_value[0] != '\0' )
    {
        rocketnet_warn( "server config ignores %s: its successor %s is set", alias_name, successor_name );
        return true;
    }

    int parsed;
    if ( !rocketnet_config_parse_int_value( value, &parsed ) )
    {
        rocketnet_error( "server config could not parse %s=\"%s\" as an integer", alias_name, value );
        return false;
    }

    rocketnet_info( "server config override %s=%d (deprecated alias; %s is the successor) (displaces %d)", alias_name, parsed, successor_name, *field );
    *field = parsed;
    return true;
}

static bool rocketnet_server_apply_env_alias_string( const char * alias_name, const char * successor_name, char * field, int capacity )
{
    const char * value = rocketnet_platform_getenv( alias_name );
    if ( !value || value[0] == '\0' )
        return true;

    const char * successor_value = rocketnet_platform_getenv( successor_name );
    if ( successor_value && successor_value[0] != '\0' )
    {
        rocketnet_warn( "server config ignores %s: its successor %s is set", alias_name, successor_name );
        return true;
    }

    if ( (int) strlen( value ) >= capacity )
    {
        rocketnet_error( "server config %s value is too long (%d character maximum)", alias_name, capacity - 1 );
        return false;
    }

    rocketnet_info( "server config override %s=%s (deprecated alias; %s is the successor) (displaces %s)", alias_name, value, successor_name, field );
    snprintf( field, (size_t) capacity, "%s", value );
    return true;
}

struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context,
                                                                         const char * public_address_string,
                                                                         const struct rocketnet_server_transport_config_t * config,
                                                                         double time )
{
    rocketnet_assert( public_address_string );

    (void) time;

    // the config layering (#106): DEFAULT < STRUCT < ENV. NULL config is
    // the default shape; the caller's struct is the middle layer; the
    // environment overlays both through the config table, every override
    // logged loudly with the value it displaced. validation refuses
    // loudly and never clamps.

    struct rocketnet_server_transport_config_t effective_config;
    if ( config )
        effective_config = *config;
    else
        rocketnet_server_transport_default_config( &effective_config );

    // the env layer's public_address is snapshotted against so its SCOPE
    // can be enforced below: the env override redirects the FIRST server
    // create in the process and is then consumed -- an operator's deployed
    // binary has one server transport and gets exactly the redirect they
    // asked for, while a process with many creates (the test suite, a
    // soak) is not wholesale redirected by one ambient variable. the
    // struct field keeps per-create scope: a caller who sets it in code
    // said what they meant, every time.

    char struct_public_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    memcpy( struct_public_address, effective_config.public_address, sizeof(struct_public_address) );

    int num_config_fields = 0;
    const struct rocketnet_config_field_t * config_fields = rocketnet_server_transport_config_fields( &num_config_fields );

    if ( !rocketnet_config_apply_env( "server", &effective_config, config_fields, num_config_fields ) )
        return NULL;

    if ( !rocketnet_server_apply_env_alias( "ROCKETNET_SERVER_NUM_QUEUES", "ROCKETNET_NUM_XDP_QUEUES", &effective_config.num_xdp_queues ) )
        return NULL;

    if ( !rocketnet_server_apply_env_alias( "ROCKETNET_SERVER_NUM_OS_CPUS", "ROCKETNET_NUM_OS_CPUS", &effective_config.num_os_cpus ) )
        return NULL;

    if ( !rocketnet_server_apply_env_alias_string( "ROCKETNET_SERVER_PUBLIC_ADDRESS", "ROCKETNET_PUBLIC_ADDRESS", effective_config.public_address, (int) sizeof(effective_config.public_address) ) )
        return NULL;

    if ( !rocketnet_config_validate( "server", &effective_config, config_fields, num_config_fields ) )
        return NULL;

    config = &effective_config;

    // the config's public_address, when set, displaces the create call's
    // address parameter HERE, before anything reads it -- so the address
    // the ladder verifies, the address the granted driver binds, and the
    // address connect tokens validate against can never diverge. the env
    // layer's version of it is first-create-consumes, per the scope note
    // above.

    // the env override's consumption flag: atomic because two transports
    // in one process is a blessed case and nothing serializes creates, and
    // CONSUMED ONLY AFTER A FULLY SUCCESSFUL CREATE -- a first create that
    // refuses (bad keys, exhausted ladder) must not eat the deployed
    // binary's redirect on the retry. two concurrent first creates racing
    // the same override is unsupported and stated: creates are setup acts,
    // not fill-phase acts.

    static std::atomic<bool> env_public_address_consumed( false );

    bool consuming_env_public_address = false;

    if ( strncmp( effective_config.public_address, struct_public_address, sizeof(struct_public_address) ) != 0 )
    {
        // the env layer (or its deprecated alias) set it

        if ( env_public_address_consumed.load() )
        {
            rocketnet_info( "the public_address env override was consumed by an earlier server create in this process. using the create parameters" );
            memcpy( effective_config.public_address, struct_public_address, sizeof(struct_public_address) );
        }
        else
        {
            consuming_env_public_address = true;
        }
    }

    if ( config->public_address[0] != '\0' )
    {
        rocketnet_info( "server config public_address %s displaces the create parameter %s", config->public_address, public_address_string );
        public_address_string = config->public_address;
    }

    const int max_clients = config->max_clients;        // the table validated the range

    // a server with no keys is refused, never defaulted: default keys are how
    // every deployment of a protocol ends up sharing one identity. development
    // wants rocketnet_server_transport_dev_config, which fills the published keys loudly;
    // a deployed binary can also carry its keys on the env surface.

    if ( !rocketnet_server_config_has_keys( config ) )
    {
        rocketnet_error( "server config has no keys: fill rocketnet_server_transport_dev_config() for development, or supply production keys" );
        return NULL;
    }

    // the driver candidate list parses before anything allocates: a typo
    // fails here, loudly, listing the valid names

    int num_available_drivers = 0;
    const int * available_drivers = rocketnet_server_driver_availability( &num_available_drivers );

    struct rocketnet_driver_ladder_t ladder;
    if ( !rocketnet_driver_ladder_parse( config->driver, available_drivers, num_available_drivers, &ladder ) )
        return NULL;

    rocketnet_info( "server transport public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        rocketnet_error( "could not parse public address" );
        return NULL;
    }

    rocketnet_address_t bind_address = public_address;
    if ( bind_address.type == ROCKETNET_ADDRESS_IPV4 )
        memset( bind_address.data.ipv4, 0, sizeof(bind_address.data.ipv4) );
    else
        memset( bind_address.data.ipv6, 0, sizeof(bind_address.data.ipv6) );

    struct rocketnet_server_transport_t * server_transport = (struct rocketnet_server_transport_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_transport_t) );
    if ( !server_transport )
        return NULL;

    memset( server_transport, 0, sizeof(struct rocketnet_server_transport_t) );

    server_transport->context = context;

    // the fragmentation shape: zero means the defaults; the table already
    // refused anything out of range, loudly, never clamped

    server_transport->max_payload_bytes = config->max_payload_bytes > 0 ? config->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int num_reassembly_slots = config->num_reassembly_slots > 0 ? config->num_reassembly_slots : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;

    uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES];
    memcpy( tag, config->filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    const int development_keys = memcmp( config->server_public_key, rocketnet_dev_server_public_key, 32 ) == 0;
    (void) development_keys;

    // THE LADDER (#106): candidates tried left to right, first grant wins.
    // an operator who wrote the list owns the list -- nothing is appended
    // -- and a one-element list is a pin that refuses rather than falls.
    // each refusal logs loudly; the exhausted list fails naming what was
    // skipped for platform and what refused for grant.

    int granted_driver = -1;

    char refused_names[128];
    refused_names[0] = '\0';

    bool refused_seen[ROCKETNET_NUM_DRIVERS];
    memset( refused_seen, 0, sizeof(refused_seen) );

    // #109: WHICH rungs the port-zero rule refused, so an exhausted ladder
    // can restate the rule at the level the failure itself is reported.
    // The names are collected rather than a bare flag because a mixed list
    // -- say "xdp,gso" where gso refused for its own queue geometry --
    // would make "no driver here can discover a port" a lie about gso. The
    // error names the rungs the rule actually refused, and nothing else.

    char port_zero_refused_names[128];
    port_zero_refused_names[0] = '\0';

    bool port_zero_refused_seen[ROCKETNET_NUM_DRIVERS];
    memset( port_zero_refused_seen, 0, sizeof(port_zero_refused_seen) );

    for ( int i = 0; i < ladder.num_candidates && granted_driver < 0; i++ )
    {
        const int candidate = ladder.candidates[i];

        // #109: a port-zero public address asks the KERNEL for an
        // ephemeral port, and the adoption below reads the granted port
        // back out of bind_address -- so the address is only meaningful on
        // a rung whose create discovers what it bound and hands it back.
        // The xdp rung does not: it takes an address STRING, binds the
        // port it is given, and leaves bind_address.port at zero, so the
        // adoption adopts zero and the transport silently advertises port
        // 0 -- tokens minted from rocketnet_server_transport_port() name
        // port 0, the cookie gate never matches, and nothing says why.
        //
        // This is asked of EVERY candidate rather than patched into the
        // one rung that fails it today, because #110 made create a loop
        // and the question is a property of each driver's bind
        // (rocketnet_driver_discovers_bound_port). It is a rung refusal
        // like any other: "auto" falls past xdp to a rung that discovers,
        // and only a PIN on a non-discovering rung refuses the create --
        // loudly, naming the rule, the way the pin law already works.

        if ( public_address.port == 0 && !rocketnet_driver_discovers_bound_port( candidate ) )
        {
            // named once however many times the operator listed it: the
            // ladder parse does not deduplicate, and "xdp, xdp" in an
            // error reads as a bug in the error rather than a repeat in
            // the list.
            //
            // keyed on the driver ID, not on a substring search of the
            // accumulated names. The hazard runs in the direction that is
            // easy to get backwards: strstr( "gso", "gso2" ) is NULL, so a
            // NEW longer name is not swallowed by a shorter accumulated
            // one -- but strstr( "gso2", "gso" ) FINDS, so an accumulated
            // longer name silently swallows a later legitimate shorter
            // one. No two names collide today; an id key cannot collide
            // whatever anyone adds.

            if ( candidate >= 0 && candidate < ROCKETNET_NUM_DRIVERS && !port_zero_refused_seen[candidate] )
            {
                port_zero_refused_seen[candidate] = true;

                const size_t used = strlen( port_zero_refused_names );
                snprintf( port_zero_refused_names + used, sizeof(port_zero_refused_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( candidate ) );
            }

            rocketnet_warn( "server driver %s refused: a port-zero public address asks the kernel for an ephemeral port, and the %s driver binds the port it is given and discovers nothing -- it would advertise port 0. give this server a real port, or leave a driver that discovers one in the list",
                            rocketnet_driver_name( candidate ), rocketnet_driver_name( candidate ) );
        }
        else switch ( candidate )
        {
#if ROCKETNET_SERVER_HAS_STAGED

            case ROCKETNET_DRIVER_XDP:
            {
                // create is verify-and-bind (Glenn's ruling on #106: "Do
                // an early check to see if we have XDP before changing the
                // machine"): its first act is the non-mutating pre-flight,
                // which verifies the machine is provisioned --
                // scripts/rocketnet-provision does the provisioning, the
                // library never does -- naming each unmet condition as its
                // own operator errand, against the same resolved address
                // this rung would bind. a refusal falls through here.

                const int num_xdp_queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : 7;
                const int num_os_cpus = config->num_os_cpus > 0 ? config->num_os_cpus : 1;

                server_transport->xdp_socket = rocketnet_server_socket_create( context, public_address_string, tag, num_xdp_queues, num_os_cpus );
                if ( server_transport->xdp_socket )
                    granted_driver = ROCKETNET_DRIVER_XDP;
            }
            break;

            case ROCKETNET_DRIVER_GSO:
            {
                // the queue count rides the config's num_xdp_queues field:
                // it is the staged drivers' queue count whichever grants,
                // kept under its historic name for ABI stability with the
                // language bindings. zero means the driver default. bounds
                // are per driver (#110 review): the table's range is
                // structural, and this rung refuses -- never clamps -- a
                // count past the gso driver's own geometry.

                if ( config->num_xdp_queues > ROCKETNET_GSO_MAX_QUEUES )
                {
                    rocketnet_warn( "gso driver refused: num_xdp_queues %d is past the gso driver's %d queue geometry", config->num_xdp_queues, ROCKETNET_GSO_MAX_QUEUES );
                    break;
                }

                rocketnet_address_t gso_bind_address = bind_address;

                server_transport->gso_socket = rocketnet_server_gso_create( context, &gso_bind_address, config->num_xdp_queues, 0 );
                if ( server_transport->gso_socket )
                {
                    bind_address = gso_bind_address;
                    granted_driver = ROCKETNET_DRIVER_GSO;
                }
            }
            break;

#endif // #if ROCKETNET_SERVER_HAS_STAGED

            case ROCKETNET_DRIVER_MSGX:
            case ROCKETNET_DRIVER_PORTABLE:
            {
                // each attempt binds its own copy of the address, so a
                // refused attempt's discovered ephemeral port never
                // constrains the next

                rocketnet_address_t attempt_address = bind_address;

                server_transport->socket = rocketnet_platform_socket_create_with_pump( context, &attempt_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, rocketnet_driver_name( candidate ) );
                if ( server_transport->socket )
                {
                    bind_address = attempt_address;
                    granted_driver = candidate;
                }
            }
            break;

            default:
            break;
        }

        if ( granted_driver < 0 )
        {
            rocketnet_warn( "server driver %s refused%s", rocketnet_driver_name( candidate ),
                            i + 1 < ladder.num_candidates ? ". trying the next candidate" : "" );

            // deduped on the same rule as the port-zero list below it: a
            // repeated name in the operator's list is a repeat in the
            // list, not two refusals worth reading, and two adjacent
            // summary lines disagreeing about that reads as a bug in one
            // of them

            if ( candidate >= 0 && candidate < ROCKETNET_NUM_DRIVERS && !refused_seen[candidate] )
            {
                refused_seen[candidate] = true;

                const size_t used = strlen( refused_names );
                snprintf( refused_names + used, sizeof(refused_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( candidate ) );
            }
        }
    }

    if ( granted_driver < 0 )
    {
        char skipped_names[128];
        skipped_names[0] = '\0';
        for ( int i = 0; i < ladder.num_skipped; i++ )
        {
            const size_t used = strlen( skipped_names );
            snprintf( skipped_names + used, sizeof(skipped_names) - used, "%s%s", used > 0 ? ", " : "", rocketnet_driver_name( ladder.skipped[i] ) );
        }

        rocketnet_error( "server driver ladder exhausted with no grant. refused: %s. skipped for platform: %s",
                         refused_names[0] != '\0' ? refused_names : "(none)",
                         skipped_names[0] != '\0' ? skipped_names : "(none)" );

        // #109 asked for an error naming the rule, and the per-rung warn
        // above is not that: a deployed binary, the test suite and the C#
        // smoke all run at the ERROR level, where a warn is invisible. So
        // the rule is restated HERE, at the level the failure itself is
        // reported -- on the fall it stays a warn, because a fall is not a
        // failure and an error line would be a lie about the outcome.

        if ( port_zero_refused_names[0] != '\0' )
        {
            // the remedy does NOT point at the per-rung warns: this error
            // exists precisely because the reader is at ERROR level, where
            // those warns are invisible. it has to stand alone.

            rocketnet_error( "the public address carries port zero, which asks the kernel for an ephemeral port: %s cannot discover the port it was granted and hand it back, so this server would advertise port 0. give this server a real port, or list a driver that discovers a granted port (portable, msgx, gso). raise the log level to WARN to see why each candidate refused",
                             port_zero_refused_names );
        }

        rocketnet_server_transport_destroy( server_transport );
        return NULL;
    }

    server_transport->driver = granted_driver;

    if ( refused_names[0] != '\0' )
    {
        rocketnet_info( "server driver ladder granted %s after refusals: %s", rocketnet_driver_name( granted_driver ), refused_names );
    }

    // the started line names the granted tier -- performance claims attach
    // to what this says, never to what was requested

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const rocketnet_address_t * started_address = granted_driver == ROCKETNET_DRIVER_XDP ? &public_address : &bind_address;
    rocketnet_info( "server transport started on %s [%s] (%s keys)", rocketnet_address_to_string( started_address, address_string ),
                    rocketnet_driver_name( granted_driver ), development_keys ? "development" : "production" );

    // a public address created with port zero adopts the granted bind
    // port, so the advertised address -- the one connect tokens are minted
    // for and validated against -- is the real one, and
    // rocketnet_server_transport_port reads it back

    if ( public_address.port == 0 )
        public_address.port = bind_address.port;

    server_transport->bind_address = bind_address;
    server_transport->public_address = public_address;
    server_transport->state = ROCKETNET_SERVER_TRANSPORT_RUNNING;

    // the protocol, on the configured identity, advertised at the public
    // address -- the address connect tokens are minted for

    struct rocketnet_server_protocol_t * protocol = &server_transport->protocol;
    protocol->protocol_id = config->protocol_id;
    protocol->send_rate = 10.0;
    memcpy( protocol->filter_value, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( protocol->backend_public_key, config->backend_public_key, 32 );
    memcpy( protocol->server_public_key, config->server_public_key, 32 );
    memcpy( protocol->server_secret_key, config->server_secret_key, 32 );

    if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
        protocol->advertised_ipv4 = public_address;
    else
        protocol->advertised_ipv6 = public_address;

    randombytes_buf( protocol->cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    protocol->cookie_lifetime_seconds = 3;
    protocol->handshake_deadline_seconds = 5.0;
    protocol->max_timeout_seconds = 0;
    protocol->allow_negative_timeout = 0;

    protocol->encryption_map = rocketnet_encryption_map_create( context, max_clients * 2 );
    protocol->used_token_history = rocketnet_used_token_history_create( context, max_clients * 8 );
    protocol->admitted_token_set = rocketnet_admitted_token_set_create( context, max_clients * 2 );
    protocol->client_slots = rocketnet_client_slots_create( context, max_clients );
    protocol->acks = rocketnet_server_acks_create( context, max_clients );
    // the delivered mirror sizes to the server's drain bound: the receive
    // path drains at most RECEIVE_PACKETS_PER_CLIENT packets per client
    // slot per update (the process arrays' sizing), and a payload takes at
    // least two fragments, so completions cap at eight per client at the
    // defaults -- exactly the default slot count. connections x slots IS
    // the server's drain bound, which is why it is also its sizing.

    protocol->reassembly = rocketnet_reassembly_set_create( context, max_clients, num_reassembly_slots, server_transport->max_payload_bytes,
                                                            max_clients * num_reassembly_slots );
    protocol->counters = &server_transport->counters;

    // the arena's slot stride stays 16-aligned so every root handed to a
    // caller is aligned for any word-based serializer

    server_transport->send_arena = rocketnet_send_arena_create( context, ROCKETNET_SEND_ARENA_SLOTS,
                                                               ( server_transport->max_payload_bytes + 15 ) & ~15 );

    if ( !protocol->encryption_map || !protocol->used_token_history || !protocol->admitted_token_set || !protocol->client_slots || !protocol->acks || !protocol->reassembly || !server_transport->send_arena )
    {
        rocketnet_server_transport_destroy( server_transport );
        return NULL;
    }

    // the staged drivers assign their own packet ids to ordinary sends from
    // a separate monotonic counter, so the transport's ids for fragmented
    // payloads start in the high half of the space -- the join keys can
    // never collide

    server_transport->next_packet_id = 1;

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
        server_transport->next_packet_id = 1ULL << 63;
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    // the env public_address is consumed only now, with the create whole

    if ( consuming_env_public_address )
    {
        bool expected = false;
        env_public_address_consumed.compare_exchange_strong( expected, true );
    }

    return server_transport;
}

void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );

    if ( server_transport->protocol.encryption_map )
        rocketnet_encryption_map_destroy( server_transport->protocol.encryption_map );
    if ( server_transport->protocol.used_token_history )
        rocketnet_used_token_history_destroy( server_transport->protocol.used_token_history );
    if ( server_transport->protocol.admitted_token_set )
        rocketnet_admitted_token_set_destroy( server_transport->protocol.admitted_token_set );
    if ( server_transport->protocol.client_slots )
        rocketnet_client_slots_destroy( server_transport->protocol.client_slots );
    if ( server_transport->protocol.acks )
        rocketnet_server_acks_destroy( server_transport->protocol.acks );
    if ( server_transport->protocol.reassembly )
        rocketnet_reassembly_set_destroy( server_transport->protocol.reassembly );
    if ( server_transport->send_arena )
        rocketnet_send_arena_destroy( server_transport->send_arena );

    // teardown branches on which socket is live, so a partial create --
    // any rung granted or none -- destroys cleanly

#if ROCKETNET_SERVER_HAS_STAGED

    if ( server_transport->xdp_socket )
    {
        // the xdp socket requires stop then a pump to stopped before destroy

        if ( rocketnet_server_socket_state( server_transport->xdp_socket ) != ROCKETNET_SERVER_SOCKET_STOPPED )
        {
            rocketnet_server_socket_stop( server_transport->xdp_socket );
            rocketnet_server_socket_update( server_transport->xdp_socket );
        }
        rocketnet_server_socket_destroy( server_transport->xdp_socket );
    }

    // the gso driver joins its workers and closes its sockets inline

    if ( server_transport->gso_socket )
        rocketnet_server_gso_destroy( server_transport->gso_socket );

#endif // #if ROCKETNET_SERVER_HAS_STAGED

    if ( server_transport->socket )
        rocketnet_platform_socket_destroy( server_transport->socket );

    rocketnet_clear_and_free( server_transport->context, server_transport, sizeof(struct rocketnet_server_transport_t) );
}

void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPING;
}

int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->state;
}

uint16_t rocketnet_server_transport_port( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->bind_address.port;
}

// the commit barrier, exported: a host process (the C# smoke, an embedding
// that must never commit an XDP data plane) installs it without reaching
// into the linux driver header; a no-op where there is no xdp driver

void rocketnet_forbid_xdp_commit( void )
{
#if ROCKETNET_SERVER_HAS_STAGED
    rocketnet_server_socket_forbid_commit();
#endif // #if ROCKETNET_SERVER_HAS_STAGED
}

// the granted-tier report: the driver that won the ladder at create, by
// name -- what performance claims and operator errands attach to

const char * rocketnet_server_transport_driver( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return rocketnet_driver_name( server_transport->driver );
}

// process the previous stats frames into published per-second numbers, then
// open the next bucket. the sum walks the whole fixed ring every update --
// constant shape, ALWAYS BE LOADED -- and only buckets younger than one
// second contribute, so the published numbers are "per second" by window
// rather than by division.

static void rocketnet_server_update_stats( struct rocketnet_server_transport_t * server_transport, double time )
{
    // process previous stats frames
    {
        uint64_t total_packets_sent = 0;
        uint64_t total_packets_received = 0;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;

        for ( int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; i++ )
        {
            // IMPORTANT: only sum the last second worth of stats frames, we want "packets sent per-second, bandwidth sent per-second etc..."
            if ( server_transport->stats_frames[i].frame == 0 || server_transport->stats_frames[i].time + 1.0 < time )
                continue;

            total_packets_sent += server_transport->stats_frames[i].packets_sent;
            total_packets_received += server_transport->stats_frames[i].packets_received;
            total_bytes_sent += server_transport->stats_frames[i].bytes_sent;
            total_bytes_received += server_transport->stats_frames[i].bytes_received;
        }

        server_transport->server_stats.packets_sent_per_second = total_packets_sent;
        server_transport->server_stats.packets_received_per_second = total_packets_received;

        server_transport->server_stats.bandwidth_sent_kbps = (float) ( ( (double) total_bytes_sent * 8.0 ) / 1000.0 );
        server_transport->server_stats.bandwidth_received_kbps = (float) ( ( (double) total_bytes_received * 8.0 ) / 1000.0 );
    }

    // setup next stats frame
    {
        server_transport->frame++;

        const int stats_index = (int) ( server_transport->frame % ROCKETNET_NUM_SERVER_STATS_FRAMES );

        server_transport->stats_frames[stats_index].time = time;
        server_transport->stats_frames[stats_index].frame = server_transport->frame;
        server_transport->stats_frames[stats_index].packets_sent = 0;
        server_transport->stats_frames[stats_index].packets_received = 0;
        server_transport->stats_frames[stats_index].bytes_sent = 0;
        server_transport->stats_frames[stats_index].bytes_received = 0;
    }
}

void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport, double time )
{
    rocketnet_assert( server_transport );

    server_transport->time = time;

    rocketnet_server_update_stats( server_transport, time );

    if ( server_transport->state == ROCKETNET_SERVER_TRANSPORT_STOPPING )
    {
#if ROCKETNET_SERVER_HAS_STAGED
        if ( server_transport->xdp_socket )
        {
            rocketnet_server_socket_stop( server_transport->xdp_socket );
            rocketnet_server_socket_update( server_transport->xdp_socket );
        }
#endif // #if ROCKETNET_SERVER_HAS_STAGED
        server_transport->state = ROCKETNET_SERVER_TRANSPORT_STOPPED;
        return;
    }

    const double current_time = time;
    const uint64_t current_timestamp = (uint64_t) ::time( NULL );

    server_transport->payloads.num_payloads = 0;

    // the acked-packet batch drains per update: what the blocks processed
    // below acknowledge lands here, valid until the next update

    server_transport->protocol.acks->num_acked = 0;

    // the reassembly's delivered window is per update: payloads completed
    // last update are invalid from here

    rocketnet_reassembly_set_begin_update( server_transport->protocol.reassembly );

    // receive and process, responses sealed straight into send frames

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {

    rocketnet_server_driver_receive_packets( server_transport );

    struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_driver_process_packets( server_transport );

    const int stats_index = (int) ( server_transport->frame % ROCKETNET_NUM_SERVER_STATS_FRAMES );

    for ( int i = 0; i < received->num_packets; i++ )
    {
        server_transport->stats_frames[stats_index].packets_received++;
        server_transport->stats_frames[stats_index].bytes_received += (uint64_t) ( received->packet_bytes[i] + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

        uint8_t * payload = server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE ?
            server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES : NULL;

        uint8_t payload_overflow[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        struct rocketnet_server_frame_t frame;
        uint8_t * response = rocketnet_server_reserve_send_frame( server_transport, &received->from[i], &frame );
        uint8_t response_overflow[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;

        if ( response )
            response += ROCKETNET_FRAME_WIRE_OFFSET;

        rocketnet_server_protocol_process_packet( &server_transport->protocol, &received->from[i],
                                         received->packet_data[i], received->packet_bytes[i],
                                         current_timestamp, current_time,
                                         response ? response : response_overflow, &response_bytes,
                                         payload ? payload : payload_overflow, &payload_bytes, &payload_client_index );

        if ( response )
        {
            if ( response_bytes > 0 )
                rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
            else
                rocketnet_server_abort_send_frame( server_transport, &frame );
        }

        if ( payload && payload_bytes > 0 )
        {
            const int index = server_transport->payloads.num_payloads;
            server_transport->payloads.client_index[index] = payload_client_index;
            server_transport->payloads.payload_data[index] = payload;
            server_transport->payloads.payload_bytes[index] = payload_bytes;
            server_transport->payloads.num_payloads++;
        }

        // a fragment that completed its payload delivers here, in arrival
        // position: the reassembled bytes live in the reassembly's
        // delivered arena, valid until the next update, and join the same
        // batch attributed to their client slot

        struct rocketnet_reassembly_set_t * reassembly = server_transport->protocol.reassembly;
        if ( reassembly->completed_bytes > 0 )
        {
            if ( server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE )
            {
                const int index = server_transport->payloads.num_payloads;
                server_transport->payloads.client_index[index] = payload_client_index;
                server_transport->payloads.payload_data[index] = reassembly->completed_payload;
                server_transport->payloads.payload_bytes[index] = reassembly->completed_bytes;
                server_transport->payloads.num_payloads++;
            }
            else
            {
                rocketnet_spam( "server dropped reassembled payload. receive batch full" );
                server_transport->counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL]++;
            }
        }
    }

    }
    else
#endif // #if ROCKETNET_SERVER_HAS_STAGED
    {

    // one pump call drains the batch, then processing walks it -- the batch
    // is how a platform with batch syscalls earns its keep, and on every
    // other platform the pump is the same per-packet loop this used to be

    struct rocketnet_server_receive_buffer_t * receive_buffer = &server_transport->receive_buffer;

    const int num_received = rocketnet_platform_socket_receive_packets( server_transport->socket,
                                                                        receive_buffer->from,
                                                                        receive_buffer->data,
                                                                        ROCKETNET_RECEIVE_CEILING,
                                                                        receive_buffer->packet_bytes,
                                                                        ROCKETNET_SERVER_MAX_RECEIVE_PACKETS );

    const int stats_index = (int) ( server_transport->frame % ROCKETNET_NUM_SERVER_STATS_FRAMES );

    for ( int i = 0; i < num_received; i++ )
    {
        struct rocketnet_address_t from = receive_buffer->from[i];
        uint8_t * receive_frame = receive_buffer->data + i * ROCKETNET_RECEIVE_CEILING;
        const int packet_bytes = receive_buffer->packet_bytes[i];

        server_transport->stats_frames[stats_index].packets_received++;
        server_transport->stats_frames[stats_index].bytes_received += (uint64_t) ( packet_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

        uint8_t * payload = server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE ?
            server_transport->payload_storage + server_transport->payloads.num_payloads * ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES : NULL;

        uint8_t payload_overflow[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        int payload_client_index = -1;

        struct rocketnet_server_frame_t frame;
        uint8_t * response = rocketnet_server_reserve_send_frame( server_transport, &from, &frame );
        uint8_t response_overflow[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0;

        if ( response )
            response += ROCKETNET_FRAME_WIRE_OFFSET;

        rocketnet_server_protocol_process_packet( &server_transport->protocol, &from, receive_frame, packet_bytes,
                                         current_timestamp, current_time,
                                         response ? response : response_overflow, &response_bytes,
                                         payload ? payload : payload_overflow, &payload_bytes, &payload_client_index );

        if ( response )
        {
            if ( response_bytes > 0 )
                rocketnet_server_commit_send_frame( server_transport, &frame, response_bytes );
            else
                rocketnet_server_abort_send_frame( server_transport, &frame );
        }

        if ( payload && payload_bytes > 0 )
        {
            const int index = server_transport->payloads.num_payloads;
            server_transport->payloads.client_index[index] = payload_client_index;
            server_transport->payloads.payload_data[index] = payload;
            server_transport->payloads.payload_bytes[index] = payload_bytes;
            server_transport->payloads.num_payloads++;
        }

        // a fragment that completed its payload delivers here, in arrival
        // position: the reassembled bytes live in the reassembly's
        // delivered arena, valid until the next update, and join the same
        // batch attributed to their client slot

        struct rocketnet_reassembly_set_t * reassembly = server_transport->protocol.reassembly;
        if ( reassembly->completed_bytes > 0 )
        {
            if ( server_transport->payloads.num_payloads < ROCKETNET_MAX_PAYLOADS_PER_UPDATE )
            {
                const int index = server_transport->payloads.num_payloads;
                server_transport->payloads.client_index[index] = payload_client_index;
                server_transport->payloads.payload_data[index] = reassembly->completed_payload;
                server_transport->payloads.payload_bytes[index] = reassembly->completed_bytes;
                server_transport->payloads.num_payloads++;
            }
            else
            {
                rocketnet_spam( "server dropped reassembled payload. receive batch full" );
                server_transport->counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL]++;
            }
        }
    }

    }

    // the sweep, then per client: the paced keep-alive or -- when the ack
    // debt has reached the threshold with no other block going out -- the
    // standalone ack, into send frames. a sixth of the slots also recompute
    // their published statistics, every slot covered over six updates.

    rocketnet_server_protocol_update( &server_transport->protocol, current_time );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    const int max_clients = rocketnet_client_slots_max_clients( server_transport->protocol.client_slots );
    for ( int i = 0; i < max_clients; i++ )
    {
        if ( ( (uint64_t) ( i % ROCKETNET_CLIENT_STATS_UPDATE_PHASES ) ) == ( acks->update_count % ROCKETNET_CLIENT_STATS_UPDATE_PHASES ) )
            rocketnet_ack_endpoint_update_stats( &acks->endpoints[i] );

        if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, i ) )
            continue;
        int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, i );
        const rocketnet_address_t * client_address = rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, client_address, &frame );
        if ( !frame_data )
            break;
        int send_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server_transport->protocol, i, current_time, frame_data + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( send_bytes == 0 )
            send_bytes = rocketnet_server_protocol_write_ack_if_due( &server_transport->protocol, i, current_time, frame_data + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( send_bytes > 0 )
            rocketnet_server_commit_send_frame( server_transport, &frame, send_bytes );
        else
            rocketnet_server_abort_send_frame( server_transport, &frame );
    }

    acks->update_count++;

    // flush. the per-driver law is stated where callers read it, at
    // send_packets in rocketnet.h: send_packets once per frame after
    // update is the guarantee, and when staged packets actually leave is
    // per driver. the portable driver has always ALSO sent update's own
    // responses and keep-alives inline right here, and much of the test
    // suite's pump loops lean on that without ever calling send_packets.
    // the xdp driver does not have that property: its worker drains only
    // the off buffer, so nothing staged by update leaves until the
    // send_packets flip retargets the buffers -- a divergence the suite
    // has never observed because it has never run against xdp (no root in
    // CI). the gso driver sides with the portable behavior: update's own
    // staging flushes inline on the main thread -- the flush is idempotent
    // -- and send_flushed advances with it, so send_packets flips only for
    // what the fill phase stages after this point. the xdp flip is
    // destructive to run twice on one staging generation, which is what
    // the staged/flushed guard below protects.

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        server_transport->send_staged++;

        if ( server_transport->gso_socket )
        {
            server_transport->send_flushed = server_transport->send_staged;
            rocketnet_server_gso_send_packets( server_transport->gso_socket );
        }

        return;
    }
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    int num_sends = 0;
    for ( int i = 0; i < server_transport->send_buffer.num_packets; i++ )
    {
        if ( server_transport->send_buffer.packet_bytes[i] > 0 )
        {
            sends[num_sends].to = &server_transport->send_buffer.to[i];
            sends[num_sends].packet_data = server_transport->send_buffer.data + i * ROCKETNET_FRAME_SIZE + ROCKETNET_FRAME_WIRE_OFFSET;
            sends[num_sends].packet_bytes = server_transport->send_buffer.packet_bytes[i];
            num_sends++;
        }
    }
    if ( num_sends > 0 )
        rocketnet_platform_socket_send_packets( server_transport->socket, sends, num_sends );
    server_transport->send_buffer.num_packets = 0;
}

int rocketnet_server_transport_num_payloads( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->payloads.num_payloads;
}

int rocketnet_server_transport_payloads( struct rocketnet_server_transport_t * server_transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( payload_data );
    rocketnet_assert( payload_bytes );

    *client_index = server_transport->payloads.client_index;
    *payload_data = server_transport->payloads.payload_data;
    *payload_bytes = server_transport->payloads.payload_bytes;

    return server_transport->payloads.num_payloads;
}

uint8_t * rocketnet_server_transport_payload( struct rocketnet_server_transport_t * server_transport, int payload_index, int * client_index, int * payload_bytes )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( payload_bytes );

    struct rocketnet_server_payloads_t * payloads = &server_transport->payloads;

    if ( payload_index < 0 || payload_index >= payloads->num_payloads )
    {
        *client_index = -1;
        *payload_bytes = 0;
        return NULL;
    }

    *client_index = payloads->client_index[payload_index];
    *payload_bytes = payloads->payload_bytes[payload_index];
    return payloads->payload_data[payload_index];
}

// The acquire/commit send surface. Phase contract, which is what makes the
// whole family lock-free: rocketnet_server_transport_update and
// rocketnet_server_transport_send_packets run on the main thread; start, finish and
// abort may run on any number of game threads strictly between them. During
// that fill phase connections are stable -- only update creates or tears
// down mappings -- so acquisition is one atomic add for the slot and one
// for the sequence, sealing runs on the caller's thread in place in the
// frame, and the flush walks finished slots single-threaded.

uint8_t * rocketnet_server_transport_start_packet( struct rocketnet_server_transport_t * server_transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    memset( context, 0, sizeof(struct rocketnet_packet_context_t) );

    if ( server_transport->state != ROCKETNET_SERVER_TRANSPORT_RUNNING )
        return NULL;

    if ( request_bytes < 1 || request_bytes > server_transport->max_payload_bytes )
    {
        rocketnet_spam( "server start packet refused. request out of range" );
        return NULL;
    }

    rocketnet_address_t to;
    uint64_t sequence;
    if ( !rocketnet_server_protocol_start_payload( &server_transport->protocol, client_index, &to, &sequence ) )
        return NULL;

    uint8_t * frame = NULL;

    if ( request_bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        // above the wire's capacity: the arena backs the write, and
        // finish_packet splits into fragments -- or collapses to one wire
        // packet when the write turns out small. the packet id comes from
        // the transport's own counter on both drivers.

        frame = rocketnet_send_arena_acquire( server_transport->send_arena );
        if ( !frame )
        {
            rocketnet_spam( "server send arena full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_ARENA_FULL], 1 );
            return NULL;
        }

        rocketnet_assert( ( (uintptr_t) frame % 16 ) == 0 );

        context->frame = frame;
        context->payload_index = 0;
        context->payload_capacity = server_transport->max_payload_bytes;
        context->packet_id = rocketnet_platform_atomic_add_u64( &server_transport->next_packet_id, 1 );
        context->sequence = sequence;
        context->client_index = client_index;

        return frame;
    }

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        uint64_t packet_id;
        frame = rocketnet_server_driver_start_packet( server_transport, &to, &packet_id );
        if ( !frame )
        {
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return NULL;
        }
        rocketnet_platform_atomic_add_u64( &server_transport->send_staged, 1 );
        context->packet_id = packet_id;
    }
    else
#endif // #if ROCKETNET_SERVER_HAS_STAGED
    {
        struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

        uint64_t slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
        if ( slot >= ROCKETNET_SERVER_PAYLOAD_RING_SIZE )
        {
            rocketnet_spam( "server payload ring full. packet dropped" );
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return NULL;
        }

        ring->info[slot].to = to;
        ring->info[slot].client_index = client_index;
        ring->info[slot].wire_bytes = 0;

        frame = ring->data + slot * ROCKETNET_FRAME_STRIDE;

        // the packet id is the join key the acked batch surfaces: unique and
        // monotonic, never a ring slot. the slot is recovered from the frame
        // pointer wherever it is needed.

        context->packet_id = rocketnet_platform_atomic_add_u64( &server_transport->next_packet_id, 1 );
    }

    rocketnet_assert( ( (uintptr_t) frame % 16 ) == 0 );

    context->frame = frame;
    context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET;
    context->payload_capacity = ROCKETNET_MAX_PAYLOAD_BYTES;
    context->sequence = sequence;
    context->client_index = client_index;

    return frame;
}

void rocketnet_server_transport_abort_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return;

    if ( rocketnet_send_arena_owns( server_transport->send_arena, context->frame ) )
    {
        // an arena slot needs no undo: the flush reclaims the whole arena
        context->frame = NULL;
        return;
    }

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_driver_abort_packet( server_transport, context->packet_id, context->frame );
    }
    else
#endif // #if ROCKETNET_SERVER_HAS_STAGED
    {
        const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
        server_transport->payload_ring.info[slot].wire_bytes = 0;
    }

    context->frame = NULL;
}

// The fill-phase wire frame behind the arena finish: one sealed packet's
// destination on any driver, reserved before sealing so a split goes
// out whole or not at all. On the portable driver the frame is a payload
// ring slot; on the staged drivers it is a driver frame with its own packet
// id for the driver's bookkeeping -- the application's join key stays the
// context's.

struct rocketnet_server_fill_frame_t
{
    uint8_t * data;
    uint64_t socket_packet_id;      // the staged drivers' bookkeeping id
    uint64_t slot;                  // the portable payload ring slot
};

static int rocketnet_server_reserve_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                                const rocketnet_address_t * to, int client_index,
                                                struct rocketnet_server_fill_frame_t * frame )
{
#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        (void) client_index;
        frame->data = rocketnet_server_driver_start_packet( server_transport, to, &frame->socket_packet_id );
        if ( !frame->data )
        {
            rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
            return 0;
        }
        rocketnet_platform_atomic_add_u64( &server_transport->send_staged, 1 );
        return 1;
    }
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    frame->slot = rocketnet_platform_atomic_add_u64( &ring->num_packets, 1 );
    if ( frame->slot >= ROCKETNET_SERVER_PAYLOAD_RING_SIZE )
    {
        rocketnet_spam( "server payload ring full. packet dropped" );
        rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        frame->data = NULL;
        return 0;
    }

    ring->info[frame->slot].to = *to;
    ring->info[frame->slot].client_index = client_index;
    ring->info[frame->slot].wire_bytes = 0;
    frame->data = ring->data + frame->slot * ROCKETNET_FRAME_STRIDE;
    return 1;
}

static void rocketnet_server_commit_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                                struct rocketnet_server_fill_frame_t * frame, int wire_bytes )
{
#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_driver_finish_packet( server_transport, frame->socket_packet_id, frame->data, wire_bytes );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_packets_sent, 1 );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_bytes_sent, (uint64_t) ( wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES ) );
        return;
    }
#endif // #if ROCKETNET_SERVER_HAS_STAGED
    server_transport->payload_ring.info[frame->slot].wire_bytes = wire_bytes;
}

static void rocketnet_server_abort_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                               struct rocketnet_server_fill_frame_t * frame )
{
    if ( !frame->data )
        return;
#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_driver_abort_packet( server_transport, frame->socket_packet_id, frame->data );
        frame->data = NULL;
        return;
    }
#endif // #if ROCKETNET_SERVER_HAS_STAGED
    server_transport->payload_ring.info[frame->slot].wire_bytes = 0;
    frame->data = NULL;
}

// The arena finish: the request was over the wire's capacity, so the
// payload sits contiguous in an arena slot. A write that turned out to fit
// one packet collapses to a single ordinary payload -- one copy into one
// wire frame, cheaper than fragmenting -- and a genuinely large one splits
// into type 9 fragments. Every wire frame is reserved before anything
// seals, so a reservation shortfall drops the payload whole; a seal
// refusal mid-split -- possible only through a fill-contract violation,
// since connections are stable during a fill -- withdraws every reserved
// frame on the portable and gso drivers, whose staging is untouched until
// the main-thread flush, while frames already committed to the xdp socket
// stay committed there (its worker drains staging asynchronously), a
// partial payload the semantics already price as fragments lost on the
// wire. Returns the total wire bytes staged.

static int rocketnet_server_transport_finish_arena_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    rocketnet_address_t to;
    if ( !rocketnet_server_protocol_client_address( &server_transport->protocol, context->client_index, &to ) )
    {
        context->frame = NULL;
        return 0;
    }

    if ( payload_bytes <= ROCKETNET_MAX_PAYLOAD_BYTES )
    {
        struct rocketnet_server_fill_frame_t frame;
        if ( !rocketnet_server_reserve_fill_frame( server_transport, &to, context->client_index, &frame ) )
        {
            context->frame = NULL;
            return 0;
        }

        memcpy( frame.data + ROCKETNET_FRAME_PAYLOAD_OFFSET, context->frame, (size_t) payload_bytes );

        int wire_bytes = rocketnet_server_protocol_seal_payload_frame( &server_transport->protocol, context->client_index,
                                                                       context->sequence, context->packet_id,
                                                                       frame.data + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                       payload_bytes, server_transport->time );
        if ( wire_bytes == 0 )
        {
            rocketnet_server_abort_fill_frame( server_transport, &frame );
            context->frame = NULL;
            return 0;
        }

        rocketnet_server_commit_fill_frame( server_transport, &frame, wire_bytes );
        context->frame = NULL;
        return wire_bytes;
    }

    int fragment_count, fragment_stride;
    if ( !rocketnet_fragment_plan( payload_bytes, &fragment_count, &fragment_stride ) )
    {
        context->frame = NULL;
        return 0;
    }

    struct rocketnet_server_fill_frame_t frames[ROCKETNET_MAX_FRAGMENTS];
    for ( int i = 0; i < fragment_count; i++ )
    {
        if ( !rocketnet_server_reserve_fill_frame( server_transport, &to, context->client_index, &frames[i] ) )
        {
            // all or nothing: a partial payload is a dead payload, so the
            // frames already reserved go back
            rocketnet_spam( "server dropped fragmented packet. could not reserve every wire frame" );
            for ( int j = 0; j < i; j++ )
                rocketnet_server_abort_fill_frame( server_transport, &frames[j] );
            context->frame = NULL;
            return 0;
        }
    }

    // fragment 0 rides the sequence start_packet burned; the rest reserve
    // a block. the payload's one packet id lives in the aggregate, and
    // surfaces when every fragment's wire packet is acknowledged.

    uint64_t base_sequence = 0;
    if ( !rocketnet_server_protocol_start_fragments( &server_transport->protocol, context->client_index, fragment_count - 1, &base_sequence ) )
    {
        for ( int i = 0; i < fragment_count; i++ )
            rocketnet_server_abort_fill_frame( server_transport, &frames[i] );
        context->frame = NULL;
        return 0;
    }

    uint32_t payload_id = rocketnet_server_protocol_next_fragment_payload_id( &server_transport->protocol, context->client_index );
    uint64_t aggregate = rocketnet_ack_endpoint_open_aggregate( &server_transport->protocol.acks->endpoints[context->client_index],
                                                                context->packet_id, fragment_count );

    int total_wire_bytes = 0;
    for ( int i = 0; i < fragment_count; i++ )
    {
        const uint64_t sequence = i == 0 ? context->sequence : base_sequence + (uint64_t) ( i - 1 );
        const int fragment_bytes = i < fragment_count - 1 ? fragment_stride : payload_bytes - fragment_stride * ( fragment_count - 1 );
        int wire_bytes = rocketnet_server_protocol_seal_fragment( &server_transport->protocol, context->client_index,
                                                                  sequence, payload_id, i, fragment_count,
                                                                  context->frame + i * fragment_stride, fragment_bytes,
                                                                  aggregate, server_transport->time,
                                                                  frames[i].data + ROCKETNET_FRAME_WIRE_OFFSET );
        if ( wire_bytes == 0 )
        {
            // the xdp socket's staging drains asynchronously, so frames
            // already committed to it stay committed (fragments lost on
            // the wire, priced by the semantics); the portable and gso
            // stagings are untouched until the main-thread flush, so the
            // whole reservation withdraws

            const int first_withdrawn =
#if ROCKETNET_SERVER_HAS_STAGED
                server_transport->xdp_socket ? i :
#endif // #if ROCKETNET_SERVER_HAS_STAGED
                0;
            for ( int j = first_withdrawn; j < fragment_count; j++ )
                rocketnet_server_abort_fill_frame( server_transport, &frames[j] );
            context->frame = NULL;
            return 0;
        }
        rocketnet_server_commit_fill_frame( server_transport, &frames[i], wire_bytes );
        total_wire_bytes += wire_bytes;
    }

    context->frame = NULL;
    return total_wire_bytes;
}

int rocketnet_server_transport_finish_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( context );

    if ( !context->frame )
        return 0;

    if ( payload_bytes < 1 || payload_bytes > context->payload_capacity )
    {
        rocketnet_server_transport_abort_packet( server_transport, context );
        return 0;
    }

    if ( rocketnet_send_arena_owns( server_transport->send_arena, context->frame ) )
        return rocketnet_server_transport_finish_arena_packet( server_transport, context, payload_bytes );

    int wire_bytes = rocketnet_server_protocol_seal_payload_frame( &server_transport->protocol, context->client_index,
                                                                   context->sequence, context->packet_id,
                                                                   context->frame + ROCKETNET_FRAME_WIRE_OFFSET,
                                                                   payload_bytes, server_transport->time );
    if ( wire_bytes == 0 )
    {
        rocketnet_server_transport_abort_packet( server_transport, context );
        return 0;
    }

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        rocketnet_server_driver_finish_packet( server_transport, context->packet_id, context->frame, wire_bytes );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_packets_sent, 1 );
        rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_bytes_sent, (uint64_t) ( wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES ) );
    }
    else
#endif // #if ROCKETNET_SERVER_HAS_STAGED
    {
        const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
        server_transport->payload_ring.info[slot].wire_bytes = wire_bytes;
    }

    context->frame = NULL;

    return wire_bytes;
}

void rocketnet_server_transport_send_packets( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );

    const int stats_index = (int) ( server_transport->frame % ROCKETNET_NUM_SERVER_STATS_FRAMES );

#if ROCKETNET_SERVER_HAS_STAGED
    if ( rocketnet_server_transport_staged( server_transport ) )
    {
        // fold the fill phase's payload sends into the current bucket. the fill
        // is closed before this call by the threading contract, so the pending
        // pair is quiescent and a plain read-and-clear is safe.

        server_transport->stats_frames[stats_index].packets_sent += server_transport->stats_pending_packets_sent;
        server_transport->stats_frames[stats_index].bytes_sent += server_transport->stats_pending_bytes_sent;
        server_transport->stats_pending_packets_sent = 0;
        server_transport->stats_pending_bytes_sent = 0;

        // flip only when something staged since the last flip: the xdp flip is
        // destructive to run twice on one staging generation; the gso flush is
        // naturally idempotent but rides the same guard

        if ( server_transport->send_staged != server_transport->send_flushed )
        {
            server_transport->send_flushed = server_transport->send_staged;
            rocketnet_server_driver_send_packets( server_transport );
        }

        rocketnet_send_arena_reset( server_transport->send_arena );
        return;
    }
#endif // #if ROCKETNET_SERVER_HAS_STAGED

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    uint64_t num_packets = ring->num_packets;
    if ( num_packets > ROCKETNET_SERVER_PAYLOAD_RING_SIZE )
        num_packets = ROCKETNET_SERVER_PAYLOAD_RING_SIZE;

    struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
    int num_sends = 0;

    for ( uint64_t i = 0; i < num_packets; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            // the payload accumulation rides the flush walk: the ring already
            // holds every committed packet's wire size, so this costs nothing

            server_transport->stats_frames[stats_index].packets_sent++;
            server_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

            sends[num_sends].to = &ring->info[i].to;
            sends[num_sends].packet_data = ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET;
            sends[num_sends].packet_bytes = ring->info[i].wire_bytes;
            num_sends++;
        }
    }

    if ( num_sends > 0 )
        rocketnet_platform_socket_send_packets( server_transport->socket, sends, num_sends );

    ring->num_packets = 0;

    rocketnet_send_arena_reset( server_transport->send_arena );
}

// the convenience: the same path underneath, plus one memcpy the caller chose

void rocketnet_server_transport_send_packet( struct rocketnet_server_transport_t * server_transport,
                                             int client_index,
                                             const uint8_t * payload_data,
                                             int payload_bytes )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( payload_data );

    if ( payload_bytes < 1 || payload_bytes > server_transport->max_payload_bytes )
        return;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server_transport, client_index, payload_bytes, &context );
    if ( !frame )
        return;

    memcpy( frame + context.payload_index, payload_data, payload_bytes );

    rocketnet_server_transport_finish_packet( server_transport, &context, payload_bytes );
}

void rocketnet_server_transport_disconnect_client( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_assert( server_transport );

    if ( !rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index ) )
        return;

    // the address must be taken before the protocol call: the disconnect
    // releases the mapping that holds it

    int mapping_index = rocketnet_client_slots_mapping_index( server_transport->protocol.client_slots, client_index );
    rocketnet_address_t client_address = *rocketnet_encryption_map_address( server_transport->protocol.encryption_map, mapping_index );

    uint8_t burst[ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int burst_count = 0;
    int burst_bytes = 0;
    rocketnet_server_protocol_disconnect_client( &server_transport->protocol, client_index, burst, &burst_count, &burst_bytes );

    for ( int i = 0; i < burst_count; i++ )
    {
        struct rocketnet_server_frame_t frame;
        uint8_t * frame_data = rocketnet_server_reserve_send_frame( server_transport, &client_address, &frame );
        if ( !frame_data )
            break;
        memcpy( frame_data + ROCKETNET_FRAME_WIRE_OFFSET, burst + i * ROCKETNET_SEND_BUDGET, (size_t) burst_bytes );
        rocketnet_server_commit_send_frame( server_transport, &frame, burst_bytes );
    }
}

int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_num_connected( server_transport->protocol.client_slots );
}

int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_connected( server_transport->protocol.client_slots, client_index );
}

uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index )
{
    rocketnet_assert( server_transport );
    return rocketnet_client_slots_client_id( server_transport->protocol.client_slots, client_index );
}

const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return &server_transport->counters;
}

void rocketnet_server_transport_stats( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_transport_stats_t * stats )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( stats );
    memset( stats, 0, sizeof(struct rocketnet_server_transport_stats_t) );
    stats->packets_sent_per_second = server_transport->server_stats.packets_sent_per_second;
    stats->packets_received_per_second = server_transport->server_stats.packets_received_per_second;
    stats->bandwidth_sent_kbps = server_transport->server_stats.bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = server_transport->server_stats.bandwidth_received_kbps;
}

int rocketnet_server_transport_max_payload_bytes( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );
    return server_transport->max_payload_bytes;
}

int rocketnet_server_transport_acked_packets( struct rocketnet_server_transport_t * server_transport, const int ** client_index, const uint64_t ** packet_ids )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( client_index );
    rocketnet_assert( packet_ids );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    *client_index = acks->acked_client_index;
    *packet_ids = acks->acked_packet_ids;

    return acks->num_acked;
}

void rocketnet_server_transport_client_stats( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_server_transport_client_stats_t * stats )
{
    rocketnet_assert( server_transport );
    rocketnet_assert( stats );

    memset( stats, 0, sizeof(struct rocketnet_server_transport_client_stats_t) );

    struct rocketnet_server_acks_t * acks = server_transport->protocol.acks;

    if ( client_index < 0 || client_index >= acks->max_clients )
        return;

    const struct rocketnet_ack_endpoint_t * endpoint = &acks->endpoints[client_index];

    stats->rtt = endpoint->rtt;
    stats->jitter = endpoint->jitter;
    stats->packet_loss = endpoint->packet_loss;
    stats->bandwidth_sent_kbps = endpoint->bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = endpoint->bandwidth_received_kbps;
    stats->bandwidth_acked_kbps = endpoint->bandwidth_acked_kbps;
}
