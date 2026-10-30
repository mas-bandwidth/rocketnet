/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include <string.h>

ROCKETNET_STATIC_ASSERT( ROCKETNET_CLIENT_CONFIG_BYTES_1 < ROCKETNET_CLIENT_CONFIG_BYTES_2, "client public config revisions must grow" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_CLIENT_CONFIG_BYTES_2 < ROCKETNET_CLIENT_CONFIG_BYTES_3, "client public config revisions must grow" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_CLIENT_CONFIG_BYTES_3 < ROCKETNET_CLIENT_CONFIG_BYTES_4, "client public config revisions must grow" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_CLIENT_CONFIG_BYTES_4 == sizeof( struct rocketnet_client_config_t ), "client public config revision 4 must be the whole struct" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_CONFIG_BYTES_1 < ROCKETNET_SERVER_CONFIG_BYTES_2, "server public config revisions must grow" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_CONFIG_BYTES_2 < ROCKETNET_SERVER_CONFIG_BYTES_3, "server public config revisions must grow" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_CONFIG_BYTES_3 < ROCKETNET_SERVER_CONFIG_BYTES_4, "server public config revisions must grow" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_CONFIG_BYTES_4 < ROCKETNET_SERVER_CONFIG_BYTES_5, "server public config revisions must grow" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_SERVER_CONFIG_BYTES_5 == sizeof( struct rocketnet_server_config_t ), "server public config revision 5 must be the whole struct" );

#include <sodium.h>
#include <inttypes.h>

// Sized configs (docs/ABI-EVOLUTION.md). A config struct's latest revision is
// today's struct: every revision size in the header must be one the compiler's
// layout has, and the latest must equal sizeof, or a size formula is wrong for
// this target.

static_assert( ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1 < ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2, "client transport config revisions must grow" );
static_assert( ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2 == sizeof( struct rocketnet_client_transport_config_t ), "client transport config revision 2 size" );
static_assert( ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2 < ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_3, "server transport config revisions must grow" );
static_assert( ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_3 < ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_4, "server transport config revisions must grow" );
static_assert( ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_4 < ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_5, "server transport config revisions must grow" );
static_assert( ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_5 == sizeof( struct rocketnet_server_transport_config_t ), "server transport config revision 5 size" );
static_assert( ROCKETNET_MESSAGE_CONFIG_BYTES_2 == sizeof( struct rocketnet_message_config_t ), "message config revision 2 size" );

static const size_t rocketnet_client_transport_config_known_bytes[] = { ROCKETNET_CLIENT_TRANSPORT_CONFIG_KNOWN_BYTES };
static const size_t rocketnet_server_transport_config_known_bytes[] = { ROCKETNET_SERVER_TRANSPORT_CONFIG_KNOWN_BYTES };
static const size_t rocketnet_message_config_known_bytes[] = { ROCKETNET_MESSAGE_CONFIG_KNOWN_BYTES };

#define ROCKETNET_KNOWN_BYTES( table ) table, (int) ( sizeof( table ) / sizeof( table[0] ) )

// docs/ABI-EVOLUTION.md 1, rules 4 and 5: a size is accepted only when it is
// one the struct has had, and never past the struct this library holds.

static bool rocketnet_config_size_known( const char * type, size_t bytes, size_t current_bytes, const size_t * known, int num_known )
{
    for ( int i = 0; i < num_known; i++ )
    {
        if ( known[i] == bytes && bytes <= current_bytes )
            return true;
    }

    rocketnet_error( "%s is %" PRIu64 " bytes here, not a size this library knows (this library's is %" PRIu64 "); refused", type, (uint64_t) bytes, (uint64_t) current_bytes );

    return false;
}

// docs/ABI-EVOLUTION.md 3: the write behind every sized default.

int rocketnet_config_write_sized( const char * type, void * config, size_t config_bytes, const void * defaults, size_t defaults_bytes, const size_t * known, int num_known )
{
    if ( !rocketnet_config_size_known( type, config_bytes, defaults_bytes, known, num_known ) )
        return 0;

    memcpy( config, defaults, config_bytes );
    return 1;
}

// docs/ABI-EVOLUTION.md 3: the read behind every sized create. rule 5: a
// smaller known size reads the prefix and the rest stays the defaults.

int rocketnet_config_read_sized( const char * type, void * config, const void * defaults, size_t defaults_bytes, const void * source, size_t source_bytes, const size_t * known, int num_known )
{
    memcpy( config, defaults, defaults_bytes );

    if ( !rocketnet_config_size_known( type, source_bytes, defaults_bytes, known, num_known ) )
        return 0;

    memcpy( config, source, source_bytes );

    return 1;
}

int rocketnet_client_transport_default_config_sized( struct rocketnet_client_transport_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;

    struct rocketnet_client_transport_config_t defaults;
    rocketnet_client_transport_default_config_current( &defaults );

    return rocketnet_config_write_sized( "rocketnet_client_transport_config_t", config, config_bytes, &defaults, sizeof( defaults ), ROCKETNET_KNOWN_BYTES( rocketnet_client_transport_config_known_bytes ) );
}

int rocketnet_server_transport_default_config_sized( struct rocketnet_server_transport_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;

    struct rocketnet_server_transport_config_t defaults;
    rocketnet_server_transport_default_config_current( &defaults );

    return rocketnet_config_write_sized( "rocketnet_server_transport_config_t", config, config_bytes, &defaults, sizeof( defaults ), ROCKETNET_KNOWN_BYTES( rocketnet_server_transport_config_known_bytes ) );
}

int rocketnet_message_default_config_sized( struct rocketnet_message_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;

    struct rocketnet_message_config_t defaults;
    rocketnet_message_default_config( &defaults );

    return rocketnet_config_write_sized( "rocketnet_message_config_t", config, config_bytes, &defaults, sizeof( defaults ), ROCKETNET_KNOWN_BYTES( rocketnet_message_config_known_bytes ) );
}

// each sized create reads its configs into full structs here, then calls the
// unsized create; a NULL config passes through as NULL, the defaults.

static const struct rocketnet_client_transport_config_t * rocketnet_client_transport_config_read_sized( struct rocketnet_client_transport_config_t * read, const struct rocketnet_client_transport_config_t * config, size_t config_bytes, bool * refused )
{
    if ( config == NULL )
        return NULL;

    struct rocketnet_client_transport_config_t defaults;
    rocketnet_client_transport_default_config_current( &defaults );

    if ( !rocketnet_config_read_sized( "rocketnet_client_transport_config_t", read, &defaults, sizeof( defaults ), config, config_bytes, ROCKETNET_KNOWN_BYTES( rocketnet_client_transport_config_known_bytes ) ) )
        *refused = true;

    return read;
}

static const struct rocketnet_server_transport_config_t * rocketnet_server_transport_config_read_sized( struct rocketnet_server_transport_config_t * read, const struct rocketnet_server_transport_config_t * config, size_t config_bytes, bool * refused )
{
    if ( config == NULL )
        return NULL;

    struct rocketnet_server_transport_config_t defaults;
    rocketnet_server_transport_default_config_current( &defaults );

    if ( !rocketnet_config_read_sized( "rocketnet_server_transport_config_t", read, &defaults, sizeof( defaults ), config, config_bytes, ROCKETNET_KNOWN_BYTES( rocketnet_server_transport_config_known_bytes ) ) )
        *refused = true;

    return read;
}

static const struct rocketnet_message_config_t * rocketnet_message_config_read_sized( struct rocketnet_message_config_t * read, const struct rocketnet_message_config_t * config, size_t config_bytes, bool * refused )
{
    if ( config == NULL )
        return NULL;

    struct rocketnet_message_config_t defaults;
    rocketnet_message_default_config( &defaults );

    if ( !rocketnet_config_read_sized( "rocketnet_message_config_t", read, &defaults, sizeof( defaults ), config, config_bytes, ROCKETNET_KNOWN_BYTES( rocketnet_message_config_known_bytes ) ) )
        *refused = true;

    return read;
}

struct rocketnet_client_transport_t * rocketnet_client_transport_create_sized( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, size_t config_bytes, double time )
{
    bool refused = false;
    struct rocketnet_client_transport_config_t read;
    const struct rocketnet_client_transport_config_t * effective = rocketnet_client_transport_config_read_sized( &read, config, config_bytes, &refused );

    if ( refused )
        return NULL;

    return rocketnet_client_transport_create_current( context, connect_token, effective, time );
}

// the server config carries the secret key: the stack copy is wiped on every path

struct rocketnet_server_transport_t * rocketnet_server_transport_create_sized( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config, size_t config_bytes )
{
    bool refused = false;
    struct rocketnet_server_transport_config_t read;
    const struct rocketnet_server_transport_config_t * effective = rocketnet_server_transport_config_read_sized( &read, config, config_bytes, &refused );
    struct rocketnet_server_transport_t * server_transport = refused ? NULL : rocketnet_server_transport_create_current( context, public_address_string, effective );
    sodium_memzero( &read, sizeof( read ) );

    return server_transport;
}

struct rocketnet_message_client_t * rocketnet_message_client_create_sized( void * context, const uint8_t * connect_token,
                                                                           const struct rocketnet_client_transport_config_t * transport_config, size_t transport_config_bytes,
                                                                           const struct rocketnet_message_config_t * message_config, size_t message_config_bytes,
                                                                           void * memory, uint64_t memory_bytes, uint32_t send_kbps )
{
    bool refused = false;
    struct rocketnet_client_transport_config_t transport_read;
    struct rocketnet_message_config_t message_read;
    const struct rocketnet_client_transport_config_t * transport_effective = rocketnet_client_transport_config_read_sized( &transport_read, transport_config, transport_config_bytes, &refused );
    const struct rocketnet_message_config_t * message_effective = rocketnet_message_config_read_sized( &message_read, message_config, message_config_bytes, &refused );

    if ( refused )
        return NULL;

    return rocketnet_message_client_create( context, connect_token, transport_effective, message_effective, memory, memory_bytes, send_kbps );
}

struct rocketnet_message_server_t * rocketnet_message_server_create_sized( void * context, const char * address,
                                                                           const struct rocketnet_server_transport_config_t * transport_config, size_t transport_config_bytes,
                                                                           const struct rocketnet_message_config_t * message_config, size_t message_config_bytes,
                                                                           void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps )
{
    bool refused = false;
    struct rocketnet_server_transport_config_t transport_read;
    struct rocketnet_message_config_t message_read;
    const struct rocketnet_server_transport_config_t * transport_effective = rocketnet_server_transport_config_read_sized( &transport_read, transport_config, transport_config_bytes, &refused );
    const struct rocketnet_message_config_t * message_effective = rocketnet_message_config_read_sized( &message_read, message_config, message_config_bytes, &refused );
    struct rocketnet_message_server_t * server = refused ? NULL : rocketnet_message_server_create( context, address, transport_effective, message_effective, memory, memory_bytes, per_client_send_kbps );
    sodium_memzero( &transport_read, sizeof( transport_read ) );

    return server;
}
