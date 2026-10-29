/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A real message server and a real message client, joined by the in-process
// loopback transport, with the fuzz harness standing on the wire between them.
// The receive loops under test are the production ones:
// rocketnet_server_transport_update drains the server's loopback endpoint
// through its receive_batch, and the step-driven client transport drains its
// own endpoint in rocketnet_client_transport_update. No socket, no thread, no
// clock: time is a variable the harness advances, and both ends read the
// token clock through game_clock_epoch.
//
// THE WIRE SEAM. This header compiles sdk/rocketnet_server_transport.cpp into
// the harness, as test_transport_control.cpp and test_unit_driver_loopback.cpp
// do, so it can stand two shims in the server's io operations -- the same seam
// test_server_connects_after_junk_from_many_sources swaps its receive_batch into:
//
//   receive_batch: the server receives exactly the datagrams the harness put
//     on its deliver list for this update, each with the source address the
//     harness chose (the client's real address, or a stranger's);
//   send_batch: what the server sends is captured, and the harness decides
//     what reaches the client, through the real loopback send_batch, from the
//     server's real address.
//
// Between the two the harness can drop, duplicate, hold back (reorder),
// corrupt and inject, in both directions, and the library code is unchanged.
//
// PAST THE AEAD. The harness reads the live connection's keys from the
// server's encryption map, so it can seal fuzz-chosen plaintext as any packet
// type with the real keys at a fresh sequence: post-decrypt parsing in both
// receive loops is reachable by mutation, which raw bytes never reach through
// a MAC.
//
// DETERMINISM. libsodium's randombytes is replaced by a splitmix64 stream that
// is reset to the same seed at every pair create, so every input sees the
// same salts, nonces, ephemeral keys and table hash keys, and therefore the
// same handshake bytes: a datagram the seed generator recorded from a
// connected pair opens again under the harness's connected pair. Time is a
// fixed schedule from 1000.0. Loopback ports advance across inputs; no
// decision depends on a port's value.
//
// ALLOCATE UP FRONT. rocketnet_init is given a counting allocator. Once the
// pair is created, the harness checks that nothing allocates again until
// destroy: rocketnet allocates every structure at create.

#pragma once

#ifndef ROCKETNET_TESTS_FUZZ_ROCKETNET_FUZZ_PAIR_H
#define ROCKETNET_TESTS_FUZZ_ROCKETNET_FUZZ_PAIR_H

#include "../../sdk/rocketnet_server_transport.cpp"
#include "../../sdk/rocketnet_driver_loopback.cpp"

#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_wire.h"
#include "rocketnet_read_write.h"
#include "../portable_crt.h"

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the page-tracked restore (see "save and restore") is Linux only
#if defined( __linux__ )
#define ROCKETNET_FUZZ_PAGE_RESTORE 1
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#if defined( __has_feature )
#if __has_feature( address_sanitizer )
#include <sanitizer/asan_interface.h>
#define ROCKETNET_FUZZ_POISON( p, n ) __asan_poison_memory_region( ( p ), ( n ) )
#endif // #if __has_feature( address_sanitizer )
#endif // #if defined( __has_feature )
#if !defined( ROCKETNET_FUZZ_POISON ) && defined( __SANITIZE_ADDRESS__ )
#include <sanitizer/asan_interface.h>
#define ROCKETNET_FUZZ_POISON( p, n ) __asan_poison_memory_region( ( p ), ( n ) )
#endif // #if !defined( ROCKETNET_FUZZ_POISON ) && defined( __SANITIZE_ADDRESS__ )
#if !defined( ROCKETNET_FUZZ_POISON )
#define ROCKETNET_FUZZ_POISON( p, n ) ( (void) ( p ), (void) ( n ) )
#endif // #if !defined( ROCKETNET_FUZZ_POISON )
#endif // #if defined( __linux__ )

static inline void rocketnet_fuzz_pair_require( bool condition, const char * property )
{
    if ( condition )
        return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

// -----------------------------------------------------------------------
//  the input cursor: reads past the end return zero
// -----------------------------------------------------------------------

struct rocketnet_fuzz_cursor_t
{
    const uint8_t * data;
    size_t size;
    size_t pos;
};

static inline uint8_t rocketnet_fuzz_u8( struct rocketnet_fuzz_cursor_t * c )
{
    return c->pos < c->size ? c->data[c->pos++] : 0;
}

static inline uint16_t rocketnet_fuzz_u16( struct rocketnet_fuzz_cursor_t * c )
{
    const uint16_t a = rocketnet_fuzz_u8( c );
    const uint16_t b = rocketnet_fuzz_u8( c );
    return (uint16_t) ( a | ( b << 8 ) );
}

static inline bool rocketnet_fuzz_more( const struct rocketnet_fuzz_cursor_t * c )
{
    return c->pos < c->size;
}

// -----------------------------------------------------------------------
//  deterministic randombytes
// -----------------------------------------------------------------------

#define ROCKETNET_FUZZ_RANDOM_SEED 0x726f636b65746e65ULL      // "rocketne"

static uint64_t rocketnet_fuzz_random_state = ROCKETNET_FUZZ_RANDOM_SEED;

static inline uint64_t rocketnet_fuzz_random_next()
{
    uint64_t z = ( rocketnet_fuzz_random_state += 0x9e3779b97f4a7c15ULL );
    z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
    z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
    return z ^ ( z >> 31 );
}

static inline const char * rocketnet_fuzz_random_name() { return "rocketnet_fuzz_splitmix64"; }
static inline uint32_t rocketnet_fuzz_random_u32() { return (uint32_t) rocketnet_fuzz_random_next(); }
static inline void rocketnet_fuzz_random_stir() {}

static inline uint32_t rocketnet_fuzz_random_uniform( const uint32_t upper_bound )
{
    return upper_bound < 2 ? 0 : (uint32_t) ( rocketnet_fuzz_random_next() % upper_bound );
}

static inline void rocketnet_fuzz_random_buf( void * const buf, const size_t size )
{
    uint8_t * out = (uint8_t *) buf;

    for ( size_t i = 0; i < size; i++ )
    {
        out[i] = (uint8_t) rocketnet_fuzz_random_next();
    }
}

static inline int rocketnet_fuzz_random_close() { return 0; }

static randombytes_implementation rocketnet_fuzz_random_implementation = {
    rocketnet_fuzz_random_name, rocketnet_fuzz_random_u32, rocketnet_fuzz_random_stir,
    rocketnet_fuzz_random_uniform, rocketnet_fuzz_random_buf, rocketnet_fuzz_random_close
};

// -----------------------------------------------------------------------
//  the counting allocator
// -----------------------------------------------------------------------

static uint64_t rocketnet_fuzz_allocations = 0;
static uint64_t rocketnet_fuzz_frees = 0;

// every live block the library holds, so a created pair can be saved
// and restored byte for byte
#define ROCKETNET_FUZZ_MAX_BLOCKS 4096

struct rocketnet_fuzz_block_t
{
    void * p;
    size_t bytes;
};

static struct rocketnet_fuzz_block_t rocketnet_fuzz_blocks[ROCKETNET_FUZZ_MAX_BLOCKS];
static int rocketnet_fuzz_num_blocks = 0;
static bool rocketnet_fuzz_states_saved = false;

#if defined( ROCKETNET_FUZZ_PAGE_RESTORE )
static size_t rocketnet_fuzz_page_bytes = 0;

// Every block owns whole pages, so the restore can write-protect a block's
// pages without touching anything else's. The slack past the block's end is
// poisoned, so AddressSanitizer still reports a write one byte past it.
static inline void * rocketnet_fuzz_block_alloc( size_t bytes )
{
    if ( rocketnet_fuzz_page_bytes == 0 )
    {
        const long page = sysconf( _SC_PAGESIZE );
        rocketnet_fuzz_pair_require( page > 0, "the page size is known" );
        rocketnet_fuzz_page_bytes = (size_t) page;
    }

    const size_t page_bytes = rocketnet_fuzz_page_bytes;
    const size_t rounded = bytes == 0 ? page_bytes : ( bytes + page_bytes - 1 ) / page_bytes * page_bytes;
    void * p = NULL;
    if ( posix_memalign( &p, page_bytes, rounded ) != 0 )
        return NULL;

    if ( rounded > bytes )
    {
        ROCKETNET_FUZZ_POISON( (uint8_t *) p + bytes, rounded - bytes );
    }

    return p;
}
#endif // #if defined( ROCKETNET_FUZZ_PAGE_RESTORE )

static inline void * rocketnet_fuzz_malloc( void *, size_t bytes )
{
    rocketnet_fuzz_allocations++;
#if defined( ROCKETNET_FUZZ_PAGE_RESTORE )
    void * p = rocketnet_fuzz_block_alloc( bytes );
#else
    void * p = malloc( bytes );
#endif // #if defined( ROCKETNET_FUZZ_PAGE_RESTORE )
    if ( p )
    {
        rocketnet_fuzz_pair_require( rocketnet_fuzz_num_blocks < ROCKETNET_FUZZ_MAX_BLOCKS, "the fuzz block table holds every live block" );

        struct rocketnet_fuzz_block_t * b = &rocketnet_fuzz_blocks[rocketnet_fuzz_num_blocks++];
        b->p = p;
        b->bytes = bytes;
    }

    return p;
}

static inline void rocketnet_fuzz_free( void *, void * p )
{
    if ( !p )
        return;
    rocketnet_fuzz_frees++;

    for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
    {
        if ( rocketnet_fuzz_blocks[i].p != p )
            continue;
        rocketnet_fuzz_pair_require( !rocketnet_fuzz_states_saved, "nothing frees a block the saved states hold" );
        rocketnet_fuzz_blocks[i] = rocketnet_fuzz_blocks[--rocketnet_fuzz_num_blocks];
        break;
    }

    free( p );
}

// Called from each target's LLVMFuzzerInitialize: the random stream first,
// so sodium_init (inside rocketnet_init) never touches the system source.
static inline int rocketnet_fuzz_pair_initialize()
{
    randombytes_set_implementation( &rocketnet_fuzz_random_implementation );
    if ( rocketnet_init( NULL, rocketnet_fuzz_malloc, rocketnet_fuzz_free ) != ROCKETNET_STATUS_OK )
        return 1;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_quiet( 1 );

    return 0;
}

// -----------------------------------------------------------------------
//  the one pair configuration, shared with the seed generator so a seed is
//  replayed under the setup it was recorded from
// -----------------------------------------------------------------------

#define ROCKETNET_FUZZ_SERVER_PORT        40000
#define ROCKETNET_FUZZ_SERVER_ADDRESS     "127.0.0.1:40000"
#define ROCKETNET_FUZZ_CLIENT_ID          0x66757a7a636c6e74ULL
#define ROCKETNET_FUZZ_START_TIME         1000.0
#define ROCKETNET_FUZZ_DT                 0.01
#define ROCKETNET_FUZZ_CONNECT_STEPS      200
#define ROCKETNET_FUZZ_NUM_CHANNELS       8
#define ROCKETNET_FUZZ_MAX_MESSAGE_BYTES  16384

// at time 1000.0 the token clock reads ROCKETNET_TEST_CREATE_TS
#define ROCKETNET_FUZZ_GAME_CLOCK_EPOCH   ( ROCKETNET_TEST_CREATE_TS - 1000 )

static inline void rocketnet_fuzz_message_config( struct rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );

    // eight channels, messages large enough to fragment, a reliable window
    // small enough to fill, and resends fast enough that loss is repaired
    // inside a short input

    config->num_channels = ROCKETNET_FUZZ_NUM_CHANNELS;
    config->reliable_messages_in_flight = 64;
    config->receive_message_buffer_bytes = 262144;
    config->max_message_bytes = ROCKETNET_FUZZ_MAX_MESSAGE_BYTES;
    config->send_message_buffer_bytes = 1048576;
    config->send_queue_messages = 256;
    config->max_message_receipts = 256;
    config->message_resend_delay_initial_ms = 20;
    config->message_resend_delay_min_ms = 10;
    config->message_resend_delay_max_ms = 40;
}

// -----------------------------------------------------------------------
//  the wire
// -----------------------------------------------------------------------

#define ROCKETNET_FUZZ_WIRE_PACKETS 256

struct rocketnet_fuzz_datagram_t
{
    struct rocketnet_address_t from;
    int bytes;
    uint8_t data[ROCKETNET_RECEIVE_CEILING];
};

struct rocketnet_fuzz_ring_t
{
    int count;
    struct rocketnet_fuzz_datagram_t packet[ROCKETNET_FUZZ_WIRE_PACKETS];
};

static inline bool rocketnet_fuzz_ring_push( struct rocketnet_fuzz_ring_t * ring, const struct rocketnet_address_t * from, const uint8_t * data, int bytes )
{
    if ( bytes <= 0 || bytes > ROCKETNET_RECEIVE_CEILING || ring->count >= ROCKETNET_FUZZ_WIRE_PACKETS )
        return false;

    struct rocketnet_fuzz_datagram_t * d = &ring->packet[ring->count++];
    d->from = *from;
    d->bytes = bytes;
    memcpy( d->data, data, (size_t) bytes );

    return true;
}

static inline void rocketnet_fuzz_ring_remove( struct rocketnet_fuzz_ring_t * ring, int index )
{
    if ( index < 0 || index >= ring->count )
        return;

    for ( int i = index; i + 1 < ring->count; i++ )
    {
        ring->packet[i] = ring->packet[i + 1];
    }

    ring->count--;
}

struct rocketnet_fuzz_pair_t
{
    double time;
    struct rocketnet_message_server_t * server;
    struct rocketnet_message_client_t * client;
    struct rocketnet_server_transport_t * transport;

    const struct rocketnet_server_transport_io_operations_t * real;
    struct rocketnet_server_transport_io_operations_t shim;

    struct rocketnet_address_t server_address;
    struct rocketnet_address_t client_address;
    struct rocketnet_address_t stranger_address;
    bool client_known;

    // the client's connection salt, read from its first request, for the
    // unsealed cookie the harness can write back to it
    uint8_t client_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    bool salt_known;

    struct rocketnet_fuzz_ring_t c2s;           // captured client -> server, not yet delivered
    struct rocketnet_fuzz_ring_t c2s_deliver;   // what the server receives at its next update
    struct rocketnet_fuzz_ring_t s2c;           // captured server -> client, not yet delivered

    uint64_t allocations_after_create;
    bool capture_s2c;                           // false: the server's sends go straight to the client

    struct rocketnet_loopback_hub_t * hub;      // the pair's loopback hub: one more block the saves copy
};

static struct rocketnet_fuzz_pair_t * rocketnet_fuzz_live_pair = NULL;

static inline int rocketnet_fuzz_shim_receive_batch( void * instance, struct rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    struct rocketnet_fuzz_pair_t * pair = rocketnet_fuzz_live_pair;

    (void) instance;

    int received = 0;
    int consumed = 0;
    for ( ; consumed < pair->c2s_deliver.count && received < count; consumed++ )
    {
        const struct rocketnet_fuzz_datagram_t * d = &pair->c2s_deliver.packet[consumed];
        if ( d->bytes > stride )
            continue;
        from[received] = d->from;
        bytes[received] = d->bytes;
        memcpy( data + (size_t) received * (size_t) stride, d->data, (size_t) d->bytes );
        received++;
    }

    // what did not fit this batch stays for the next call, in order

    for ( int i = 0; i + consumed < pair->c2s_deliver.count; i++ )
    {
        pair->c2s_deliver.packet[i] = pair->c2s_deliver.packet[i + consumed];
    }

    pair->c2s_deliver.count -= consumed;

    return received;
}

static inline void rocketnet_fuzz_shim_send_batch( void * instance, const rocketnet_platform_send_t * packets, int count )
{
    struct rocketnet_fuzz_pair_t * pair = rocketnet_fuzz_live_pair;
    if ( !pair->capture_s2c )
    {
        pair->real->send_batch( instance, packets, count );
        return;
    }

    for ( int i = 0; i < count; i++ )
    {
        rocketnet_fuzz_ring_push( &pair->s2c, packets[i].to, packets[i].packet_data, packets[i].packet_bytes );
    }
}

// pull everything the client sent off the server's real loopback endpoint
static inline void rocketnet_fuzz_pair_capture_c2s( struct rocketnet_fuzz_pair_t * pair )
{
    static struct rocketnet_address_t from[64];
    static uint8_t data[64 * ROCKETNET_RECEIVE_CEILING];
    static int bytes[64];

    for ( ;; )
    {
        const int n = pair->real->receive_batch( pair->transport->io.instance, from, data, ROCKETNET_RECEIVE_CEILING, bytes, 64 );
        if ( n <= 0 )
            break;

        for ( int i = 0; i < n; i++ )
        {
            const uint8_t * packet = data + (size_t) i * ROCKETNET_RECEIVE_CEILING;

            if ( !pair->client_known )
            {
                pair->client_address = from[i];
                pair->client_known = true;
            }

            if ( !pair->salt_known && bytes[i] == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES && packet[0] == ROCKETNET_CONNECTION_REQUEST_PACKET )
            {
                memcpy( pair->client_salt, packet + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, ROCKETNET_CONNECTION_SALT_BYTES );
                pair->salt_known = true;
            }

            rocketnet_fuzz_ring_push( &pair->c2s, &from[i], packet, bytes[i] );
        }
    }
}

// one captured c2s datagram onto the server's next receive
static inline void rocketnet_fuzz_pair_deliver_c2s( struct rocketnet_fuzz_pair_t * pair, int index )
{
    if ( index < 0 || index >= pair->c2s.count )
        return;

    const struct rocketnet_fuzz_datagram_t * d = &pair->c2s.packet[index];
    rocketnet_fuzz_ring_push( &pair->c2s_deliver, &d->from, d->data, d->bytes );
    rocketnet_fuzz_ring_remove( &pair->c2s, index );
}

// one captured s2c datagram to the client, from the server's real address
static inline void rocketnet_fuzz_pair_deliver_s2c( struct rocketnet_fuzz_pair_t * pair, int index )
{
    if ( index < 0 || index >= pair->s2c.count )
        return;

    const struct rocketnet_fuzz_datagram_t * d = &pair->s2c.packet[index];
    rocketnet_platform_send_t send;
    send.to = &d->from;                 // the ring keeps the destination in from
    send.packet_data = d->data;
    send.packet_bytes = d->bytes;
    pair->real->send_batch( pair->transport->io.instance, &send, 1 );
    rocketnet_fuzz_ring_remove( &pair->s2c, index );
}

static inline void rocketnet_fuzz_pair_deliver_all( struct rocketnet_fuzz_pair_t * pair )
{
    while ( pair->c2s.count > 0 )
    {
        rocketnet_fuzz_pair_deliver_c2s( pair, 0 );
    }

    while ( pair->s2c.count > 0 )
    {
        rocketnet_fuzz_pair_deliver_s2c( pair, 0 );
    }
}

// a datagram at the server's receive loop, from the client's address or a stranger's
static inline void rocketnet_fuzz_pair_inject_c2s( struct rocketnet_fuzz_pair_t * pair, bool from_client, const uint8_t * data, int bytes )
{
    const struct rocketnet_address_t * from = from_client && pair->client_known ? &pair->client_address : &pair->stranger_address;
    rocketnet_fuzz_ring_push( &pair->c2s_deliver, from, data, bytes );
}

// a datagram at the client's receive loop, from the server's real address
static inline void rocketnet_fuzz_pair_inject_s2c( struct rocketnet_fuzz_pair_t * pair, const uint8_t * data, int bytes )
{
    if ( !pair->client_known || bytes <= 0 || bytes > ROCKETNET_RECEIVE_CEILING )
        return;

    rocketnet_platform_send_t send;
    send.to = &pair->client_address;
    send.packet_data = data;
    send.packet_bytes = bytes;
    pair->real->send_batch( pair->transport->io.instance, &send, 1 );
}

// -----------------------------------------------------------------------
//  sealing with the live keys
// -----------------------------------------------------------------------

static inline struct rocketnet_encryption_mapping_t * rocketnet_fuzz_pair_mapping( struct rocketnet_fuzz_pair_t * pair )
{
    if ( !pair->client_known )
        return NULL;

    const int index = rocketnet_encryption_map_find( pair->transport->protocol.encryption_map, &pair->client_address );
    if ( index < 0 )
        return NULL;

    return rocketnet_encryption_map_entry( pair->transport->protocol.encryption_map, index );
}

static inline bool rocketnet_fuzz_session_type( uint8_t type )
{
    return type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET || type == ROCKETNET_CONNECTION_PAYLOAD_PACKET ||
           type == ROCKETNET_CONNECTION_DISCONNECT_PACKET || type == ROCKETNET_CONNECTION_ACK_PACKET ||
           type == ROCKETNET_CONNECTION_FRAGMENT_PACKET;
}

// Seal plaintext as a client -> server packet of the given type under the
// live mapping's keys, at the receiver's next unseen sequence plus skip.
// Returns the wire size, or 0 when there is no mapping or the type is not
// one the client seals.
static inline int rocketnet_fuzz_pair_seal_c2s( struct rocketnet_fuzz_pair_t * pair, uint8_t type, int skip, const uint8_t * plaintext, int plaintext_bytes, uint8_t * packet )
{
    struct rocketnet_encryption_mapping_t * m = rocketnet_fuzz_pair_mapping( pair );
    if ( !m || plaintext_bytes < 0 || plaintext_bytes > ROCKETNET_RECEIVE_CEILING - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_SESSION_NONCE_BYTES )
        return 0;

    const uint64_t protocol_id = pair->transport->protocol.protocol_id;
    const uint8_t * filter = pair->transport->protocol.packet_filter;

    if ( rocketnet_fuzz_session_type( type ) )
    {
        const uint64_t sequence = m->replay_session.highest_opened + 1 + (uint64_t) skip;
        return rocketnet_write_sealed_packet( packet, type, filter, sequence, NULL, plaintext, plaintext_bytes,
                                              protocol_id, m->connection_salt, NULL, m->keys.session_client_to_server );
    }

    if ( type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
    {
        const uint64_t sequence = m->handshake_highest_opened + 1 + (uint64_t) skip;
        return rocketnet_write_sealed_packet( packet, type, filter, sequence, NULL, plaintext, plaintext_bytes,
                                              protocol_id, m->connection_salt, m->client_ephemeral_public_key, m->keys.handshake_client_to_server );
    }

    return 0;
}

// Seal plaintext as a server -> client session packet of the given type, at
// the server's next send sequence plus skip; the server's own counter moves
// past it, exactly as if the server had sent it. The handshake types are not
// sealed here (tests/fuzz/README.md, "Exclusions").
static inline int rocketnet_fuzz_pair_seal_s2c( struct rocketnet_fuzz_pair_t * pair, uint8_t type, int skip, const uint8_t * plaintext, int plaintext_bytes, uint8_t * packet )
{
    struct rocketnet_encryption_mapping_t * m = rocketnet_fuzz_pair_mapping( pair );
    if ( !m || plaintext_bytes < 0 || plaintext_bytes > ROCKETNET_RECEIVE_CEILING - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_SESSION_NONCE_BYTES )
        return 0;

    const uint64_t protocol_id = pair->transport->protocol.protocol_id;
    const uint8_t * filter = pair->transport->protocol.packet_filter;

    if ( rocketnet_fuzz_session_type( type ) )
    {
        const uint64_t sequence = m->send_sequence_session + (uint64_t) skip;
        m->send_sequence_session = sequence + 1;
        return rocketnet_write_sealed_packet( packet, type, filter, sequence, NULL, plaintext, plaintext_bytes,
                                              protocol_id, m->connection_salt, NULL, m->keys.session_server_to_client );
    }

    return 0;
}

// -----------------------------------------------------------------------
//  create, step, connect, destroy
// -----------------------------------------------------------------------

static double rocketnet_fuzz_client_clock_time = ROCKETNET_FUZZ_START_TIME;
static inline double rocketnet_fuzz_client_clock( void * ) { return rocketnet_fuzz_client_clock_time; }

static inline bool rocketnet_fuzz_pair_create( struct rocketnet_fuzz_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    rocketnet_fuzz_random_state = ROCKETNET_FUZZ_RANDOM_SEED;
    pair->time = ROCKETNET_FUZZ_START_TIME;
    rocketnet_fuzz_client_clock_time = pair->time;

    struct rocketnet_message_config_t message_config;
    rocketnet_fuzz_message_config( &message_config );

    pair->hub = rocketnet_loopback_hub_create( NULL, 1, NULL );
    if ( !pair->hub )
        return false;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    server_config.max_clients = 1;
    server_config.game_clock_epoch = ROCKETNET_FUZZ_GAME_CLOCK_EPOCH;
    memcpy( server_config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( server_config.server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( server_config.server_secret_key, rocketnet_dev_server_secret_key, 32 );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = pair->hub;

    pair->server = rocketnet_message_server_create( NULL, ROCKETNET_FUZZ_SERVER_ADDRESS, &server_config, &message_config, NULL, 0, 0 );
    if ( !pair->server )
        return false;
    pair->transport = rocketnet_message_server_transport( pair->server );

    // the wire seam

    pair->real = pair->transport->io.operations;
    pair->shim = *pair->real;
    pair->shim.receive_batch = rocketnet_fuzz_shim_receive_batch;
    pair->shim.send_batch = rocketnet_fuzz_shim_send_batch;
    pair->transport->io.operations = &pair->shim;
    pair->capture_s2c = true;
    rocketnet_fuzz_live_pair = pair;

    rocketnet_address_parse( &pair->server_address, ROCKETNET_FUZZ_SERVER_ADDRESS );
    rocketnet_address_parse( &pair->stranger_address, "198.51.100.77:51234" );

    const char * addresses[] = { ROCKETNET_FUZZ_SERVER_ADDRESS };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    if ( rocketnet_write_production_connect_token( rocketnet_dev_backend_secret_key, rocketnet_dev_server_public_key,
                                                   ROCKETNET_DEV_PROTOCOL_ID, addresses, 1, ROCKETNET_FUZZ_CLIENT_ID,
                                                   ROCKETNET_TEST_CREATE_TS - 10, ROCKETNET_TEST_CREATE_TS + 3600, 10,
                                                   NULL, token, sizeof( token ) ) != ROCKETNET_STATUS_OK )
        return false;

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = pair->hub;
    client_config.step_driven = 1;
    client_config.time_callback = rocketnet_fuzz_client_clock;
    client_config.game_clock_epoch = ROCKETNET_FUZZ_GAME_CLOCK_EPOCH;

    pair->client = rocketnet_message_client_create( NULL, token, &client_config, &message_config, NULL, 0, 0 );
    if ( !pair->client )
        return false;

    pair->allocations_after_create = rocketnet_fuzz_allocations;

    return true;
}

static inline void rocketnet_fuzz_pair_check_no_allocation( const struct rocketnet_fuzz_pair_t * pair )
{
    rocketnet_fuzz_pair_require( rocketnet_fuzz_allocations == pair->allocations_after_create,
                                 "nothing allocates after create (rocketnet allocates up front)" );
}

// the server's half of a step: it receives what is on the deliver list
static inline void rocketnet_fuzz_pair_server_update( struct rocketnet_fuzz_pair_t * pair )
{
    rocketnet_message_server_update( pair->server, pair->time );
}

static inline void rocketnet_fuzz_pair_client_update( struct rocketnet_fuzz_pair_t * pair )
{
    rocketnet_fuzz_client_clock_time = pair->time;
    rocketnet_message_client_update( pair->client, pair->time );
}

static inline void rocketnet_fuzz_pair_send( struct rocketnet_fuzz_pair_t * pair )
{
    rocketnet_message_server_send_packets( pair->server );
    rocketnet_message_client_send_packets( pair->client );
    rocketnet_fuzz_pair_capture_c2s( pair );
}

static inline void rocketnet_fuzz_pair_drain_messages( struct rocketnet_fuzz_pair_t * pair )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    int channel = 0;
    int client_index = 0;
    while ( rocketnet_message_server_receive( pair->server, &client_index, &channel, &data, &bytes ) )
    {
        rocketnet_message_server_release( pair->server, data );
    }

    while ( rocketnet_message_client_receive( pair->client, &channel, &data, &bytes ) )
    {
        rocketnet_message_client_release( pair->client, data );
    }
}

// one clean step: time moves, both update, both send, the wire delivers all
static inline void rocketnet_fuzz_pair_clean_step( struct rocketnet_fuzz_pair_t * pair )
{
    pair->time += ROCKETNET_FUZZ_DT;
    rocketnet_fuzz_pair_server_update( pair );
    rocketnet_fuzz_pair_client_update( pair );
    rocketnet_fuzz_pair_drain_messages( pair );
    rocketnet_fuzz_pair_send( pair );
    rocketnet_fuzz_pair_deliver_all( pair );
}

static inline bool rocketnet_fuzz_pair_connected( struct rocketnet_fuzz_pair_t * pair )
{
    return rocketnet_message_client_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
           rocketnet_message_server_client_connected( pair->server, 0 );
}

static inline bool rocketnet_fuzz_pair_connect( struct rocketnet_fuzz_pair_t * pair )
{
    for ( int i = 0; i < ROCKETNET_FUZZ_CONNECT_STEPS && !rocketnet_fuzz_pair_connected( pair ); i++ )
    {
        rocketnet_fuzz_pair_clean_step( pair );
    }

    return rocketnet_fuzz_pair_connected( pair );
}

static inline void rocketnet_fuzz_pair_destroy( struct rocketnet_fuzz_pair_t * pair )
{
    if ( pair->client )
    {
        rocketnet_message_client_destroy( pair->client );
    }

    if ( pair->server )
    {
        pair->transport->io.operations = pair->real;
        rocketnet_message_server_destroy( pair->server );
    }

    rocketnet_fuzz_live_pair = NULL;
    pair->client = NULL;
    pair->server = NULL;

    rocketnet_fuzz_pair_require( rocketnet_loopback_hub_destroy( pair->hub ) == ROCKETNET_STATUS_OK, "the pair's hub destroys once its endpoints are gone" );
    pair->hub = NULL;
}

// -----------------------------------------------------------------------
//  save and restore
//
// A created pair holds over 100 MB (the transports allocate their full
// compile-time plans up front, and create zeroes them) and destroy wipes
// every byte with sodium_memzero, so a create and destroy per input runs a
// few inputs a second. Instead the pair is created once per process and its
// state is saved twice: just after create (the handshake not yet
// started) and just after connect. A saved state is every block the library
// allocated, the loopback hub among them, the harness's own pair state and
// the random stream. Each input restores one of the two, so input N
// starts from the same bytes whether it runs alone or after N-1, and nothing
// allocates or frees on the way.
// -----------------------------------------------------------------------

struct rocketnet_fuzz_saved_state_t
{
    struct rocketnet_fuzz_pair_t pair;
    uint64_t random_state;
    double client_clock_time;
};

#define ROCKETNET_FUZZ_STATE_CREATED   0
#define ROCKETNET_FUZZ_STATE_CONNECTED 1

static struct rocketnet_fuzz_saved_state_t rocketnet_fuzz_saved_states[2];
static void * rocketnet_fuzz_block_copies[2][ROCKETNET_FUZZ_MAX_BLOCKS];
static int rocketnet_fuzz_saved_blocks = 0;

static inline void rocketnet_fuzz_state_save( struct rocketnet_fuzz_pair_t * pair, int which )
{
    struct rocketnet_fuzz_saved_state_t * saved = &rocketnet_fuzz_saved_states[which];

    rocketnet_fuzz_saved_blocks = rocketnet_fuzz_num_blocks;
    for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
    {
        struct rocketnet_fuzz_block_t * b = &rocketnet_fuzz_blocks[i];
        void * copy = malloc( b->bytes > 0 ? b->bytes : 1 );
        rocketnet_fuzz_pair_require( copy != NULL, "the saved copy allocates" );
        memcpy( copy, b->p, b->bytes );
        rocketnet_fuzz_block_copies[which][i] = copy;
    }

    saved->pair = *pair;
    saved->random_state = rocketnet_fuzz_random_state;
    saved->client_clock_time = rocketnet_fuzz_client_clock_time;
}

// The page-tracked restore (Linux). A full restore copies every live block
// back, about 112 MB for the pair, and that copy was most of each input's
// time. Once both states are saved, every block page is write-protected;
// the first write to a page faults, the handler records the page and makes
// it writable, and the restore copies back only the recorded pages (plus,
// when the input asks for the other saved state, the pages where the two
// saved states differ) and protects them again. It fails closed: if protection
// cannot be set up, every restore is a full copy, and
// ROCKETNET_FUZZ_CHECK_RESTORE=1 compares every block with its saved copy
// after every restore. The pair runs no thread and makes no system call that
// writes a block (the loopback transport is memory only), so every write to
// a block is a fault in this thread.

static bool rocketnet_fuzz_check_restore = false;   // ROCKETNET_FUZZ_CHECK_RESTORE=1

static inline void rocketnet_fuzz_restore_full( int which )
{
    for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
    {
        memcpy( rocketnet_fuzz_blocks[i].p, rocketnet_fuzz_block_copies[which][i], rocketnet_fuzz_blocks[i].bytes );
    }
}

#if defined( ROCKETNET_FUZZ_PAGE_RESTORE )

struct rocketnet_fuzz_page_t
{
    int block;
    uint32_t page;          // the page's index within its block
};

static struct rocketnet_fuzz_page_t * rocketnet_fuzz_dirty = NULL;     // pages written since the last restore
static volatile size_t rocketnet_fuzz_num_dirty = 0;
static uint8_t * rocketnet_fuzz_page_marks[ROCKETNET_FUZZ_MAX_BLOCKS];  // per block page: 1 when in the dirty list
static struct rocketnet_fuzz_page_t * rocketnet_fuzz_differ = NULL;    // pages where the two saved states differ
static size_t rocketnet_fuzz_num_differ = 0;
static int rocketnet_fuzz_protect_state = 0;     // 0 not set up, 1 protected, -1 full copies only
static int rocketnet_fuzz_live_image = -1;       // the saved state the blocks hold, the dirty pages aside
static struct sigaction rocketnet_fuzz_old_segv;

static inline size_t rocketnet_fuzz_block_pages( int block )
{
    const size_t bytes = rocketnet_fuzz_blocks[block].bytes;
    return bytes == 0 ? 1 : ( bytes + rocketnet_fuzz_page_bytes - 1 ) / rocketnet_fuzz_page_bytes;
}

static inline uint8_t * rocketnet_fuzz_page_address( struct rocketnet_fuzz_page_t page )
{
    return (uint8_t *) rocketnet_fuzz_blocks[page.block].p + (size_t) page.page * rocketnet_fuzz_page_bytes;
}

static void rocketnet_fuzz_segv( int sig, siginfo_t * info, void * context )
{
    const uintptr_t address = (uintptr_t) info->si_addr;

    for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
    {
        const uintptr_t begin = (uintptr_t) rocketnet_fuzz_blocks[i].p;
        const uintptr_t end = begin + rocketnet_fuzz_block_pages( i ) * rocketnet_fuzz_page_bytes;
        if ( address < begin || address >= end )
            continue;

        const uint32_t page = (uint32_t) ( ( address - begin ) / rocketnet_fuzz_page_bytes );
        if ( rocketnet_fuzz_page_marks[i][page] )
            break;      // already writable: not our fault
        rocketnet_fuzz_page_marks[i][page] = 1;

        struct rocketnet_fuzz_page_t * entry = &rocketnet_fuzz_dirty[rocketnet_fuzz_num_dirty];
        entry->block = i;
        entry->page = page;
        rocketnet_fuzz_num_dirty = rocketnet_fuzz_num_dirty + 1;
        if ( mprotect( (void *) ( begin + (size_t) page * rocketnet_fuzz_page_bytes ), rocketnet_fuzz_page_bytes, PROT_READ | PROT_WRITE ) == 0 )
            return;
        break;
    }

    // not a tracked page: hand the fault to whoever had it before

    if ( rocketnet_fuzz_old_segv.sa_flags & SA_SIGINFO )
    {
        if ( rocketnet_fuzz_old_segv.sa_sigaction )
        {
            rocketnet_fuzz_old_segv.sa_sigaction( sig, info, context );
            return;
        }
    }
    else if ( rocketnet_fuzz_old_segv.sa_handler != SIG_DFL && rocketnet_fuzz_old_segv.sa_handler != SIG_IGN )
    {
        rocketnet_fuzz_old_segv.sa_handler( sig );
        return;
    }

    sigaction( SIGSEGV, &rocketnet_fuzz_old_segv, NULL );    // the fault repeats under the old disposition
}

static inline bool rocketnet_fuzz_protect_page( struct rocketnet_fuzz_page_t page, int prot )
{
    return mprotect( rocketnet_fuzz_page_address( page ), rocketnet_fuzz_page_bytes, prot ) == 0;
}

// after both states are saved: the blocks hold the connected image; list the pages
// where the two images differ, then protect every block page
static inline bool rocketnet_fuzz_protect_setup()
{
    size_t total = 0;

    for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
    {
        total += rocketnet_fuzz_block_pages( i );
    }

    rocketnet_fuzz_dirty = (struct rocketnet_fuzz_page_t *) calloc( total, sizeof( struct rocketnet_fuzz_page_t ) );
    rocketnet_fuzz_differ = (struct rocketnet_fuzz_page_t *) calloc( total, sizeof( struct rocketnet_fuzz_page_t ) );
    if ( !rocketnet_fuzz_dirty || !rocketnet_fuzz_differ )
        return false;

    for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
    {
        const size_t pages = rocketnet_fuzz_block_pages( i );
        rocketnet_fuzz_page_marks[i] = (uint8_t *) calloc( pages, 1 );
        if ( !rocketnet_fuzz_page_marks[i] )
            return false;

        const uint8_t * a = (const uint8_t *) rocketnet_fuzz_block_copies[ROCKETNET_FUZZ_STATE_CREATED][i];
        const uint8_t * b = (const uint8_t *) rocketnet_fuzz_block_copies[ROCKETNET_FUZZ_STATE_CONNECTED][i];
        const size_t bytes = rocketnet_fuzz_blocks[i].bytes;

        for ( size_t k = 0; k < pages; k++ )
        {
            const size_t from = k * rocketnet_fuzz_page_bytes;
            if ( from >= bytes )
                break;

            const size_t n = bytes - from < rocketnet_fuzz_page_bytes ? bytes - from : rocketnet_fuzz_page_bytes;
            if ( memcmp( a + from, b + from, n ) != 0 )
            {
                rocketnet_fuzz_differ[rocketnet_fuzz_num_differ].block = i;
                rocketnet_fuzz_differ[rocketnet_fuzz_num_differ].page = (uint32_t) k;
                rocketnet_fuzz_num_differ++;
            }
        }
    }

    struct sigaction action;
    memset( &action, 0, sizeof( action ) );
    action.sa_sigaction = rocketnet_fuzz_segv;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset( &action.sa_mask );
    if ( sigaction( SIGSEGV, &action, &rocketnet_fuzz_old_segv ) != 0 )
        return false;

    for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
    {
        if ( mprotect( rocketnet_fuzz_blocks[i].p, rocketnet_fuzz_block_pages( i ) * rocketnet_fuzz_page_bytes, PROT_READ ) != 0 )
        {
            sigaction( SIGSEGV, &rocketnet_fuzz_old_segv, NULL );

            for ( int j = 0; j <= i; j++ )
            {
                mprotect( rocketnet_fuzz_blocks[j].p, rocketnet_fuzz_block_pages( j ) * rocketnet_fuzz_page_bytes, PROT_READ | PROT_WRITE );
            }

            return false;
        }
    }

    rocketnet_fuzz_live_image = ROCKETNET_FUZZ_STATE_CONNECTED;

    return true;
}

static inline void rocketnet_fuzz_copy_page( struct rocketnet_fuzz_page_t page, int which )
{
    const size_t from = (size_t) page.page * rocketnet_fuzz_page_bytes;
    const size_t bytes = rocketnet_fuzz_blocks[page.block].bytes;
    if ( from >= bytes )
        return;

    const size_t n = bytes - from < rocketnet_fuzz_page_bytes ? bytes - from : rocketnet_fuzz_page_bytes;
    memcpy( (uint8_t *) rocketnet_fuzz_blocks[page.block].p + from, (const uint8_t *) rocketnet_fuzz_block_copies[which][page.block] + from, n );
}

static inline void rocketnet_fuzz_restore_pages( int which )
{
    // pages the last input wrote: still writable, copy then protect

    for ( size_t d = 0; d < rocketnet_fuzz_num_dirty; d++ )
    {
        rocketnet_fuzz_copy_page( rocketnet_fuzz_dirty[d], which );
    }

    // the other image: the pages where the two saved states differ

    if ( which != rocketnet_fuzz_live_image )
    {
        for ( size_t d = 0; d < rocketnet_fuzz_num_differ; d++ )
        {
            const struct rocketnet_fuzz_page_t page = rocketnet_fuzz_differ[d];
            const bool writable = rocketnet_fuzz_page_marks[page.block][page.page] != 0;
            if ( !writable )
            {
                rocketnet_fuzz_pair_require( rocketnet_fuzz_protect_page( page, PROT_READ | PROT_WRITE ), "a differing page opens for the restore" );
            }

            rocketnet_fuzz_copy_page( page, which );

            if ( !writable )
            {
                rocketnet_fuzz_pair_require( rocketnet_fuzz_protect_page( page, PROT_READ ), "a differing page closes after the restore" );
            }
        }

        rocketnet_fuzz_live_image = which;
    }

    for ( size_t d = 0; d < rocketnet_fuzz_num_dirty; d++ )
    {
        const struct rocketnet_fuzz_page_t page = rocketnet_fuzz_dirty[d];
        rocketnet_fuzz_pair_require( rocketnet_fuzz_protect_page( page, PROT_READ ), "a written page closes after the restore" );
        rocketnet_fuzz_page_marks[page.block][page.page] = 0;
    }

    rocketnet_fuzz_num_dirty = 0;
}

#endif // #if defined(ROCKETNET_FUZZ_PAGE_RESTORE)

static inline void rocketnet_fuzz_restore_blocks( int which )
{
#if defined( ROCKETNET_FUZZ_PAGE_RESTORE )
    if ( rocketnet_fuzz_protect_state == 0 )
    {
        rocketnet_fuzz_protect_state = rocketnet_fuzz_protect_setup() ? 1 : -1;
    }

    if ( rocketnet_fuzz_protect_state == 1 )
    {
        rocketnet_fuzz_restore_pages( which );
    }
    else
    {
        rocketnet_fuzz_restore_full( which );
    }
#else
    rocketnet_fuzz_restore_full( which );
#endif // #if defined( ROCKETNET_FUZZ_PAGE_RESTORE )
    if ( rocketnet_fuzz_check_restore )
    {
        for ( int i = 0; i < rocketnet_fuzz_num_blocks; i++ )
            rocketnet_fuzz_pair_require( memcmp( rocketnet_fuzz_blocks[i].p, rocketnet_fuzz_block_copies[which][i], rocketnet_fuzz_blocks[i].bytes ) == 0,
                                         "a restore leaves every block equal to its saved copy" );
    }
}

static inline void rocketnet_fuzz_state_restore( struct rocketnet_fuzz_pair_t * pair, int which )
{
    const struct rocketnet_fuzz_saved_state_t * saved = &rocketnet_fuzz_saved_states[which];

    rocketnet_fuzz_pair_require( rocketnet_fuzz_num_blocks == rocketnet_fuzz_saved_blocks, "the live blocks are the saved blocks" );
    rocketnet_fuzz_restore_blocks( which );
    *pair = saved->pair;
    rocketnet_fuzz_random_state = saved->random_state;
    rocketnet_fuzz_client_clock_time = saved->client_clock_time;

    // the transport points at the pair's own shim table

    pair->transport->io.operations = &pair->shim;
    rocketnet_fuzz_live_pair = pair;
    pair->allocations_after_create = rocketnet_fuzz_allocations;
}

// ROCKETNET_FUZZ_COVERAGE_INPUTS_ONLY=1, in a build with source-based
// coverage: the one-time create, connect and save run before any input
// and would count as covered code no input drove, so the profile counters
// are zeroed once the states are saved (tools/fuzz_coverage.sh sets it).
// Only the coverage build (tools/fuzz_coverage.sh, which defines
// ROCKETNET_FUZZ_COVERAGE_BUILD) links the profile runtime. Every other build
// names no profile symbol: a weak undefined reference links on Linux but not
// with the macOS linker.
#if defined( ROCKETNET_FUZZ_COVERAGE_BUILD )
extern "C" void __llvm_profile_reset_counters( void );
#endif // #if defined( ROCKETNET_FUZZ_COVERAGE_BUILD )

static inline void rocketnet_fuzz_coverage_reset_after_setup()
{
#if defined( ROCKETNET_FUZZ_COVERAGE_BUILD )
    char value[8];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_COVERAGE_INPUTS_ONLY", value, sizeof( value ) ) && value[0] == '1' )
    {
        __llvm_profile_reset_counters();
    }
#endif // #if defined( ROCKETNET_FUZZ_COVERAGE_BUILD )
}

// The pair for one input, freshly created (connected false: the handshake
// has not started) or carried through the handshake. The first call creates,
// connects and saves; it aborts if the deterministic pair does not create
// or connect -- both are properties of the fixed setup, not of the input.
static inline void rocketnet_fuzz_pair_acquire( struct rocketnet_fuzz_pair_t * pair, bool connected )
{
    if ( !rocketnet_fuzz_states_saved )
    {
        rocketnet_fuzz_pair_require( rocketnet_fuzz_pair_create( pair ), "the fuzz pair creates" );
        rocketnet_fuzz_state_save( pair, ROCKETNET_FUZZ_STATE_CREATED );
        rocketnet_fuzz_pair_require( rocketnet_fuzz_pair_connect( pair ), "the deterministic pair connects" );
        rocketnet_fuzz_pair_require( rocketnet_fuzz_num_blocks == rocketnet_fuzz_saved_blocks, "connect allocates nothing" );
        rocketnet_fuzz_state_save( pair, ROCKETNET_FUZZ_STATE_CONNECTED );
        rocketnet_fuzz_states_saved = true;
        rocketnet_fuzz_coverage_reset_after_setup();
    }

    rocketnet_fuzz_state_restore( pair, connected ? ROCKETNET_FUZZ_STATE_CONNECTED : ROCKETNET_FUZZ_STATE_CREATED );
}

// -----------------------------------------------------------------------
//  the replay check: ROCKETNET_FUZZ_CHECK_REPLAY=1 runs every input twice
//  and requires the same counters both times, the property a corpus replay
//  gate rests on
// -----------------------------------------------------------------------

static inline uint64_t rocketnet_fuzz_fnv( uint64_t hash, const void * data, size_t bytes )
{
    const uint8_t * p = (const uint8_t *) data;

    for ( size_t i = 0; i < bytes; i++ )
    {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }

    return hash;
}

static inline const struct rocketnet_counters_t * rocketnet_fuzz_pair_client_counters( struct rocketnet_fuzz_pair_t * pair )
{
    return rocketnet_client_transport_counters( rocketnet_message_client_transport( pair->client ) );
}

static inline uint64_t rocketnet_fuzz_pair_fingerprint( struct rocketnet_fuzz_pair_t * pair )
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    hash = rocketnet_fuzz_fnv( hash, pair->transport->counters.values, sizeof( pair->transport->counters.values ) );
    hash = rocketnet_fuzz_fnv( hash, rocketnet_message_server_counters( pair->server )->values, sizeof( pair->transport->counters.values ) );
    hash = rocketnet_fuzz_fnv( hash, rocketnet_message_client_counters( pair->client )->values, sizeof( pair->transport->counters.values ) );
    hash = rocketnet_fuzz_fnv( hash, rocketnet_fuzz_pair_client_counters( pair )->values, sizeof( pair->transport->counters.values ) );
    hash = rocketnet_fuzz_fnv( hash, &pair->time, sizeof( pair->time ) );
    return hash;
}

static bool rocketnet_fuzz_check_replay = false;
static bool rocketnet_fuzz_dump_counters = false;

// ROCKETNET_FUZZ_DUMP_COUNTERS=1 prints every nonzero counter of the server
// transport and of the message client after each input: how a seed author
// sees which branch of the reading order an input reached
static inline void rocketnet_fuzz_pair_dump( struct rocketnet_fuzz_pair_t * pair )
{
    const struct rocketnet_counters_t * blocks[4] = { &pair->transport->counters, rocketnet_message_server_counters( pair->server ),
                                                      rocketnet_fuzz_pair_client_counters( pair ), rocketnet_message_client_counters( pair->client ) };

    const char * names[4] = { "server_transport", "message_server", "client_transport", "message_client" };

    for ( int b = 0; b < 4; b++ )
        for ( int i = 0; i < rocketnet_num_counters(); i++ )
            if ( rocketnet_counter_value( blocks[b], i ) != 0 )
            {
                printf( "%s %s %llu\n", names[b], rocketnet_counter_name( i ), (unsigned long long) rocketnet_counter_value( blocks[b], i ) );
            }
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>
//
// yojimbo's gen_seed_corpus, built into the harness so the seeds are made
// by the same pair the harness replays them on. The generator runs the real
// handshake from the created state and a scripted session from the
// connected one (messages both ways, a message large enough to fragment,
// idle time for keep-alives and acks, a disconnect from each side), and
// records the first real datagram of every packet type in each direction,
// with its plaintext opened under the live keys. Because the pair is
// deterministic, a recorded datagram opens again under the harness's pair.
// Each target turns the recording into seeds, replays every seed through
// itself, and refuses to finish unless every packet type its receive loop
// reads has a seed that reaches that type's reader: random mutation cannot
// make a valid AEAD tag, so a type with no seed is a parser the fuzzer
// never enters.
// -----------------------------------------------------------------------

#define ROCKETNET_FUZZ_NUM_PACKET_TYPES 10

struct rocketnet_fuzz_recorded_t
{
    int bytes;
    bool handshake;
    int step;                   // the step it was recorded at, from the saved state
    uint8_t data[ROCKETNET_RECEIVE_CEILING];
    int plaintext_bytes;
    uint8_t plaintext[ROCKETNET_RECEIVE_CEILING];
};

struct rocketnet_fuzz_recording_t
{
    struct rocketnet_fuzz_recorded_t c2s[ROCKETNET_FUZZ_NUM_PACKET_TYPES];
    struct rocketnet_fuzz_recorded_t s2c[ROCKETNET_FUZZ_NUM_PACKET_TYPES];
};

// the mapping as it stood at the start of the recording step: a disconnect
// frees the live mapping in the same call that sends the disconnect, so its
// datagram is opened under this copy
static struct rocketnet_encryption_mapping_t rocketnet_fuzz_recorded_mapping;
static bool rocketnet_fuzz_recorded_mapping_valid = false;

// open a recorded sealed datagram under the live mapping (or the step's
// copy of it); -1 when it does not open
static inline int rocketnet_fuzz_pair_open( struct rocketnet_fuzz_pair_t * pair, bool c2s, const uint8_t * packet, int bytes, uint8_t * out )
{
    const struct rocketnet_encryption_mapping_t * m = rocketnet_fuzz_pair_mapping( pair );
    if ( !m && rocketnet_fuzz_recorded_mapping_valid )
    {
        m = &rocketnet_fuzz_recorded_mapping;
    }

    if ( !m || bytes < ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES )
        return -1;

    const uint8_t type = packet[0];
    const uint8_t * p = packet + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), 0 );
    const uint8_t * key = NULL;
    const uint8_t * ephemeral = NULL;
    if ( rocketnet_fuzz_session_type( type ) )
    {
        key = c2s ? m->keys.session_client_to_server : m->keys.session_server_to_client;
    }
    else if ( c2s && type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
    {
        key = m->keys.handshake_client_to_server;
        ephemeral = m->client_ephemeral_public_key;
    }
    else if ( !c2s && ( type == ROCKETNET_CONNECTION_CHALLENGE_PACKET || type == ROCKETNET_CONNECTION_DENIED_PACKET ) )
    {
        key = m->keys.handshake_server_to_client;
        ephemeral = type == ROCKETNET_CONNECTION_CHALLENGE_PACKET ? m->client_ephemeral_public_key : NULL;
    }

    if ( !key )
        return -1;

    uint8_t nonce[ROCKETNET_SESSION_NONCE_BYTES];
    const bool has_nonce = type == ROCKETNET_CONNECTION_CHALLENGE_PACKET || type == ROCKETNET_CONNECTION_DENIED_PACKET;
    return rocketnet_open_sealed_packet( out, has_nonce ? nonce : NULL, ROCKETNET_RECEIVE_CEILING, packet, bytes, type, sequence,
                                         pair->transport->protocol.protocol_id, m->connection_salt, ephemeral, key );
}

static int rocketnet_fuzz_record_steps = 0;

static inline void rocketnet_fuzz_pair_record_ring( struct rocketnet_fuzz_pair_t * pair, struct rocketnet_fuzz_ring_t * ring, bool c2s,
                                                    struct rocketnet_fuzz_recorded_t * table, bool handshake )
{
    for ( int i = 0; i < ring->count; i++ )
    {
        const struct rocketnet_fuzz_datagram_t * d = &ring->packet[i];
        const int type = d->data[0];
        if ( type >= ROCKETNET_FUZZ_NUM_PACKET_TYPES || table[type].bytes > 0 )
            continue;

        struct rocketnet_fuzz_recorded_t * r = &table[type];
        r->bytes = d->bytes;
        r->handshake = handshake;
        r->step = rocketnet_fuzz_record_steps;
        memcpy( r->data, d->data, (size_t) d->bytes );
        r->plaintext_bytes = rocketnet_fuzz_pair_open( pair, c2s, d->data, d->bytes, r->plaintext );
    }
}

static inline void rocketnet_fuzz_pair_record_keep_mapping( struct rocketnet_fuzz_pair_t * pair )
{
    const struct rocketnet_encryption_mapping_t * m = rocketnet_fuzz_pair_mapping( pair );
    if ( m )
    {
        rocketnet_fuzz_recorded_mapping = *m;
        rocketnet_fuzz_recorded_mapping_valid = true;
    }
}

static inline void rocketnet_fuzz_pair_record_step( struct rocketnet_fuzz_pair_t * pair, struct rocketnet_fuzz_recording_t * rec, bool handshake )
{
    rocketnet_fuzz_record_steps++;
    rocketnet_fuzz_pair_record_keep_mapping( pair );
    pair->time += ROCKETNET_FUZZ_DT;
    rocketnet_fuzz_pair_server_update( pair );
    rocketnet_fuzz_pair_client_update( pair );
    rocketnet_fuzz_pair_drain_messages( pair );
    rocketnet_fuzz_pair_send( pair );
    rocketnet_fuzz_pair_record_ring( pair, &pair->c2s, true, rec->c2s, handshake );
    rocketnet_fuzz_pair_record_ring( pair, &pair->s2c, false, rec->s2c, handshake );
    rocketnet_fuzz_pair_deliver_all( pair );
}

static inline void rocketnet_fuzz_pair_record( struct rocketnet_fuzz_pair_t * pair, struct rocketnet_fuzz_recording_t * rec )
{
    memset( rec, 0, sizeof( *rec ) );

    // the handshake: request, cookie, challenge, response

    rocketnet_fuzz_pair_acquire( pair, false );
    rocketnet_fuzz_record_steps = 0;

    for ( int i = 0; i < ROCKETNET_FUZZ_CONNECT_STEPS && !rocketnet_fuzz_pair_connected( pair ); i++ )
    {
        rocketnet_fuzz_pair_record_step( pair, rec, true );
    }

    // the session: payloads, a large reliable message (message-layer
    // blocks), acks and keep-alives both ways, then the client's disconnect

    static uint8_t message[ROCKETNET_FUZZ_MAX_MESSAGE_BYTES];

    for ( int i = 0; i < ROCKETNET_FUZZ_MAX_MESSAGE_BYTES; i++ )
    {
        message[i] = (uint8_t) i;
    }

    rocketnet_fuzz_pair_acquire( pair, true );
    rocketnet_fuzz_record_steps = 0;
    rocketnet_fuzz_recorded_mapping_valid = false;
    rocketnet_message_client_send( pair->client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, 64 );
    rocketnet_message_server_send( pair->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, 64 );
    rocketnet_fuzz_pair_record_step( pair, rec, false );
    rocketnet_message_client_send( pair->client, 3, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, ROCKETNET_FUZZ_MAX_MESSAGE_BYTES );
    rocketnet_message_server_send( pair->server, 0, 3, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, ROCKETNET_FUZZ_MAX_MESSAGE_BYTES );

    for ( int i = 0; i < 300; i++ )
    {
        rocketnet_fuzz_pair_record_step( pair, rec, false );
    }

    rocketnet_client_transport_disconnect( rocketnet_message_client_transport( pair->client ) );

    for ( int i = 0; i < 5; i++ )
    {
        rocketnet_fuzz_pair_record_step( pair, rec, false );
    }

    // the server's disconnect

    rocketnet_fuzz_pair_acquire( pair, true );
    rocketnet_fuzz_record_steps = 0;
    rocketnet_fuzz_pair_record_keep_mapping( pair );
    rocketnet_server_transport_disconnect_client( pair->transport, 0 );

    for ( int i = 0; i < 5; i++ )
    {
        rocketnet_fuzz_pair_record_step( pair, rec, false );
    }

    // the message layer promotes a large unreliable message to a reliable
    // block, so the transport's own fragmentation is reached through the
    // transport's payload send: one payload past a packet's room, each way

    rocketnet_fuzz_pair_acquire( pair, true );
    rocketnet_fuzz_record_steps = 0;
    rocketnet_client_transport_send_packet( rocketnet_message_client_transport( pair->client ), message, 3000 );
    rocketnet_server_transport_send_packet( pair->transport, 0, message, 3000 );

    for ( int i = 0; i < 5; i++ )
    {
        rocketnet_fuzz_pair_record_step( pair, rec, false );
    }
}

// one record: [mode][u16 length][bytes], the record shape the server and
// client targets read; a and b are concatenated
static inline int rocketnet_fuzz_record_write( uint8_t * out, uint8_t mode, const uint8_t * a, int a_bytes, const uint8_t * b, int b_bytes )
{
    const int length = a_bytes + b_bytes;

    out[0] = mode;
    out[1] = (uint8_t) ( length & 0xFF );
    out[2] = (uint8_t) ( length >> 8 );

    if ( a_bytes > 0 )
    {
        memcpy( out + 3, a, (size_t) a_bytes );
    }

    if ( b_bytes > 0 )
    {
        memcpy( out + 3 + a_bytes, b, (size_t) b_bytes );
    }

    return 3 + length;
}

static inline bool rocketnet_fuzz_seed_write( const char * dir, const char * name, const uint8_t * data, int bytes )
{
    char path[1024];
    snprintf( path, sizeof( path ), "%s/%s", dir, name );

    FILE * file = rocketnet_test_fopen( path, "wb" );
    if ( !file )
    {
        fprintf( stderr, "seed generator: cannot write %s\n", path );
        return false;
    }

    const bool ok = fwrite( data, 1, (size_t) bytes, file ) == (size_t) bytes;

    fclose( file );

    return ok;
}

// A seed is [selector] then `wait` empty records (each one quiet step) then
// one record. The generator searches the wait so the record lands where the
// recording says its type is read -- the handshake types at their state --
// and keeps the first wait whose replay raises `counter` past the replay of
// the same seed without the record.
typedef uint64_t ( *rocketnet_fuzz_run_t )( const uint8_t * data, size_t size );

static inline int rocketnet_fuzz_seed_place( uint8_t * out, rocketnet_fuzz_run_t run, const struct rocketnet_counters_t * ( *counters )(struct rocketnet_fuzz_pair_t *),
                                             int counter, uint8_t selector, int wait_from, int wait_to,
                                             uint8_t mode, const uint8_t * a, int a_bytes, const uint8_t * b, int b_bytes )
{
    for ( int wait = wait_from < 0 ? 0 : wait_from; wait <= wait_to && wait < 256; wait++ )
    {
        out[0] = selector;

        int pos = 1;

        for ( int i = 0; i <= wait; i++ )
        {
            pos += rocketnet_fuzz_record_write( out + pos, 0, NULL, 0, NULL, 0 );
        }

        // the baseline: the same steps, an empty record in the record's place

        run( out, (size_t) pos );

        const uint64_t before = rocketnet_counter_value( counters( rocketnet_fuzz_live_pair ), counter );

        pos -= 3;
        pos += rocketnet_fuzz_record_write( out + pos, mode, a, a_bytes, b, b_bytes );
        run( out, (size_t) pos );
        if ( rocketnet_counter_value( counters( rocketnet_fuzz_live_pair ), counter ) > before )
            return pos;
    }

    return 0;
}

static inline const struct rocketnet_counters_t * rocketnet_fuzz_server_counters_of( struct rocketnet_fuzz_pair_t * pair )
{
    return &pair->transport->counters;
}

static const char * const rocketnet_fuzz_packet_type_names[ROCKETNET_FUZZ_NUM_PACKET_TYPES] = {
    "request", "denied", "challenge", "response", "keep_alive", "payload", "disconnect", "cookie", "ack", "fragment"
};

#define ROCKETNET_FUZZ_PAIR_TARGET( run, generate )                                          \
    extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )                                    \
    {                                                                                                   \
        (void) argc;                                                                                    \
        (void) argv;                                                                                    \
        char check[8];                                                                                  \
        rocketnet_fuzz_check_replay = rocketnet_test_getenv( "ROCKETNET_FUZZ_CHECK_REPLAY", check, sizeof( check ) ) && check[0] == '1'; \
        char dump[8];                                                                                   \
        rocketnet_fuzz_dump_counters = rocketnet_test_getenv( "ROCKETNET_FUZZ_DUMP_COUNTERS", dump, sizeof( dump ) ) && dump[0] == '1'; \
        char restore[8];                                                                                \
        rocketnet_fuzz_check_restore = rocketnet_test_getenv( "ROCKETNET_FUZZ_CHECK_RESTORE", restore, sizeof( restore ) ) && restore[0] == '1'; \
        if ( rocketnet_fuzz_pair_initialize() != 0 )                                                    \
            return 1;                                                                                   \
        char seed_dir[1024];                                                                            \
        if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) )          \
            exit( generate( seed_dir ) ? 0 : 1 );                                                       \
        return 0;                                                                                       \
    }                                                                                                   \
    extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )                          \
    {                                                                                                   \
        const uint64_t first = run( data, size );                                                       \
        if ( rocketnet_fuzz_check_replay )                                                              \
            rocketnet_fuzz_pair_require( run( data, size ) == first, "an input replays to the same counters" ); \
        if ( rocketnet_fuzz_dump_counters && rocketnet_fuzz_live_pair )                                 \
            rocketnet_fuzz_pair_dump( rocketnet_fuzz_live_pair );                                       \
        return 0;                                                                                       \
    }

#endif // ROCKETNET_TESTS_FUZZ_ROCKETNET_FUZZ_PAIR_H
