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

#include <sodium.h>
#include <memory.h>
#include <stdlib.h>
#include <time.h>

// The transport driver is selected at compile time for now: the portable
// UDP socket by default, the AF_XDP server socket with
// -DROCKETNET_SERVER_XDP=1 on linux. Runtime selection with
// preference ordering is a named follow-on.

#if !defined( ROCKETNET_SERVER_XDP )
#define ROCKETNET_SERVER_XDP 0
#endif // #if !defined( ROCKETNET_SERVER_XDP )

#if ROCKETNET_SERVER_XDP
#include "rocketnet_server_socket.h"
#endif // #if ROCKETNET_SERVER_XDP

#define ROCKETNET_SERVER_MAX_RECEIVE_PACKETS 256
#define ROCKETNET_SERVER_MAX_SEND_PACKETS    256

struct rocketnet_server_send_buffer_t
{
    int num_packets;
    rocketnet_address_t to[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    int packet_bytes[ROCKETNET_SERVER_MAX_SEND_PACKETS];
    uint8_t data[ROCKETNET_FRAME_SIZE*ROCKETNET_SERVER_MAX_SEND_PACKETS];
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

#if ROCKETNET_SERVER_XDP
    struct rocketnet_server_socket_t * xdp_socket;
#else // #if ROCKETNET_SERVER_XDP
    rocketnet_platform_socket_t * socket;
#endif // #if ROCKETNET_SERVER_XDP

    struct rocketnet_counters_t counters;
    struct rocketnet_server_protocol_t protocol;

#if !ROCKETNET_SERVER_XDP
    struct rocketnet_server_send_buffer_t send_buffer;
    struct rocketnet_server_payload_ring_t payload_ring;
#endif // #if !ROCKETNET_SERVER_XDP

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

#if ROCKETNET_SERVER_XDP
    volatile uint64_t send_staged;                      // bumped by every producer; fills use the atomic add
    uint64_t send_flushed;                              // main thread only

    // payload sends on the xdp driver seal straight into the socket's own
    // staging from any game thread, so there is no transport-side ring for
    // the flush walk to read; each finish adds its packet here instead --
    // the same relaxed atomic the path already pays for send_staged -- and
    // send_packets folds the pair into the current bucket on the main thread.
    volatile uint64_t stats_pending_packets_sent;
    volatile uint64_t stats_pending_bytes_sent;
#endif // #if ROCKETNET_SERVER_XDP

    struct rocketnet_server_payloads_t payloads;
    // +8: serialize's BitReader loads 8-byte windows at byte granularity, so
    // any buffer a ReadStream is bound over must extend at least 8 bytes past
    // the end of its data -- this is the last member, so the slack is real.
    uint8_t payload_storage[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES*ROCKETNET_MAX_PAYLOADS_PER_UPDATE + 8];
};

// The send frame seam both drivers serve: reserve hands back a frame to
// seal complete wire bytes into, commit stamps its length, abort returns it
// unused. The portable driver batches frames and flushes them through the
// socket at the end of update; the xdp driver seals straight into the
// af_xdp send staging, zero copies between the seal and the wire.

struct rocketnet_server_frame_t
{
    uint8_t * data;
#if ROCKETNET_SERVER_XDP
    uint64_t packet_id;
#endif // #if ROCKETNET_SERVER_XDP
};

static uint8_t * rocketnet_server_reserve_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                                const rocketnet_address_t * to,
                                                                struct rocketnet_server_frame_t * frame )
{
#if ROCKETNET_SERVER_XDP

    frame->data = rocketnet_server_socket_start_packet( server_transport->xdp_socket, to, &frame->packet_id );
    if ( !frame->data )
    {
        rocketnet_spam( "server transport send staging full. packet dropped" );
    }
    return frame->data;

#else // #if ROCKETNET_SERVER_XDP

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

#endif // #if ROCKETNET_SERVER_XDP
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

#if ROCKETNET_SERVER_XDP

    rocketnet_server_socket_finish_packet( server_transport->xdp_socket, frame->packet_id, frame->data, packet_bytes );

#else // #if ROCKETNET_SERVER_XDP

    struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;
    send_buffer->packet_bytes[send_buffer->num_packets] = packet_bytes;
    send_buffer->num_packets++;

#endif // #if ROCKETNET_SERVER_XDP
}

static void rocketnet_server_abort_send_frame( struct rocketnet_server_transport_t * server_transport,
                                                         struct rocketnet_server_frame_t * frame )
{
    if ( !frame->data )
        return;

#if ROCKETNET_SERVER_XDP
    rocketnet_server_socket_abort_packet( server_transport->xdp_socket, frame->packet_id, frame->data );
#else // #if ROCKETNET_SERVER_XDP
    (void) server_transport;        // the reserved slot is reused by the next reserve; nothing to undo
#endif // #if ROCKETNET_SERVER_XDP

    frame->data = NULL;
}

void rocketnet_server_transport_default_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_assert( config );
    memset( config, 0, sizeof(struct rocketnet_server_transport_config_t) );
    config->max_clients = 64;
    const uint8_t test_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( config->filter_value, test_filter, ROCKETNET_FILTER_VALUE_BYTES );
}

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

struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context,
                                                                         const char * public_address_string,
                                                                         const struct rocketnet_server_transport_config_t * config,
                                                                         double time )
{
    rocketnet_assert( public_address_string );
    rocketnet_assert( config );

    (void) time;

    const int max_clients = config->max_clients;

    if ( max_clients <= 0 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        rocketnet_error( "server config max_clients %d is out of range (1 to %d)", max_clients, ROCKETNET_MAX_CLIENTS );
        return NULL;
    }

    // a server with no keys is refused, never defaulted: default keys are how
    // every deployment of a protocol ends up sharing one identity. development
    // wants rocketnet_server_transport_dev_config, which fills the published keys loudly.

    if ( !rocketnet_server_config_has_keys( config ) )
    {
        rocketnet_error( "server config has no keys: fill rocketnet_server_transport_dev_config() for development, or supply production keys" );
        return NULL;
    }

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

    // the fragmentation shape: zero means the defaults, anything else is
    // validated -- a maximum below the wire capacity or past the
    // 256-fragment ceiling is a configuration error, refused loudly

    server_transport->max_payload_bytes = config->max_payload_bytes > 0 ? config->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int num_reassembly_slots = config->num_reassembly_slots > 0 ? config->num_reassembly_slots : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;

    if ( server_transport->max_payload_bytes < ROCKETNET_MAX_PAYLOAD_BYTES ||
         server_transport->max_payload_bytes > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES )
    {
        rocketnet_error( "server config max_payload_bytes %d is out of range (%d to %d)", server_transport->max_payload_bytes, ROCKETNET_MAX_PAYLOAD_BYTES, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES );
        rocketnet_free( context, server_transport );
        return NULL;
    }

    if ( num_reassembly_slots > ROCKETNET_MAX_FRAGMENTS )
    {
        rocketnet_error( "server config num_reassembly_slots %d is out of range (1 to %d)", num_reassembly_slots, ROCKETNET_MAX_FRAGMENTS );
        rocketnet_free( context, server_transport );
        return NULL;
    }

    uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES];
    memcpy( tag, config->filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    const int development_keys = memcmp( config->server_public_key, rocketnet_dev_server_public_key, 32 ) == 0;
    (void) development_keys;

#if ROCKETNET_SERVER_XDP

    const int num_xdp_queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : 7;
    const int num_os_cpus = config->num_os_cpus > 0 ? config->num_os_cpus : 1;

    server_transport->xdp_socket = rocketnet_server_socket_create( context, public_address_string, tag, num_xdp_queues, num_os_cpus );
    if ( server_transport->xdp_socket == NULL )
    {
        rocketnet_error( "could not create xdp server socket" );
        rocketnet_server_transport_destroy( server_transport );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server transport started on %s [xdp] (%s keys)", rocketnet_address_to_string( &public_address, address_string ), development_keys ? "development" : "production" );

#else // #if ROCKETNET_SERVER_XDP

    server_transport->socket = rocketnet_platform_socket_create( context, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    if ( server_transport->socket == NULL )
    {
        rocketnet_error( "could not create server socket" );
        rocketnet_server_transport_destroy( server_transport );
        return NULL;
    }

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server transport started on %s (%s keys)", rocketnet_address_to_string( &bind_address, address_string ), development_keys ? "development" : "production" );

#endif // #if ROCKETNET_SERVER_XDP

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

    server_transport->next_packet_id = 1;

#if ROCKETNET_SERVER_XDP
    // the xdp socket assigns its own packet ids to ordinary sends from a
    // separate monotonic counter, so the transport's ids for fragmented
    // payloads start in the high half of the space -- the join keys can
    // never collide
    server_transport->next_packet_id = 1ULL << 63;
#endif // #if ROCKETNET_SERVER_XDP

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

#if ROCKETNET_SERVER_XDP

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

#else // #if ROCKETNET_SERVER_XDP

    if ( server_transport->socket )
        rocketnet_platform_socket_destroy( server_transport->socket );

#endif // #if ROCKETNET_SERVER_XDP

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
#if ROCKETNET_SERVER_XDP
        rocketnet_server_socket_stop( server_transport->xdp_socket );
        rocketnet_server_socket_update( server_transport->xdp_socket );
#endif // #if ROCKETNET_SERVER_XDP
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

#if ROCKETNET_SERVER_XDP

    rocketnet_server_socket_receive_packets( server_transport->xdp_socket );

    struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_socket_process_packets( server_transport->xdp_socket );

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

#else // #if ROCKETNET_SERVER_XDP

    uint8_t receive_frame[ROCKETNET_RECEIVE_CEILING];

    const int stats_index = (int) ( server_transport->frame % ROCKETNET_NUM_SERVER_STATS_FRAMES );

    for ( int i = 0; i < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS; i++ )
    {
        struct rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( server_transport->socket, &from, receive_frame, sizeof(receive_frame) );
        if ( packet_bytes == 0 )
            break;

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

#endif // #if ROCKETNET_SERVER_XDP

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

    // flush. the portable response ring sends inline here; the XDP socket is
    // flushed by rocketnet_server_transport_send_packets -- its buffer flip retargets
    // the drain and zeroes the buffers behind it, so flipping with nothing
    // staged would destroy in-flight packets. update marks itself staged
    // (responses and keep-alives ride the same buffers) and the flush below
    // flips only when something has staged since the last flip, which makes
    // a redundant send_packets call a no-op instead of the failure this
    // machinery produced live.

#if ROCKETNET_SERVER_XDP

    server_transport->send_staged++;

#else // #if ROCKETNET_SERVER_XDP

    for ( int i = 0; i < server_transport->send_buffer.num_packets; i++ )
    {
        if ( server_transport->send_buffer.packet_bytes[i] > 0 )
        {
            rocketnet_platform_socket_send_packet( server_transport->socket,
                                                   &server_transport->send_buffer.to[i],
                                                   server_transport->send_buffer.data + i * ROCKETNET_FRAME_SIZE + ROCKETNET_FRAME_WIRE_OFFSET,
                                                   server_transport->send_buffer.packet_bytes[i] );
        }
    }
    server_transport->send_buffer.num_packets = 0;

#endif // #if ROCKETNET_SERVER_XDP
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

#if ROCKETNET_SERVER_XDP

    uint64_t packet_id;
    frame = rocketnet_server_socket_start_packet( server_transport->xdp_socket, &to, &packet_id );
    if ( !frame )
    {
        rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return NULL;
    }
    rocketnet_platform_atomic_add_u64( &server_transport->send_staged, 1 );
    context->packet_id = packet_id;

#else // #if ROCKETNET_SERVER_XDP

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

#endif // #if ROCKETNET_SERVER_XDP

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

#if ROCKETNET_SERVER_XDP
    rocketnet_server_socket_abort_packet( server_transport->xdp_socket, context->packet_id, context->frame );
#else // #if ROCKETNET_SERVER_XDP
    const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
    server_transport->payload_ring.info[slot].wire_bytes = 0;
#endif // #if ROCKETNET_SERVER_XDP

    context->frame = NULL;
}

// The fill-phase wire frame behind the arena finish: one sealed packet's
// destination on either driver, reserved before sealing so a split goes
// out whole or not at all. On the portable driver the frame is a payload
// ring slot; on xdp it is a socket frame with its own packet id for the
// socket's bookkeeping -- the application's join key stays the context's.

struct rocketnet_server_fill_frame_t
{
    uint8_t * data;
#if ROCKETNET_SERVER_XDP
    uint64_t socket_packet_id;
#else // #if ROCKETNET_SERVER_XDP
    uint64_t slot;
#endif // #if ROCKETNET_SERVER_XDP
};

static int rocketnet_server_reserve_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                                const rocketnet_address_t * to, int client_index,
                                                struct rocketnet_server_fill_frame_t * frame )
{
#if ROCKETNET_SERVER_XDP

    (void) client_index;
    frame->data = rocketnet_server_socket_start_packet( server_transport->xdp_socket, to, &frame->socket_packet_id );
    if ( !frame->data )
    {
        rocketnet_platform_atomic_add_u64( &server_transport->protocol.counters->values[ROCKETNET_COUNTER_SEND_RING_FULL], 1 );
        return 0;
    }
    rocketnet_platform_atomic_add_u64( &server_transport->send_staged, 1 );
    return 1;

#else // #if ROCKETNET_SERVER_XDP

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

#endif // #if ROCKETNET_SERVER_XDP
}

static void rocketnet_server_commit_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                                struct rocketnet_server_fill_frame_t * frame, int wire_bytes )
{
#if ROCKETNET_SERVER_XDP
    rocketnet_server_socket_finish_packet( server_transport->xdp_socket, frame->socket_packet_id, frame->data, wire_bytes );
    rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_packets_sent, 1 );
    rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_bytes_sent, (uint64_t) ( wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES ) );
#else // #if ROCKETNET_SERVER_XDP
    server_transport->payload_ring.info[frame->slot].wire_bytes = wire_bytes;
#endif // #if ROCKETNET_SERVER_XDP
}

static void rocketnet_server_abort_fill_frame( struct rocketnet_server_transport_t * server_transport,
                                               struct rocketnet_server_fill_frame_t * frame )
{
    if ( !frame->data )
        return;
#if ROCKETNET_SERVER_XDP
    rocketnet_server_socket_abort_packet( server_transport->xdp_socket, frame->socket_packet_id, frame->data );
#else // #if ROCKETNET_SERVER_XDP
    server_transport->payload_ring.info[frame->slot].wire_bytes = 0;
#endif // #if ROCKETNET_SERVER_XDP
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
// frame on the portable driver, while frames already committed to the xdp
// socket stay committed there, a partial payload the semantics already
// price as fragments lost on the wire. Returns the total wire bytes
// staged.

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
#if ROCKETNET_SERVER_XDP
            for ( int j = i; j < fragment_count; j++ )
                rocketnet_server_abort_fill_frame( server_transport, &frames[j] );
#else // #if ROCKETNET_SERVER_XDP
            for ( int j = 0; j < fragment_count; j++ )
                rocketnet_server_abort_fill_frame( server_transport, &frames[j] );
#endif // #if ROCKETNET_SERVER_XDP
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

#if ROCKETNET_SERVER_XDP
    rocketnet_server_socket_finish_packet( server_transport->xdp_socket, context->packet_id, context->frame, wire_bytes );
    rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_packets_sent, 1 );
    rocketnet_platform_atomic_add_u64( &server_transport->stats_pending_bytes_sent, (uint64_t) ( wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES ) );
#else // #if ROCKETNET_SERVER_XDP
    const uint64_t slot = (uint64_t) ( context->frame - server_transport->payload_ring.data ) / ROCKETNET_FRAME_STRIDE;
    server_transport->payload_ring.info[slot].wire_bytes = wire_bytes;
#endif // #if ROCKETNET_SERVER_XDP

    context->frame = NULL;

    return wire_bytes;
}

void rocketnet_server_transport_send_packets( struct rocketnet_server_transport_t * server_transport )
{
    rocketnet_assert( server_transport );

    const int stats_index = (int) ( server_transport->frame % ROCKETNET_NUM_SERVER_STATS_FRAMES );

#if ROCKETNET_SERVER_XDP

    // fold the fill phase's payload sends into the current bucket. the fill
    // is closed before this call by the threading contract, so the pending
    // pair is quiescent and a plain read-and-clear is safe.

    server_transport->stats_frames[stats_index].packets_sent += server_transport->stats_pending_packets_sent;
    server_transport->stats_frames[stats_index].bytes_sent += server_transport->stats_pending_bytes_sent;
    server_transport->stats_pending_packets_sent = 0;
    server_transport->stats_pending_bytes_sent = 0;

    // flip only when something staged since the last flip: the flip is
    // destructive to run twice on one staging generation

    if ( server_transport->send_staged != server_transport->send_flushed )
    {
        server_transport->send_flushed = server_transport->send_staged;
        rocketnet_server_socket_send_packets( server_transport->xdp_socket );
    }

#else // #if ROCKETNET_SERVER_XDP

    struct rocketnet_server_payload_ring_t * ring = &server_transport->payload_ring;

    uint64_t num_packets = ring->num_packets;
    if ( num_packets > ROCKETNET_SERVER_PAYLOAD_RING_SIZE )
        num_packets = ROCKETNET_SERVER_PAYLOAD_RING_SIZE;

    for ( uint64_t i = 0; i < num_packets; i++ )
    {
        if ( ring->info[i].wire_bytes > 0 )
        {
            // the payload accumulation rides the flush walk: the ring already
            // holds every committed packet's wire size, so this costs nothing

            server_transport->stats_frames[stats_index].packets_sent++;
            server_transport->stats_frames[stats_index].bytes_sent += (uint64_t) ( ring->info[i].wire_bytes + ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

            rocketnet_platform_socket_send_packet( server_transport->socket, &ring->info[i].to,
                                                   ring->data + i * ROCKETNET_FRAME_STRIDE + ROCKETNET_FRAME_WIRE_OFFSET,
                                                   ring->info[i].wire_bytes );
        }
    }

    ring->num_packets = 0;

#endif // #if ROCKETNET_SERVER_XDP

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
