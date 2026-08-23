/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Stage 1 of the flow -> rocketnet conversion.

    WHAT IS REAL IN THIS FILE: the global lifecycle (init/term/time), the
    allocator, logging and assert hooks, error and backend strings, the
    default configs, addresses (rocketnet_address.cpp), the platform time
    base (rocketnet_platform.cpp), and random_key. These are moved from flow
    and converted to the rocketnet idiom on arrival, and their tests are real.

    WHAT IS STUBBED: everything that needs a transport underneath — pools,
    channels, connections, flows-with-subscribers, IO backends, connect token
    sealing. Every one of the 72 exported functions exists with the correct
    signature; the unimplemented ones return the documented error for the
    situation the caller is actually in (no pool exists, so a claim is
    POOL_EXHAUSTED; no connection can exist, so an id is UNKNOWN_CONNECTION;
    no claim can be live, so a buffer handle is STALE_BUFFER) and never crash.

    STAGE 3 DESIGN NOTE, recorded where the stubs are so nobody follows the
    wrong reference: the connection layer follows the NETCODE model — a
    backend-issued sealed connect token and a handshake with a MUTUAL
    protocol id check (Glenn: "netcode already does this fine", "established
    best practice"). flow's handshake-less direct path is passthrough, not
    intended, temporary — do not port it as the connection design.
*/

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>

#if defined( _MSC_VER )
#pragma warning(push)
#pragma warning(disable:4996)
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
// stage 2 brings the windows CSPRNG with the IOCP backend work
#else
#include <sys/random.h>
#endif

// ---------------------------------------------------------------
// Asserts. Moved from flow.cpp.
// ---------------------------------------------------------------

static void default_assert_function( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_error( "assert failed: ( %s ), function %s, file %s, line %d", condition, function, file, line );
    fflush( stdout );
    #if defined(_MSC_VER)
        __debugbreak();
    #elif defined(__clang__)
        __builtin_debugtrap();
    #elif defined(__GNUC__)
        __builtin_trap();
    #elif defined(linux) || defined(__linux) || defined(__linux__) || defined(__APPLE__)
        raise(SIGTRAP);
    #else
        #error "asserts not supported on this platform!"
    #endif
}

void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line ) = default_assert_function;

void rocketnet_assert_function( void (*function)( const char * condition, const char * function, const char * file, int line ) )
{
    if ( function )
    {
        rocketnet_assert_function_pointer = function;
    }
    else
    {
        rocketnet_assert_function_pointer = default_assert_function;
    }
}

// ---------------------------------------------------------------
// Logging. Moved from flow.cpp. flow's exported flow_info/warn/error/debug
// conveniences are internal here (rocketnet_internal.h) because rocketnet.h
// exports only rocketnet_printf.
// ---------------------------------------------------------------

static int log_quiet = 0;

void rocketnet_quiet( bool flag )
{
    log_quiet = flag;
}

static int log_level = ROCKETNET_LOG_LEVEL_INFO;

void rocketnet_log_level( int level )
{
    log_level = level;
}

static const char * rocketnet_log_level_string( int level )
{
    if ( level == ROCKETNET_LOG_LEVEL_SPAM )
        return "spam";
    else if ( level == ROCKETNET_LOG_LEVEL_DEBUG )
        return "debug";
    else if ( level == ROCKETNET_LOG_LEVEL_INFO )
        return "info";
    else if ( level == ROCKETNET_LOG_LEVEL_ERROR )
        return "error";
    else if ( level == ROCKETNET_LOG_LEVEL_WARN )
        return "warning";
    else
        return "???";
}

static void default_log_function( int level, const char * format, ... )
{
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    if ( level != ROCKETNET_LOG_LEVEL_NONE )
    {
        if ( !log_quiet )
        {
            const char * level_string = rocketnet_log_level_string( level );
            printf( "%.6f: %s: %s\n", rocketnet_platform_time(), level_string, buffer );
        }
    }
    else
    {
        printf( "%s\n", buffer );
    }
    va_end( args );
    fflush( stdout );
}

static void (*log_function)( int level, const char * format, ... ) = default_log_function;

void rocketnet_log_function( void (*function)( int level, const char * format, ... ) )
{
    if ( function )
    {
        log_function = function;
    }
    else
    {
        log_function = default_log_function;
    }
}

void rocketnet_printf( int level, const char * format, ... )
{
    if ( level > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( level, "%s", buffer );
    va_end( args );
}

void rocketnet_info( const char * format, ... )
{
    if ( ROCKETNET_LOG_LEVEL_INFO > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_INFO, "%s", buffer );
    va_end( args );
}

void rocketnet_warn( const char * format, ... )
{
    if ( ROCKETNET_LOG_LEVEL_WARN > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_WARN, "%s", buffer );
    va_end( args );
}

void rocketnet_error( const char * format, ... )
{
    if ( ROCKETNET_LOG_LEVEL_ERROR > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_ERROR, "%s", buffer );
    va_end( args );
}

void rocketnet_debug( const char * format, ... )
{
    if ( ROCKETNET_LOG_LEVEL_DEBUG > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( ROCKETNET_LOG_LEVEL_DEBUG, "%s", buffer );
    va_end( args );
}

// ---------------------------------------------------------------
// Allocator. Moved from flow.cpp. flow_clear_and_free is not at the
// rocketnet seam and was not moved; a stage that needs it moves it.
// ---------------------------------------------------------------

static void * rocketnet_default_malloc_function( void * context, size_t bytes )
{
    (void) context;
    return malloc( bytes );
}

static void rocketnet_default_free_function( void * context, void * p )
{
    (void) context;
    free( p );
}

static void * (*rocketnet_malloc_function)( void * context, size_t bytes ) = rocketnet_default_malloc_function;
static void (*rocketnet_free_function)( void * context, void * p ) = rocketnet_default_free_function;

void rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ),
                          void (*free_function)( void * context, void * p ) )
{
    // Passing NULL for both restores the defaults. Passing NULL for one is a
    // caller bug caught by the assert; in release the pair is left unchanged
    // rather than half-swapped.
    if ( malloc_function == NULL && free_function == NULL )
    {
        rocketnet_malloc_function = rocketnet_default_malloc_function;
        rocketnet_free_function = rocketnet_default_free_function;
        return;
    }
    rocketnet_assert( malloc_function );
    rocketnet_assert( free_function );
    if ( !malloc_function || !free_function )
        return;
    rocketnet_malloc_function = malloc_function;
    rocketnet_free_function = free_function;
}

void * rocketnet_malloc( void * context, size_t bytes )
{
    rocketnet_assert( rocketnet_malloc_function );
    return rocketnet_malloc_function( context, bytes );
}

void rocketnet_free( void * context, void * p )
{
    rocketnet_assert( rocketnet_free_function );
    return rocketnet_free_function( context, p );
}

// ---------------------------------------------------------------
// Internal string copy. Moved from flow_copy_string.
// ---------------------------------------------------------------

size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size )
{
    rocketnet_assert( dest );
    rocketnet_assert( source );
    rocketnet_assert( dest_size >= 1 );
    memset( dest, 0, dest_size );
    size_t i = 0;
    for ( ; i < dest_size - 1; i++ )
    {
        if ( source[i] == '\0' )
            break;
        dest[i] = source[i];
    }
    return i;
}

// ---------------------------------------------------------------
// Global lifecycle. Moved from flow.cpp and converted at the seam:
// flow_init( void * context = NULL ) returning bool became
// rocketnet_init( void * context ) returning int — the default argument
// does not survive a C header, and the header says int error codes.
// ---------------------------------------------------------------

void * rocketnet_global_context = NULL;

static bool rocketnet_initialized = false;

int rocketnet_init( void * context )
{
    rocketnet_assert( !rocketnet_initialized );

    rocketnet_global_context = context;

    if ( !rocketnet_platform_init() )
    {
        rocketnet_error( "failed to initialize platform" );
        return ROCKETNET_ERROR;
    }

    rocketnet_info( "rocketnet version is %s", ROCKETNET_VERSION_FULL );

    const char * log_level_override = rocketnet_platform_getenv( "ROCKETNET_LOG_LEVEL" );
    if ( log_level_override )
    {
        log_level = atoi( log_level_override );
        rocketnet_info( "log level overridden to %d", log_level );
    }

    rocketnet_initialized = true;

    return ROCKETNET_OK;
}

void rocketnet_term( void )
{
    rocketnet_platform_term();

    rocketnet_global_context = NULL;

    rocketnet_initialized = false;
}

double rocketnet_time( void )
{
    return rocketnet_platform_time();
}

uint64_t rocketnet_protocol_version( void )
{
    // Wire compatibility follows major.minor; patch releases must not change
    // the protocol version, so patch is deliberately excluded. flow derived
    // this by hashing "majorminor" — whether the handshake wants a hash or
    // this packed form is a stage 3 decision, and nothing on the wire depends
    // on it yet.
    return ( (uint64_t) ROCKETNET_VERSION_MAJOR_INT << 32 ) | ( (uint64_t) ROCKETNET_VERSION_MINOR_INT << 16 );
}

const char * rocketnet_error_string( int result )
{
    switch ( result )
    {
        case ROCKETNET_OK:                          return "ok";
        case ROCKETNET_ERROR:                       return "error";
        case ROCKETNET_ERROR_INVALID_ARGUMENT:      return "invalid argument";
        case ROCKETNET_ERROR_OUT_OF_MEMORY:         return "out of memory";
        case ROCKETNET_ERROR_NOT_CONNECTED:         return "not connected";
        case ROCKETNET_ERROR_UNKNOWN_CONNECTION:    return "unknown connection";
        case ROCKETNET_ERROR_UNKNOWN_FLOW:          return "unknown flow";
        case ROCKETNET_ERROR_UNKNOWN_CHANNEL:       return "unknown channel";
        case ROCKETNET_ERROR_POOL_EXHAUSTED:        return "send buffer pool exhausted";
        case ROCKETNET_ERROR_QUOTA_EXCEEDED:        return "connection buffer quota exceeded";
        case ROCKETNET_ERROR_QUEUE_FULL:            return "channel send queue full";
        case ROCKETNET_ERROR_PAYLOAD_TOO_LARGE:     return "payload too large";
        case ROCKETNET_ERROR_STALE_BUFFER:          return "stale buffer";
        case ROCKETNET_ERROR_CHANNEL_MISMATCH:      return "channel layout mismatch";
        case ROCKETNET_ERROR_TOKEN_INVALID:         return "connect token invalid";
        case ROCKETNET_ERROR_TOKEN_EXPIRED:         return "connect token expired";
        case ROCKETNET_ERROR_SERVER_FULL:           return "server full";
        case ROCKETNET_ERROR_BACKEND_UNAVAILABLE:   return "backend unavailable";
        default:
            break;
    }
    return "unknown error";
}

bool rocketnet_backend_available( int backend )
{
    // Stage 1 has no IO backends at all, so the honest answer is that nothing
    // can actually run right now — including SOCKET. The stage that lands the
    // socket backend flips SOCKET (and AUTO) to true here.
    (void) backend;
    return false;
}

const char * rocketnet_backend_string( int backend )
{
    switch ( backend )
    {
        case ROCKETNET_BACKEND_AUTO:    return "auto";
        case ROCKETNET_BACKEND_SOCKET:  return "socket";
        case ROCKETNET_BACKEND_XDP:     return "xdp";
        case ROCKETNET_BACKEND_IOCP:    return "iocp";
        case ROCKETNET_BACKEND_KQUEUE:  return "kqueue";
        default:
            break;
    }
    return "unknown";
}

// ---------------------------------------------------------------
// Default configs
// ---------------------------------------------------------------

void rocketnet_default_channel_config( struct rocketnet_channel_config_t * config, int kind )
{
    rocketnet_assert( config );
    if ( !config )
        return;
    rocketnet_assert( kind == ROCKETNET_CHANNEL_UNRELIABLE || kind == ROCKETNET_CHANNEL_RELIABLE || kind == ROCKETNET_CHANNEL_LATEST );
    memset( config, 0, sizeof( rocketnet_channel_config_t ) );
    config->kind = kind;
    config->max_payload_bytes = ROCKETNET_MAX_PAYLOAD_BYTES;    // one packet's worth; no fragmentation unless asked for
    config->send_queue_size = 256;                              // ~4 seconds of backlog for a 60Hz caller
    config->receive_queue_size = 256;
}

void rocketnet_default_connection_config( struct rocketnet_connection_config_t * config )
{
    rocketnet_assert( config );
    if ( !config )
        return;
    memset( config, 0, sizeof( rocketnet_connection_config_t ) );
    config->num_channels = 1;
    rocketnet_default_channel_config( &config->channel[0], ROCKETNET_CHANNEL_UNRELIABLE );
    config->timeout_seconds = 10.0f;
    config->keep_alive_seconds = 0.1f;
}

void rocketnet_default_client_config( struct rocketnet_client_config_t * config )
{
    rocketnet_assert( config );
    if ( !config )
        return;
    memset( config, 0, sizeof( rocketnet_client_config_t ) );
    rocketnet_copy_string( config->bind_address, "0.0.0.0:0", sizeof( config->bind_address ) );
    config->protocol_id = 0;
    rocketnet_default_connection_config( &config->connection );
    config->pool_bytes = 1 * 1024 * 1024;                       // a client sends little
    config->socket_send_buffer_size = 256 * 1024;
    config->socket_receive_buffer_size = 256 * 1024;
    config->backend = ROCKETNET_BACKEND_AUTO;
    config->receive_callback = NULL;
}

void rocketnet_default_server_config( struct rocketnet_server_config_t * config )
{
    rocketnet_assert( config );
    if ( !config )
        return;
    memset( config, 0, sizeof( rocketnet_server_config_t ) );
    rocketnet_copy_string( config->server_address, "127.0.0.1:0", sizeof( config->server_address ) );
    rocketnet_copy_string( config->bind_address, "0.0.0.0:0", sizeof( config->bind_address ) );
    config->protocol_id = 0;
    config->max_connections = 64;
    config->max_flows = 64;
    rocketnet_default_connection_config( &config->connection );
    config->pool_bytes = 16 * 1024 * 1024;
    config->max_pool_bytes_per_connection = 1 * 1024 * 1024;
    config->socket_send_buffer_size = 4 * 1024 * 1024;
    config->socket_receive_buffer_size = 4 * 1024 * 1024;
    config->backend = ROCKETNET_BACKEND_AUTO;
    config->num_queues = 0;                                     // pick from the machine
}

// ---------------------------------------------------------------
// Shared validation
// ---------------------------------------------------------------

static bool rocketnet_validate_connection_config( const rocketnet_connection_config_t * connection )
{
    if ( connection->num_channels < 0 || connection->num_channels > ROCKETNET_MAX_CHANNELS )
    {
        rocketnet_error( "invalid num_channels: %d", connection->num_channels );
        return false;
    }

    for ( int i = 0; i < connection->num_channels; i++ )
    {
        const rocketnet_channel_config_t * channel = &connection->channel[i];

        if ( channel->kind != ROCKETNET_CHANNEL_UNRELIABLE &&
             channel->kind != ROCKETNET_CHANNEL_RELIABLE &&
             channel->kind != ROCKETNET_CHANNEL_LATEST )
        {
            rocketnet_error( "channel %d has invalid kind: %d", i, channel->kind );
            return false;
        }

        if ( channel->max_payload_bytes <= 0 || channel->max_payload_bytes > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES )
        {
            rocketnet_error( "channel %d has invalid max_payload_bytes: %d", i, channel->max_payload_bytes );
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------
// Connect tokens
// ---------------------------------------------------------------

int rocketnet_generate_connect_token( uint64_t protocol_id,
                                      uint64_t client_id,
                                      int expire_seconds,
                                      int timeout_seconds,
                                      int num_server_addresses,
                                      const char ** server_addresses,
                                      const uint8_t * private_key,
                                      const uint8_t * user_data,
                                      uint8_t * output )
{
    (void) protocol_id;
    (void) client_id;
    (void) expire_seconds;
    (void) timeout_seconds;

    rocketnet_assert( output );

    if ( !output || !private_key || !user_data || !server_addresses )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_server_addresses < 1 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // Argument validation is real: every server address must parse.
    for ( int i = 0; i < num_server_addresses; i++ )
    {
        rocketnet_address_t address;
        if ( rocketnet_address_parse( &address, server_addresses[i] ) != ROCKETNET_OK )
        {
            rocketnet_error( "connect token server address %d does not parse: %s", i, server_addresses[i] ? server_addresses[i] : "(null)" );
            return ROCKETNET_ERROR_INVALID_ARGUMENT;
        }
    }

    // STUB (stage 3): the real token is the netcode.io model, unchanged —
    // a versioned token whose private portion is sealed under the private
    // key the backend shares with the server, carrying per-client keys, the
    // client id, expiry, and the user data. The handshake that consumes it
    // performs a MUTUAL protocol id check per the recorded ruling; netcode
    // is the reference, not flow's direct path. Until then this writes a
    // placeholder all-zero token so callers exercise the full connect path
    // and fail at the connection stage, not here.
    memset( output, 0, ROCKETNET_CONNECT_TOKEN_BYTES );

    return ROCKETNET_OK;
}

int rocketnet_random_key( uint8_t * key )
{
    rocketnet_assert( key );

    if ( !key )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    // STUB (stage 2): the windows CSPRNG arrives with the windows platform
    // work. Failing loudly beats a weak key quietly.
    memset( key, 0, ROCKETNET_KEY_BYTES );
    return ROCKETNET_ERROR;
#else
    if ( getentropy( key, ROCKETNET_KEY_BYTES ) != 0 )
        return ROCKETNET_ERROR;
    return ROCKETNET_OK;
#endif
}

// ===============================================================
// CLIENT
// ===============================================================

struct rocketnet_client_t
{
    void * context;
    rocketnet_client_config_t config;
    rocketnet_address_t bind_address;
    int state;
    double connect_start_time;
};

struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config )
{
    rocketnet_assert( config );

    if ( !config )
    {
        rocketnet_error( "client create failed: config is NULL" );
        return NULL;
    }

    rocketnet_address_t bind_address;
    if ( rocketnet_address_parse( &bind_address, config->bind_address ) != ROCKETNET_OK )
    {
        rocketnet_error( "client create failed: could not parse bind address: %s", config->bind_address );
        return NULL;
    }

    if ( !rocketnet_validate_connection_config( &config->connection ) )
    {
        rocketnet_error( "client create failed: invalid connection config" );
        return NULL;
    }

    if ( config->pool_bytes <= 0 )
    {
        rocketnet_error( "client create failed: invalid pool_bytes: %d", config->pool_bytes );
        return NULL;
    }

    rocketnet_client_t * client = (rocketnet_client_t*) rocketnet_malloc( context, sizeof( rocketnet_client_t ) );
    if ( !client )
    {
        rocketnet_error( "client create failed: out of memory" );
        return NULL;
    }

    memset( client, 0, sizeof( rocketnet_client_t ) );

    client->context = context;
    client->config = *config;
    client->bind_address = bind_address;
    client->state = ROCKETNET_CLIENT_STATE_DISCONNECTED;

    // STUB (stage 2): no socket is bound and no pool is allocated yet. The
    // send/receive path and the claim pool arrive with the transport.

    return client;
}

void rocketnet_client_destroy( struct rocketnet_client_t * client )
{
    if ( !client )
        return;

    rocketnet_client_disconnect( client );

    rocketnet_free( client->context, client );
}

int rocketnet_client_connect( struct rocketnet_client_t * client, const uint8_t * connect_token )
{
    rocketnet_assert( client );
    rocketnet_assert( connect_token );

    if ( !client || !connect_token )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 3): the real path parses and validates the netcode-model
    // connect token (returning ROCKETNET_ERROR_TOKEN_INVALID / _EXPIRED on a
    // bad one), then begins the handshake — request, challenge, response —
    // with a MUTUAL protocol id check. netcode is the reference for that
    // handshake; flow's handshake-less direct path is passthrough, not
    // intended, temporary, and is NOT the model. Until the transport exists
    // the attempt starts and can only time out, which is the correct failure
    // for a request nothing can answer.

    client->state = ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST;
    client->connect_start_time = rocketnet_time();

    return ROCKETNET_OK;
}

void rocketnet_client_disconnect( struct rocketnet_client_t * client )
{
    rocketnet_assert( client );

    if ( !client )
        return;

    // STUB (stage 3): sends the disconnect packets so the server does not
    // wait out a timeout. Nothing is on the wire yet.

    client->state = ROCKETNET_CLIENT_STATE_DISCONNECTED;
}

int rocketnet_client_state( struct rocketnet_client_t * client )
{
    rocketnet_assert( client );

    if ( !client )
        return ROCKETNET_CLIENT_STATE_DISCONNECTED;

    return client->state;
}

void rocketnet_client_update( struct rocketnet_client_t * client )
{
    rocketnet_assert( client );

    if ( !client )
        return;

    // STUB (stage 2/3): receive, dispatch, acknowledge, retransmit. With no
    // transport underneath, the only live behavior is the connection request
    // timing out — nothing can answer it. Stage 3 drives this from the
    // token's own timeout per the netcode model; until a token is parsed the
    // connection config's timeout is the honest stand-in.

    if ( client->state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST )
    {
        double elapsed = rocketnet_time() - client->connect_start_time;
        if ( elapsed > client->config.connection.timeout_seconds )
        {
            client->state = ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT;
        }
    }
}

void rocketnet_client_flush( struct rocketnet_client_t * client )
{
    rocketnet_assert( client );

    if ( !client )
        return;

    // STUB (stage 2): pushes queued payloads to the wire. Nothing is queued
    // because nothing can be committed yet.
}

uint16_t rocketnet_client_port( struct rocketnet_client_t * client )
{
    rocketnet_assert( client );

    if ( !client )
        return 0;

    // STUB (stage 2): once a socket is bound this reports the actual bound
    // port. Until then it is the configured bind port — 0 when the config
    // asked for an ephemeral port, which is the honest answer for a socket
    // that does not exist.
    return client->bind_address.port;
}

const struct rocketnet_address_t * rocketnet_client_server_address( struct rocketnet_client_t * client )
{
    rocketnet_assert( client );

    if ( !client )
        return NULL;

    // STUB (stage 3): the server address comes from the connect token once
    // tokens parse. NULL until a connection exists. (The header does not say
    // what this returns when disconnected — recorded as underspecified.)
    return NULL;
}

int rocketnet_client_claim( struct rocketnet_client_t * client, int bytes, struct rocketnet_buffer_t * buffer )
{
    rocketnet_assert( client );
    rocketnet_assert( buffer );

    if ( !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // A failed claim leaves the caller's buffer zeroed rather than half-filled.
    memset( buffer, 0, sizeof( rocketnet_buffer_t ) );

    if ( !client )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes <= 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES )
        return ROCKETNET_ERROR_PAYLOAD_TOO_LARGE;

    // STUB (stage 2): the claim pool — size classes, slot generations, one
    // atomic on the hot path — is the first real transport piece. No pool
    // exists yet, and a pool of zero bytes is permanently exhausted.
    return ROCKETNET_ERROR_POOL_EXHAUSTED;
}

int rocketnet_client_commit( struct rocketnet_client_t * client, int channel, const struct rocketnet_buffer_t * buffer, int bytes )
{
    rocketnet_assert( client );
    rocketnet_assert( buffer );

    if ( !client || !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes < 0 || bytes > buffer->bytes )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( channel < 0 || channel >= client->config.connection.num_channels )
        return ROCKETNET_ERROR_UNKNOWN_CHANNEL;

    if ( client->state != ROCKETNET_CLIENT_STATE_CONNECTED )
        return ROCKETNET_ERROR_NOT_CONNECTED;

    // STUB (stage 2): with no pool there can be no live claim, so any handle
    // that reaches here is stale by definition.
    return ROCKETNET_ERROR_STALE_BUFFER;
}

int rocketnet_client_abandon( struct rocketnet_client_t * client, const struct rocketnet_buffer_t * buffer )
{
    rocketnet_assert( client );
    rocketnet_assert( buffer );

    if ( !client || !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 2): no pool, no live claims, every handle is stale.
    return ROCKETNET_ERROR_STALE_BUFFER;
}

int rocketnet_client_send( struct rocketnet_client_t * client, int channel, const uint8_t * payload, int bytes )
{
    rocketnet_assert( client );
    rocketnet_assert( payload );

    if ( !client || !payload )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes <= 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( channel < 0 || channel >= client->config.connection.num_channels )
        return ROCKETNET_ERROR_UNKNOWN_CHANNEL;

    if ( client->state != ROCKETNET_CLIENT_STATE_CONNECTED )
        return ROCKETNET_ERROR_NOT_CONNECTED;

    // STUB (stage 2): claim, copy, commit once the pool exists.
    return ROCKETNET_ERROR_POOL_EXHAUSTED;
}

int rocketnet_client_receive( struct rocketnet_client_t * client, struct rocketnet_payload_t * payloads, int max_payloads, int * num_payloads )
{
    rocketnet_assert( client );
    rocketnet_assert( payloads );
    rocketnet_assert( num_payloads );

    if ( !client || !payloads || !num_payloads )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    *num_payloads = 0;

    if ( max_payloads < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // Two receive paths racing over the same payloads is a bug waiting to be
    // written. This check is real, per the header.
    if ( client->config.receive_callback != NULL )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 2): nothing can arrive without a transport. OK with zero
    // payloads is the documented answer for "nothing arrived".
    return ROCKETNET_OK;
}

int rocketnet_client_release( struct rocketnet_client_t * client, const struct rocketnet_payload_t * payloads, int num_payloads )
{
    rocketnet_assert( client );

    if ( !client )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_payloads < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_payloads == 0 )
        return ROCKETNET_OK;

    if ( !payloads )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 2): no payload can be live, so any release is a release of
    // something already dead — the documented answer is stale, not corruption.
    return ROCKETNET_ERROR_STALE_BUFFER;
}

bool rocketnet_client_stats( struct rocketnet_client_t * client, struct rocketnet_connection_stats_t * stats )
{
    rocketnet_assert( client );
    rocketnet_assert( stats );

    if ( !client || !stats )
        return false;

    // Returns false when not connected, leaving stats untouched — and a
    // stage 1 client can never be connected.
    if ( client->state != ROCKETNET_CLIENT_STATE_CONNECTED )
        return false;

    memset( stats, 0, sizeof( rocketnet_connection_stats_t ) );
    return true;
}

// ===============================================================
// SERVER
// ===============================================================

struct rocketnet_flow_slot_t
{
    bool active;
    int channel;
};

struct rocketnet_server_t
{
    void * context;
    rocketnet_server_config_t config;
    rocketnet_address_t server_address;
    rocketnet_address_t bind_address;
    rocketnet_flow_slot_t * flows;
    int num_flows;
};

static bool rocketnet_server_flow_valid( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id )
{
    return flow_id < (rocketnet_flow_id_t) server->config.max_flows && server->flows[flow_id].active;
}

struct rocketnet_server_t * rocketnet_server_create( void * context, const struct rocketnet_server_config_t * config )
{
    rocketnet_assert( config );

    if ( !config )
    {
        rocketnet_error( "server create failed: config is NULL" );
        return NULL;
    }

    rocketnet_address_t server_address;
    if ( rocketnet_address_parse( &server_address, config->server_address ) != ROCKETNET_OK )
    {
        rocketnet_error( "server create failed: could not parse server address: %s", config->server_address );
        return NULL;
    }

    rocketnet_address_t bind_address;
    if ( rocketnet_address_parse( &bind_address, config->bind_address ) != ROCKETNET_OK )
    {
        rocketnet_error( "server create failed: could not parse bind address: %s", config->bind_address );
        return NULL;
    }

    if ( !rocketnet_validate_connection_config( &config->connection ) )
    {
        rocketnet_error( "server create failed: invalid connection config" );
        return NULL;
    }

    if ( config->max_connections < 1 )
    {
        rocketnet_error( "server create failed: invalid max_connections: %d", config->max_connections );
        return NULL;
    }

    if ( config->max_flows < 0 )
    {
        rocketnet_error( "server create failed: invalid max_flows: %d", config->max_flows );
        return NULL;
    }

    if ( config->pool_bytes <= 0 )
    {
        rocketnet_error( "server create failed: invalid pool_bytes: %d", config->pool_bytes );
        return NULL;
    }

    rocketnet_server_t * server = (rocketnet_server_t*) rocketnet_malloc( context, sizeof( rocketnet_server_t ) );
    if ( !server )
    {
        rocketnet_error( "server create failed: out of memory" );
        return NULL;
    }

    memset( server, 0, sizeof( rocketnet_server_t ) );

    server->context = context;
    server->config = *config;
    server->server_address = server_address;
    server->bind_address = bind_address;

    if ( config->max_flows > 0 )
    {
        server->flows = (rocketnet_flow_slot_t*) rocketnet_malloc( context, sizeof( rocketnet_flow_slot_t ) * config->max_flows );
        if ( !server->flows )
        {
            rocketnet_error( "server create failed: out of memory" );
            rocketnet_free( context, server );
            return NULL;
        }
        memset( server->flows, 0, sizeof( rocketnet_flow_slot_t ) * config->max_flows );
    }

    // STUB (stage 2): no socket is bound and no kernel-bypass path attaches
    // yet. The IO backends are the substance of the later stages; per the
    // header's note, nothing here will ever reconfigure the machine to get
    // them.

    return server;
}

void rocketnet_server_destroy( struct rocketnet_server_t * server )
{
    if ( !server )
        return;

    rocketnet_server_disconnect_all( server );

    if ( server->flows )
    {
        rocketnet_free( server->context, server->flows );
    }

    rocketnet_free( server->context, server );
}

void rocketnet_server_update( struct rocketnet_server_t * server )
{
    rocketnet_assert( server );

    if ( !server )
        return;

    // STUB (stage 2/3): receive, dispatch, acknowledge, retransmit, age out
    // timeouts, fire connect and disconnect callbacks. The handshake that
    // admits connections is stage 3 and follows netcode — sealed connect
    // token, challenge/response, MUTUAL protocol id check — not flow's
    // direct path. Until then there is nothing to pump.
}

void rocketnet_server_flush( struct rocketnet_server_t * server )
{
    rocketnet_assert( server );

    if ( !server )
        return;

    // STUB (stage 2): nothing can be queued yet.
}

uint16_t rocketnet_server_port( struct rocketnet_server_t * server )
{
    rocketnet_assert( server );

    if ( !server )
        return 0;

    // STUB (stage 2): once a socket binds this reports the actual bound port.
    // Until then, the configured bind port.
    return server->bind_address.port;
}

const struct rocketnet_address_t * rocketnet_server_address( struct rocketnet_server_t * server )
{
    rocketnet_assert( server );

    if ( !server )
        return NULL;

    return &server->server_address;
}

int rocketnet_server_connection_count( struct rocketnet_server_t * server )
{
    rocketnet_assert( server );

    if ( !server )
        return 0;

    // STUB (stage 3): no handshake, no connections.
    return 0;
}

int rocketnet_server_connection_ids( struct rocketnet_server_t * server, rocketnet_connection_id_t * ids, int max_ids, int * num_ids )
{
    rocketnet_assert( server );
    rocketnet_assert( ids );
    rocketnet_assert( num_ids );

    if ( !server || !ids || !num_ids )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( max_ids < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    *num_ids = 0;

    // STUB (stage 3): no connections can exist yet.
    return ROCKETNET_OK;
}

bool rocketnet_server_connection_connected( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id )
{
    rocketnet_assert( server );

    (void) connection_id;

    if ( !server )
        return false;

    // STUB (stage 3): no connections can exist yet.
    return false;
}

const struct rocketnet_address_t * rocketnet_server_connection_address( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id )
{
    rocketnet_assert( server );

    (void) connection_id;

    if ( !server )
        return NULL;

    // STUB (stage 3): no connections can exist yet. (The header does not say
    // what this returns for an unknown id — NULL recorded as the answer.)
    return NULL;
}

int rocketnet_server_disconnect( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id )
{
    rocketnet_assert( server );

    (void) connection_id;

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 3): every id is unknown while connections cannot exist.
    return ROCKETNET_ERROR_UNKNOWN_CONNECTION;
}

void rocketnet_server_disconnect_all( struct rocketnet_server_t * server )
{
    rocketnet_assert( server );

    if ( !server )
        return;

    // STUB (stage 3): nothing to disconnect yet.
}

// ---------------------------------------------------------------
// Server send: claim, write, commit
// ---------------------------------------------------------------

int rocketnet_server_claim( struct rocketnet_server_t * server, int bytes, struct rocketnet_buffer_t * buffer )
{
    rocketnet_assert( server );
    rocketnet_assert( buffer );

    if ( !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // A failed claim leaves the caller's buffer zeroed rather than half-filled.
    memset( buffer, 0, sizeof( rocketnet_buffer_t ) );

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes <= 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES )
        return ROCKETNET_ERROR_PAYLOAD_TOO_LARGE;

    // STUB (stage 2): the pool with slot generations is the first real
    // transport piece. No pool exists yet; a pool of zero bytes is
    // permanently exhausted, and the documented caller response — retry next
    // tick, or send less — is exactly right for a stub too.
    return ROCKETNET_ERROR_POOL_EXHAUSTED;
}

int rocketnet_server_commit( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id, int channel, const struct rocketnet_buffer_t * buffer, int bytes )
{
    rocketnet_assert( server );
    rocketnet_assert( buffer );

    (void) connection_id;

    if ( !server || !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes < 0 || bytes > buffer->bytes )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( channel < 0 || channel >= server->config.connection.num_channels )
        return ROCKETNET_ERROR_UNKNOWN_CHANNEL;

    // STUB (stage 3): no connection can exist, so every id is unknown.
    return ROCKETNET_ERROR_UNKNOWN_CONNECTION;
}

int rocketnet_server_commit_batch( struct rocketnet_server_t * server, struct rocketnet_send_t * sends, int num_sends )
{
    rocketnet_assert( server );

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_sends < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_sends == 0 )
        return ROCKETNET_OK;

    if ( !sends )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // The call is well-formed: fill each entry's result and return OK, per
    // the header, even though every entry fails in stage 1.
    for ( int i = 0; i < num_sends; i++ )
    {
        sends[i].result = rocketnet_server_commit( server, sends[i].connection_id, sends[i].channel, &sends[i].buffer, sends[i].bytes );
    }

    return ROCKETNET_OK;
}

int rocketnet_server_commit_many( struct rocketnet_server_t * server,
                                  const rocketnet_connection_id_t * connection_ids,
                                  int num_connections,
                                  int channel,
                                  const struct rocketnet_buffer_t * buffer,
                                  int bytes )
{
    rocketnet_assert( server );
    rocketnet_assert( buffer );

    if ( !server || !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_connections < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_connections > 0 && !connection_ids )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes < 0 || bytes > buffer->bytes )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( channel < 0 || channel >= server->config.connection.num_channels )
        return ROCKETNET_ERROR_UNKNOWN_CHANNEL;

    // STUB (stage 2): commit_many consumes the buffer whatever happens, and
    // with no pool no handle can be live — stale is the documented answer.
    return ROCKETNET_ERROR_STALE_BUFFER;
}

int rocketnet_server_abandon( struct rocketnet_server_t * server, const struct rocketnet_buffer_t * buffer )
{
    rocketnet_assert( server );
    rocketnet_assert( buffer );

    if ( !server || !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 2): no pool, no live claims, every handle is stale.
    return ROCKETNET_ERROR_STALE_BUFFER;
}

int rocketnet_server_send( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id, int channel, const uint8_t * payload, int bytes )
{
    rocketnet_assert( server );
    rocketnet_assert( payload );

    (void) connection_id;

    if ( !server || !payload )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes <= 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( channel < 0 || channel >= server->config.connection.num_channels )
        return ROCKETNET_ERROR_UNKNOWN_CHANNEL;

    // STUB (stage 3): no connection can exist, so every id is unknown.
    return ROCKETNET_ERROR_UNKNOWN_CONNECTION;
}

// ---------------------------------------------------------------
// Flows
//
// The flow REGISTRY is real in stage 1 — create, destroy, channel binding,
// capacity — because the server example needs a live flow id to run.
// Subscriber sets are not: no connection can exist to subscribe. Subscribe
// paths validate the flow for real and then answer UNKNOWN_CONNECTION,
// which is the truth.
// ---------------------------------------------------------------

rocketnet_flow_id_t rocketnet_server_flow_create( struct rocketnet_server_t * server, int channel )
{
    rocketnet_assert( server );

    if ( !server )
        return ROCKETNET_INVALID_FLOW_ID;

    if ( channel < 0 || channel >= server->config.connection.num_channels )
    {
        // The header documents INVALID_FLOW_ID only for max_flows exhaustion;
        // a bad channel gets the same return plus a logged error. Recorded as
        // underspecified.
        rocketnet_error( "flow create failed: invalid channel: %d", channel );
        return ROCKETNET_INVALID_FLOW_ID;
    }

    for ( int i = 0; i < server->config.max_flows; i++ )
    {
        if ( !server->flows[i].active )
        {
            server->flows[i].active = true;
            server->flows[i].channel = channel;
            server->num_flows++;
            return (rocketnet_flow_id_t) i;
        }
    }

    return ROCKETNET_INVALID_FLOW_ID;
}

int rocketnet_server_flow_destroy( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id )
{
    rocketnet_assert( server );

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    server->flows[flow_id].active = false;
    server->flows[flow_id].channel = 0;
    server->num_flows--;

    return ROCKETNET_OK;
}

int rocketnet_server_flow_subscribe( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id, rocketnet_connection_id_t connection_id )
{
    rocketnet_assert( server );

    (void) connection_id;

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    // STUB (stage 3): subscription is an operation the server performs on a
    // live connection, and no connection can exist yet.
    return ROCKETNET_ERROR_UNKNOWN_CONNECTION;
}

int rocketnet_server_flow_subscribe_many( struct rocketnet_server_t * server,
                                          rocketnet_flow_id_t flow_id,
                                          const rocketnet_connection_id_t * connection_ids,
                                          int num_connections )
{
    rocketnet_assert( server );

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_connections < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_connections > 0 && !connection_ids )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    if ( num_connections == 0 )
        return ROCKETNET_OK;

    // STUB (stage 3): no connection can exist yet.
    return ROCKETNET_ERROR_UNKNOWN_CONNECTION;
}

int rocketnet_server_flow_unsubscribe( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id, rocketnet_connection_id_t connection_id )
{
    rocketnet_assert( server );

    (void) connection_id;

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    // STUB (stage 3): no connection can exist yet.
    return ROCKETNET_ERROR_UNKNOWN_CONNECTION;
}

int rocketnet_server_flow_unsubscribe_many( struct rocketnet_server_t * server,
                                            rocketnet_flow_id_t flow_id,
                                            const rocketnet_connection_id_t * connection_ids,
                                            int num_connections )
{
    rocketnet_assert( server );

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_connections < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_connections > 0 && !connection_ids )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    if ( num_connections == 0 )
        return ROCKETNET_OK;

    // STUB (stage 3): no connection can exist yet.
    return ROCKETNET_ERROR_UNKNOWN_CONNECTION;
}

int rocketnet_server_flow_subscriber_count( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id )
{
    rocketnet_assert( server );

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    // A live flow with no subscribers is a normal state of the world.
    return 0;
}

int rocketnet_server_flow_subscribers( struct rocketnet_server_t * server,
                                       rocketnet_flow_id_t flow_id,
                                       rocketnet_connection_id_t * ids,
                                       int max_ids,
                                       int * num_ids )
{
    rocketnet_assert( server );
    rocketnet_assert( ids );
    rocketnet_assert( num_ids );

    if ( !server || !ids || !num_ids )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( max_ids < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    *num_ids = 0;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    return ROCKETNET_OK;
}

int rocketnet_server_commit_flow( struct rocketnet_server_t * server,
                                  rocketnet_flow_id_t flow_id,
                                  const struct rocketnet_buffer_t * buffer,
                                  int bytes )
{
    rocketnet_assert( server );
    rocketnet_assert( buffer );

    if ( !server || !buffer )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( bytes < 0 || bytes > buffer->bytes )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( !rocketnet_server_flow_valid( server, flow_id ) )
        return ROCKETNET_ERROR_UNKNOWN_FLOW;

    // STUB (stage 2): once the pool exists, committing to a flow with no
    // subscribers consumes the buffer and returns OK per the header. With no
    // pool no handle can be live, so the honest answer is stale.
    return ROCKETNET_ERROR_STALE_BUFFER;
}

// ---------------------------------------------------------------
// Server receive and statistics
// ---------------------------------------------------------------

int rocketnet_server_receive( struct rocketnet_server_t * server, struct rocketnet_payload_t * payloads, int max_payloads, int * num_payloads )
{
    rocketnet_assert( server );
    rocketnet_assert( payloads );
    rocketnet_assert( num_payloads );

    if ( !server || !payloads || !num_payloads )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    *num_payloads = 0;

    if ( max_payloads < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // Real check per the header: both receive paths configured is an error.
    if ( server->config.receive_callback != NULL )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 2): nothing can arrive without a transport.
    return ROCKETNET_OK;
}

int rocketnet_server_release( struct rocketnet_server_t * server, const struct rocketnet_payload_t * payloads, int num_payloads )
{
    rocketnet_assert( server );

    if ( !server )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_payloads < 0 )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    if ( num_payloads == 0 )
        return ROCKETNET_OK;

    if ( !payloads )
        return ROCKETNET_ERROR_INVALID_ARGUMENT;

    // STUB (stage 2): no payload can be live.
    return ROCKETNET_ERROR_STALE_BUFFER;
}

bool rocketnet_server_stats( struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats )
{
    rocketnet_assert( server );
    rocketnet_assert( stats );

    if ( !server || !stats )
        return false;

    memset( stats, 0, sizeof( rocketnet_server_stats_t ) );

    stats->num_flows = server->num_flows;

    // STUB (stage 2): the header says AUTO resolves to a concrete backend
    // here. No backend runs yet, so there is nothing to resolve to and the
    // configured value is reported as-is. The stage that lands the first
    // backend makes this concrete.
    stats->backend = server->config.backend;

    return true;
}

bool rocketnet_server_connection_stats( struct rocketnet_server_t * server,
                                        rocketnet_connection_id_t connection_id,
                                        struct rocketnet_connection_stats_t * stats )
{
    rocketnet_assert( server );
    rocketnet_assert( stats );

    (void) connection_id;

    if ( !server || !stats )
        return false;

    // False for an unknown id, leaving stats untouched — and every id is
    // unknown while connections cannot exist.
    return false;
}

#if defined( _MSC_VER )
#pragma warning(pop)
#endif
