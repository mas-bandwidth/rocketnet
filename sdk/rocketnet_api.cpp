/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public client and server (rocketnet.h) over the SDK's layers: a public
// client is a session object holding one message client per connection, and
// a public server a lifecycle object holding one message server per run, so
// every call here is a guard, a state check and one call down. The configs are resolved here: the public
// struct is split into the transport config and the message config the
// layers take, the message fields and the send budget take their environment
// overrides here, and the transport fields take theirs at the transport's
// create, as they always have.

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_message_object.h"
#include "rocketnet_send_budget.h"

#include <sodium.h>

#include <limits.h>
#include <string.h>

// ---------------------------------------------------------------
//                     the contracts this file keeps
// ---------------------------------------------------------------

static_assert( ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_DENIED == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_DISCONNECTED == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTED == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED, "client state" );

static_assert( sizeof( struct rocketnet_receipt_t ) == sizeof( struct rocketnet_message_receipt_t ) &&
               offsetof( struct rocketnet_receipt_t, serial ) == offsetof( struct rocketnet_message_receipt_t, serial ) &&
               offsetof( struct rocketnet_receipt_t, status ) == offsetof( struct rocketnet_message_receipt_t, status ), "the public receipt is the message receipt" );

static_assert( ROCKETNET_DEFAULT_CLIENT_SEND_KBPS <= ROCKETNET_SEND_BUDGET_MAX_KBPS && ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS <= ROCKETNET_SEND_BUDGET_MAX_KBPS, "the default budgets are valid budgets" );

uint32_t rocketnet_version( void )
{
    return ROCKETNET_VERSION;
}

// ---------------------------------------------------------------
//                          the config split
// ---------------------------------------------------------------

// every message field, one row each, in the order of rocketnet_message_config_t

#define RN_PUBLIC_MESSAGE_FIELDS( X ) \
    X( num_channels ) X( window_messages ) X( receive_ring_bytes ) X( max_message_bytes ) X( send_buffer_bytes ) \
    X( send_queue_messages ) X( packets_in_flight ) X( max_packets_per_update ) X( unreliable_deliveries )      \
    X( sequenced_holds ) X( receipt_ring ) X( rto_initial_ms ) X( rto_min_ms ) X( rto_max_ms ) X( reliable_timeout_ms )

// the send budget fields: one int each, read under the endpoint's env name.
// 0 or less removes the budget; the ceiling is the budget's own.

static const struct rocketnet_config_field_t rn_client_budget_fields[] =
{
    { "send_kbps", ROCKETNET_CONFIG_KIND_INT, 0, (int) sizeof(int), (int64_t) INT_MIN, (int64_t) ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 },
};

static const struct rocketnet_config_field_t rn_server_budget_fields[] =
{
    { "per_client_send_kbps", ROCKETNET_CONFIG_KIND_INT, 0, (int) sizeof(int), (int64_t) INT_MIN, (int64_t) ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 },
};

const struct rocketnet_config_field_t * rocketnet_client_budget_config_fields( int * num_fields )
{
    *num_fields = 1;
    return rn_client_budget_fields;
}

const struct rocketnet_config_field_t * rocketnet_server_budget_config_fields( int * num_fields )
{
    *num_fields = 1;
    return rn_server_budget_fields;
}

static uint32_t rn_budget_kbps( int kbps )
{
    return kbps <= 0 ? (uint32_t) ROCKETNET_SEND_BUDGET_MAX_KBPS : (uint32_t) kbps;
}

// a NULL or empty string is the default; a longer one than the layer holds
// is refused, loudly, never cut

static bool rn_copy_config_string( char * dest, size_t dest_size, const char * source, const char * fallback, const char * endpoint, const char * field )
{
    if ( !source || source[0] == '\0' )
        source = fallback;
    const size_t length = strlen( source );
    if ( length + 1 > dest_size )
    {
        rocketnet_error( "%s config refused: %s is %d characters, the most is %d", endpoint, field, (int) length, (int) dest_size - 1 );
        return false;
    }
    memcpy( dest, source, length + 1 );
    return true;
}

static bool rn_apply_message_and_budget_env( const char * endpoint, struct rocketnet_message_config_t * message, int * kbps, const struct rocketnet_config_field_t * budget_fields )
{
    int num_message_fields = 0;
    const struct rocketnet_config_field_t * message_fields = rocketnet_message_config_fields( &num_message_fields );
    if ( !rocketnet_config_apply_env( endpoint, message, message_fields, num_message_fields ) )
        return false;
    if ( !rocketnet_config_apply_env( endpoint, kbps, budget_fields, 1 ) )
        return false;
    return rocketnet_config_validate( endpoint, kbps, budget_fields, 1 );
}

static bool rn_client_split( const struct rocketnet_client_config_t * config, bool read_env,
                             struct rocketnet_client_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    struct rocketnet_client_config_t defaults;
    if ( !config )
    {
        rocketnet_client_default_config_sized( &defaults, sizeof( defaults ) );
        config = &defaults;
    }

    rocketnet_client_transport_default_config_current( transport );
    memcpy( transport->filter_value, config->filter_value, sizeof( transport->filter_value ) );
    transport->max_payload_bytes = config->max_payload_bytes;
    transport->num_reassembly_slots = config->num_reassembly_slots;
    if ( !rn_copy_config_string( transport->driver, sizeof( transport->driver ), config->transport, "auto", "client", "transport" ) )
        return false;
    transport->state_change_callback = config->state_change_callback;
    transport->time_callback = config->time_callback;
    transport->step_driven = config->step_driven;
    transport->simulated_loss_percent = config->simulated_loss_percent;
    transport->simulated_latency_ms = config->simulated_latency_ms;
    transport->simulated_jitter_ms = config->simulated_jitter_ms;
    transport->simulated_duplicate_percent = config->simulated_duplicate_percent;

    rocketnet_message_default_config( message );
#define RN_COPY( name ) message->name = config->name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY

    int send_kbps = config->send_kbps;
    if ( read_env && !rn_apply_message_and_budget_env( "client", message, &send_kbps, rn_client_budget_fields ) )
        return false;
    if ( !read_env && !rocketnet_config_validate( "client", &send_kbps, rn_client_budget_fields, 1 ) )
        return false;
    *kbps = rn_budget_kbps( send_kbps );
    return true;
}

static bool rn_server_split( const struct rocketnet_server_config_t * config, bool read_env,
                             struct rocketnet_server_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    struct rocketnet_server_config_t defaults;
    if ( !config )
    {
        rocketnet_server_default_config_sized( &defaults, sizeof( defaults ) );
        config = &defaults;
    }

    rocketnet_server_transport_default_config_current( transport );
    transport->protocol_id = config->protocol_id;
    transport->max_clients = config->max_clients;
    memcpy( transport->filter_value, config->filter_value, sizeof( transport->filter_value ) );
    memcpy( transport->backend_public_key, config->backend_public_key, sizeof( transport->backend_public_key ) );
    memcpy( transport->server_public_key, config->server_public_key, sizeof( transport->server_public_key ) );
    memcpy( transport->server_secret_key, config->server_secret_key, sizeof( transport->server_secret_key ) );
    transport->num_xdp_queues = config->num_xdp_queues;
    transport->num_os_cpus = config->num_os_cpus;
    transport->max_payload_bytes = config->max_payload_bytes;
    transport->num_reassembly_slots = config->num_reassembly_slots;
    transport->max_timeout_seconds = config->max_timeout_seconds;
    transport->handshake_deadline_seconds = config->handshake_deadline_seconds;
    transport->simulated_loss_percent = config->simulated_loss_percent;
    transport->simulated_latency_ms = config->simulated_latency_ms;
    transport->simulated_jitter_ms = config->simulated_jitter_ms;
    transport->simulated_duplicate_percent = config->simulated_duplicate_percent;
    if ( !rn_copy_config_string( transport->driver, sizeof( transport->driver ), config->transport, "auto", "server", "transport" ) )
        return false;
    if ( !rn_copy_config_string( transport->public_address, sizeof( transport->public_address ), config->public_address, "", "server", "public_address" ) )
        return false;

    rocketnet_message_default_config( message );
#define RN_COPY( name ) message->name = config->name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY

    // max_clients sizes the message server as well as the transport, so its
    // env override applies here, before either is made: the transport's
    // create reads the same variable again and agrees. every other transport
    // field takes its override at the transport's create, as before.
    if ( read_env )
    {
        int num_fields = 0;
        const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
        for ( int i = 0; i < num_fields; i++ )
            if ( strcmp( fields[i].name, "max_clients" ) == 0 && !rocketnet_config_apply_env( "server", transport, &fields[i], 1 ) )
                return false;
    }

    int send_kbps = config->per_client_send_kbps;
    if ( read_env && !rn_apply_message_and_budget_env( "server", message, &send_kbps, rn_server_budget_fields ) )
        return false;
    if ( !read_env && !rocketnet_config_validate( "server", &send_kbps, rn_server_budget_fields, 1 ) )
        return false;
    *kbps = rn_budget_kbps( send_kbps );
    return true;
}

bool rocketnet_client_config_resolve( const struct rocketnet_client_config_t * config, bool read_env,
                                     struct rocketnet_client_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    return rn_client_split( config, read_env, transport, message, kbps );
}

bool rocketnet_server_config_resolve( const struct rocketnet_server_config_t * config, bool read_env,
                                     struct rocketnet_server_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    return rn_server_split( config, read_env, transport, message, kbps );
}

// ---------------------------------------------------------------
//                              client
// ---------------------------------------------------------------

// The public client is a session object (tla/ClientSession.tla): it holds
// the resolved config, one memory block made at create, the lifetime
// counters and the last disconnect reason (the model's `kept` and `reason`),
// and, between rocketnet_client_connect and rocketnet_client_disconnect, one
// connection: a message client over its own client transport, created in the
// block by connect (the model's Connect) and destroyed by disconnect (End,
// then Reset). Every per-connection field lives in that connection, so the
// next connect starts from a block in which nothing of the last connection
// remains: the reset is the connection's own destroy, which runs
// rocketnet_client_connection_reset on the transport and the message object
// (sdk/rocketnet_connection_reset.h), and the new connection's create.
//
// The socket and the I/O thread are per connection: the token decides the
// server's address family, and the socket binds to that family after the
// token parses, so a client that connects to an IPv4 server and then an IPv6
// one needs a socket of each.

struct rocketnet_client_t
{
    void * context;
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps;
    int owns_block;                                     // 1: this struct and the block are one rocketnet_malloc
    uint8_t * block;                                    // the connection's memory, ROCKETNET_ALLOCATION_ALIGNMENT aligned
    uint64_t block_bytes;
    struct rocketnet_message_client_t * connection;     // NULL while DISCONNECTED or after a refused connect
    int refused_state;                                  // the error state a refused connect left, 0 otherwise
    int last_reason;                                    // the reason the last connection ended with, kept until the next one is established
    uint64_t lifetime[ROCKETNET_CLIENT_COUNTER_COUNT];  // the counters of every ended connection
};

static inline uint64_t rn_round_up_alignment( uint64_t bytes )
{
    return ( bytes + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) / ROCKETNET_ALLOCATION_ALIGNMENT * ROCKETNET_ALLOCATION_ALIGNMENT;
}

static const uint64_t rn_client_header_bytes = rn_round_up_alignment( sizeof( struct rocketnet_client_t ) );

struct rocketnet_message_client_t * rocketnet_client_message_client( struct rocketnet_client_t * client )
{
    return client ? client->connection : NULL;
}

struct rocketnet_client_t * rocketnet_client_adopt( void * context, struct rocketnet_message_client_t * connection )
{
    if ( !rocketnet_api_null_guard( connection ) )
        return NULL;
    struct rocketnet_client_t * client = (struct rocketnet_client_t *) rocketnet_malloc( context, sizeof( struct rocketnet_client_t ) );
    if ( !client )
        return NULL;
    memset( client, 0, sizeof( *client ) );
    client->context = context;
    rocketnet_client_transport_default_config( &client->transport );
    rocketnet_message_default_config( &client->message );
    client->owns_block = 1;
    client->connection = connection;
    return client;
}

static void rn_client_default_config_current( struct rocketnet_client_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    memset( config, 0, sizeof( *config ) );
    struct rocketnet_client_transport_config_t transport;
    rocketnet_client_transport_default_config_current( &transport );
    memcpy( config->filter_value, transport.filter_value, sizeof( config->filter_value ) );
    config->max_payload_bytes = transport.max_payload_bytes;
    config->num_reassembly_slots = transport.num_reassembly_slots;
    config->transport = NULL;
    config->state_change_callback = transport.state_change_callback;
    config->time_callback = transport.time_callback;
    config->step_driven = transport.step_driven;
    config->simulated_loss_percent = transport.simulated_loss_percent;
    config->simulated_latency_ms = transport.simulated_latency_ms;
    config->simulated_jitter_ms = transport.simulated_jitter_ms;
    config->simulated_duplicate_percent = transport.simulated_duplicate_percent;
    config->send_kbps = ROCKETNET_DEFAULT_CLIENT_SEND_KBPS;
    struct rocketnet_message_config_t message;
    rocketnet_message_default_config( &message );
#define RN_COPY( name ) config->name = message.name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY
}

void rocketnet_client_default_config( struct rocketnet_client_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_client_config_t defaults;
    rn_client_default_config_current( &defaults );
    memcpy( config, &defaults, ROCKETNET_CLIENT_CONFIG_BYTES_1 );
}

static const size_t rn_client_config_known_bytes[] = { ROCKETNET_CLIENT_CONFIG_KNOWN_BYTES };
static const size_t rn_server_config_known_bytes[] = { ROCKETNET_SERVER_CONFIG_KNOWN_BYTES };

static bool rn_public_config_size_known( const char * endpoint, const char * type_name, size_t bytes,
                                         const size_t * known, int known_count, size_t current )
{
    for ( int i = 0; i < known_count; i++ )
        if ( bytes == known[i] )
            return true;
    rocketnet_error( "%s config size refused: %d bytes for %s; current size is %d",
                     endpoint, (int) bytes, type_name, (int) current );
    return false;
}

#define RN_CLIENT_CONFIG_SIZE_KNOWN( bytes ) rn_public_config_size_known( "client", "rocketnet_client_config_t", (bytes), rn_client_config_known_bytes, (int) ( sizeof(rn_client_config_known_bytes) / sizeof(rn_client_config_known_bytes[0]) ), sizeof(struct rocketnet_client_config_t) )
#define RN_SERVER_CONFIG_SIZE_KNOWN( bytes ) rn_public_config_size_known( "server", "rocketnet_server_config_t", (bytes), rn_server_config_known_bytes, (int) ( sizeof(rn_server_config_known_bytes) / sizeof(rn_server_config_known_bytes[0]) ), sizeof(struct rocketnet_server_config_t) )

int rocketnet_client_default_config_sized( struct rocketnet_client_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;
    if ( !RN_CLIENT_CONFIG_SIZE_KNOWN( config_bytes ) )
        return 0;
    struct rocketnet_client_config_t defaults;
    rn_client_default_config_current( &defaults );
    memcpy( config, &defaults, config_bytes );
    return 1;
}

static const rocketnet_client_config_t * rn_client_config_read( rocketnet_client_config_t * current,
                                                                    const rocketnet_client_config_t * config,
                                                                    size_t config_bytes )
{
    if ( !config )
        return NULL;
    rn_client_default_config_current( current );
    memcpy( current, config, config_bytes );
    return current;
}

static int rn_client_validate_config_current( const struct rocketnet_client_config_t * config )

{
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_client_split( config, false, &transport, &message, &kbps ) )
        return 0;
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "client", &transport, fields, num_fields ) )
        return 0;
    return rocketnet_message_validate_config( &message );
}

int rocketnet_client_validate_config( const struct rocketnet_client_config_t * config )
{
    struct rocketnet_client_config_t current;
    return rn_client_validate_config_current( rn_client_config_read( &current, config, ROCKETNET_CLIENT_CONFIG_BYTES_1 ) );
}

int rocketnet_client_validate_config_sized( const struct rocketnet_client_config_t * config, size_t config_bytes )
{
    if ( config && !RN_CLIENT_CONFIG_SIZE_KNOWN( config_bytes ) )
        return 0;
    struct rocketnet_client_config_t current;
    return rn_client_validate_config_current( rn_client_config_read( &current, config, config_bytes ) );
}

static uint64_t rn_client_required_memory_current( const struct rocketnet_client_config_t * config )

{
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_client_split( config, true, &transport, &message, &kbps ) )
        return 0;
    const uint64_t connection_bytes = rocketnet_message_client_required_memory( &message );
    return connection_bytes == 0 ? 0 : rn_client_header_bytes + connection_bytes;
}

uint64_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config )
{
    struct rocketnet_client_config_t current;
    return rn_client_required_memory_current( rn_client_config_read( &current, config, ROCKETNET_CLIENT_CONFIG_BYTES_1 ) );
}

uint64_t rocketnet_client_required_memory_sized( const struct rocketnet_client_config_t * config, size_t config_bytes )
{
    if ( config && !RN_CLIENT_CONFIG_SIZE_KNOWN( config_bytes ) )
        return 0;
    struct rocketnet_client_config_t current;
    return rn_client_required_memory_current( rn_client_config_read( &current, config, config_bytes ) );
}

static struct rocketnet_client_t * rn_client_create_current( void * context, const struct rocketnet_client_config_t * config, void * memory, uint64_t memory_bytes )
{
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_client_split( config, true, &transport, &message, &kbps ) )
        return NULL;
    const uint64_t connection_bytes = rocketnet_message_client_required_memory( &message );
    if ( connection_bytes == 0 )
        return NULL;
    const uint64_t total = rn_client_header_bytes + connection_bytes;
    uint8_t * base = (uint8_t *) memory;
    if ( base )
    {
        if ( memory_bytes < total || ( (uintptr_t) base % ROCKETNET_ALLOCATION_ALIGNMENT ) != 0 )
        {
            rocketnet_error( "client create refused: the memory block is short or not %d-aligned", ROCKETNET_ALLOCATION_ALIGNMENT );
            return NULL;
        }
    }
    else
    {
        base = (uint8_t *) rocketnet_malloc( context, (size_t) total );
        if ( !base )
            return NULL;
    }
    struct rocketnet_client_t * client = (struct rocketnet_client_t *) base;
    memset( client, 0, sizeof( *client ) );
    client->context = context;
    client->transport = transport;
    client->message = message;
    client->kbps = kbps;
    client->owns_block = memory ? 0 : 1;
    client->block = base + rn_client_header_bytes;
    client->block_bytes = connection_bytes;
    client->last_reason = ROCKETNET_MESSAGE_ERROR_NONE;
    return client;
}

struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, void * memory, uint64_t memory_bytes )
{
    struct rocketnet_client_config_t current;
    return rn_client_create_current( context,
        rn_client_config_read( &current, config, ROCKETNET_CLIENT_CONFIG_BYTES_1 ), memory, memory_bytes );
}

struct rocketnet_client_t * rocketnet_client_create_sized( void * context, const struct rocketnet_client_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes )
{
    if ( config && !RN_CLIENT_CONFIG_SIZE_KNOWN( config_bytes ) )
        return NULL;
    if ( !config || config_bytes == sizeof( struct rocketnet_client_config_t ) )
        return rn_client_create_current( context, config, memory, memory_bytes );
    struct rocketnet_client_config_t current;
    rn_client_default_config_current( &current );
    memcpy( &current, config, config_bytes );
    return rn_client_create_current( context, &current, memory, memory_bytes );
}

static uint64_t rn_client_connection_counter( struct rocketnet_message_client_t * message_client, int counter );

// End then Reset (tla/ClientSession.tla): the connection's counters move into
// the lifetime counters, its reason is kept, and its destroy -- which sends
// the redundant disconnects from a live state and runs the connection reset
// -- leaves the block holding nothing of it.
static void rn_client_end_connection( struct rocketnet_client_t * client )
{
    if ( !client->connection )
        return;

    // From a handshake state or CONNECTED the message connection ends first:
    // its link performs the transport disconnect synchronously (the redundant
    // disconnect packets), preserving that transition's callback order.
    if ( rocketnet_message_client_state( client->connection ) > ROCKETNET_CLIENT_STATE_DISCONNECTED )
        rn_message_object_kick( &client->connection->object, 0, ROCKETNET_MESSAGE_ERROR_NONE );
    for ( int i = 0; i < ROCKETNET_CLIENT_COUNTER_COUNT; i++ )
        client->lifetime[i] += rn_client_connection_counter( client->connection, i );
    const int reason = rocketnet_message_client_disconnect_reason( client->connection );
    if ( reason != ROCKETNET_MESSAGE_ERROR_NONE || rocketnet_message_client_state( client->connection ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        client->last_reason = reason;
    rocketnet_message_client_destroy( client->connection );
    client->connection = NULL;
}

static int rn_client_state( const struct rocketnet_client_t * client )
{
    if ( client->connection )
        return rocketnet_message_client_state( client->connection );
    return client->refused_state;
}

enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const uint8_t * connect_token, double time )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( connect_token ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    (void) time;    // the connection's clock starts at its first update, as create's always has
    if ( rn_client_state( client ) != ROCKETNET_CLIENT_STATE_DISCONNECTED )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "client connect refused: the client is not disconnected" );
        return ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED;
    }
    int status = ROCKETNET_STATUS_OK;
    client->connection = rocketnet_message_client_create_ex( client->context, connect_token, &client->transport, &client->message,
                                                              client->block, client->block_bytes, client->kbps, &status );
    if ( client->connection )
        return ROCKETNET_STATUS_OK;
    if ( status == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN )
        client->refused_state = ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN;
    else if ( status == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED )
        client->refused_state = ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED;
    return status == ROCKETNET_STATUS_OK ? ROCKETNET_STATUS_INVALID_ARGUMENT : (enum rocketnet_status_t) status;
}

void rocketnet_client_destroy( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rn_client_end_connection( client );
    if ( client->owns_block )
    {
        void * context = client->context;
        rocketnet_free( context, client );
    }
    else
    {
        memset( client, 0, sizeof( *client ) );
    }
}

int rocketnet_client_update( struct rocketnet_client_t * client, double time )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_MESSAGE_OK;
    if ( !client->connection )
        return ROCKETNET_MESSAGE_OK;
    const int result = rocketnet_message_client_update( client->connection, time );
    if ( rocketnet_message_client_state( client->connection ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        client->last_reason = ROCKETNET_MESSAGE_ERROR_NONE;
    return result;
}

void rocketnet_client_send_messages( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    if ( !client->connection )
        return;
    rocketnet_message_client_send_packets( client->connection );
}

void rocketnet_client_disconnect( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;

    // the disconnect packets from a live state, then the connection is torn
    // down and the client is DISCONNECTED again (rn_client_end_connection)
    rn_client_end_connection( client );
    client->refused_state = ROCKETNET_CLIENT_STATE_DISCONNECTED;
}

enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_CLIENT_STATE_DISCONNECTED;
    return (enum rocketnet_client_state_t) rn_client_state( client );
}

const char * rocketnet_client_state_name( int state )
{
    return rocketnet_client_transport_state_name( state );
}

int rocketnet_client_disconnect_reason( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_MESSAGE_ERROR_NONE;
    if ( client->connection )
    {
        const int reason = rocketnet_message_client_disconnect_reason( client->connection );
        if ( reason != ROCKETNET_MESSAGE_ERROR_NONE || rocketnet_message_client_state( client->connection ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            return reason;
    }
    return client->last_reason;
}

uint64_t rocketnet_client_index( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !client->connection )
        return 0;
    return (uint64_t) rocketnet_client_transport_client_index( rocketnet_message_client_transport( client->connection ) );
}

const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    if ( !client->connection )
        return NULL;
    return rocketnet_client_transport_driver( rocketnet_message_client_transport( client->connection ) );
}

// Messages flow only while CONNECTED: in every other state a send is refused
// and counted (rocketnet_api_misuse_count).
static struct rocketnet_message_client_t * rn_client_connected( struct rocketnet_client_t * client )
{
    if ( client->connection && rocketnet_message_client_state( client->connection ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        return client->connection;
    rocketnet_api_misuse_record();
    return NULL;
}

uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, int reliability, int bytes )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    struct rocketnet_message_client_t * connection = rn_client_connected( client );
    if ( !connection )
        return NULL;
    return rocketnet_message_client_start( connection, channel, reliability, bytes );
}

uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    struct rocketnet_message_client_t * connection = rn_client_connected( client );
    if ( !connection )
        return 0;
    return rocketnet_message_client_finish( connection, bytes_written );
}

void rocketnet_client_abort_message( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    if ( !client->connection )
        return;
    rocketnet_message_client_abort( client->connection );
}

int rocketnet_client_receive_message( struct rocketnet_client_t * client, int channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( data ) || !rocketnet_api_null_guard( bytes ) )
        return 0;
    if ( !client->connection )
        return 0;
    return rocketnet_message_client_receive( client->connection, channel, data, bytes );
}

void rocketnet_client_release_message( struct rocketnet_client_t * client, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( data ) )
        return;
    if ( !client->connection )
    {
        rocketnet_api_misuse_record();
        return;
    }
    rocketnet_message_client_release( client->connection, data );
}

int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !client->connection )
        return 0;
    return rocketnet_message_client_receipts( client->connection, (struct rocketnet_message_receipt_t *) out, capacity );
}

int rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !client->connection )
        return 0;
    return rocketnet_message_client_set_send_kbps( client->connection, rn_budget_kbps( kbps ) );
}

static uint64_t rn_counter_sum( const struct rocketnet_counters_t * a, const struct rocketnet_counters_t * b, int counter )
{
    return ( a ? rocketnet_counter_value( a, counter ) : 0 ) + ( b ? rocketnet_counter_value( b, counter ) : 0 );
}

static uint64_t rn_client_connection_counter( struct rocketnet_message_client_t * message_client, int counter )
{
    struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( message_client );
    const struct rocketnet_counters_t * transport_counters = rocketnet_client_transport_counters( transport );
    const struct rocketnet_counters_t * message_counters = rocketnet_message_client_counters( message_client );
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( transport, &stats );
    switch ( counter )
    {
        case ROCKETNET_CLIENT_COUNTER_PACKETS_SENT:         return stats.packets_sent;
        case ROCKETNET_CLIENT_COUNTER_PACKETS_RECEIVED:     return stats.packets_received;
        case ROCKETNET_CLIENT_COUNTER_BYTES_SENT:           return stats.bytes_sent;
        case ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED:       return stats.bytes_received;
        case ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED:    return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED );
        case ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT:      return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
        case ROCKETNET_CLIENT_COUNTER_MESSAGES_DUPLICATE:   return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
        case ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED:       return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED );
        case ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
        case ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS:         return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        case ROCKETNET_CLIENT_COUNTER_SEND_DEFERRED:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED );
        case ROCKETNET_CLIENT_COUNTER_MESSAGES_NOT_RELEASED: return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED );
        default:                                            return 0;
    }
}

uint64_t rocketnet_client_counter( const struct rocketnet_client_t * client, enum rocketnet_client_counter_t counter )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( (int) counter < 0 || (int) counter >= ROCKETNET_CLIENT_COUNTER_COUNT )
        return 0;
    return client->lifetime[counter] + ( client->connection ? rn_client_connection_counter( client->connection, (int) counter ) : 0 );
}

double rocketnet_client_stat( const struct rocketnet_client_t * client, enum rocketnet_client_stat_t stat )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0.0;
    if ( !client->connection )
        return 0.0;
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( rocketnet_message_client_transport( client->connection ), &stats );
    switch ( stat )
    {
        case ROCKETNET_CLIENT_STAT_RTT:                         return stats.rtt;
        case ROCKETNET_CLIENT_STAT_JITTER:                      return stats.jitter;
        case ROCKETNET_CLIENT_STAT_PACKET_LOSS:                 return stats.packet_loss;
        case ROCKETNET_CLIENT_STAT_PACKETS_SENT_PER_SECOND:     return stats.packets_sent_per_second;
        case ROCKETNET_CLIENT_STAT_PACKETS_RECEIVED_PER_SECOND: return stats.packets_received_per_second;
        case ROCKETNET_CLIENT_STAT_BANDWIDTH_SENT:              return stats.bandwidth_sent_kbps;
        case ROCKETNET_CLIENT_STAT_BANDWIDTH_RECEIVED:          return stats.bandwidth_received_kbps;
        default:                                                return 0.0;
    }
}

// ---------------------------------------------------------------
//                              server
// ---------------------------------------------------------------

// The public server is a lifecycle object (tla/ServerSession.tla): create
// validates the config and makes the one memory block, binding nothing;
// rocketnet_server_start creates the running server -- a message server over
// its own server transport, bound at the address -- in that block;
// rocketnet_server_stop disconnects every client (the redundant disconnect
// packets, then the per-slot connection reset of
// sdk/rocketnet_connection_reset.h, the model's EndClient) and releases the
// transport, leaving the server stopped and able to start again. The server's
// counters are kept across starts.

struct rocketnet_server_t
{
    void * context;
    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    struct rocketnet_server_transport_config_t transport;   // holds the secret key: wiped at destroy
    struct rocketnet_message_config_t message;
    uint32_t kbps;
    int max_clients;
    int owns_block;
    uint8_t * block;
    uint64_t block_bytes;
    struct rocketnet_message_server_t * running;            // NULL while stopped
    uint64_t lifetime[ROCKETNET_SERVER_COUNTER_COUNT];      // the counters of every earlier run
};

static const uint64_t rn_server_header_bytes = rn_round_up_alignment( sizeof( struct rocketnet_server_t ) );

static inline struct rocketnet_message_server_t * rn_server( const struct rocketnet_server_t * server )
{
    return server->running;
}

static inline struct rocketnet_server_transport_t * rn_server_transport( const struct rocketnet_server_t * server )
{
    return server->running ? rocketnet_message_server_transport( server->running ) : NULL;
}

struct rocketnet_message_server_t * rocketnet_server_message_server( struct rocketnet_server_t * server )
{
    return server ? server->running : NULL;
}

struct rocketnet_server_t * rocketnet_server_adopt( void * context, struct rocketnet_message_server_t * running )
{
    if ( !rocketnet_api_null_guard( running ) )
        return NULL;
    struct rocketnet_server_t * server = (struct rocketnet_server_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_t ) );
    if ( !server )
        return NULL;
    memset( server, 0, sizeof( *server ) );
    server->context = context;
    rocketnet_server_transport_default_config( &server->transport );
    rocketnet_message_default_config( &server->message );
    server->max_clients = ( (const struct rn_message_object_t *) running )->num_connections;
    server->owns_block = 1;
    server->running = running;
    return server;
}

// A destination id with ROCKETNET_FLOW_ID_BIT set is a flow id: reserved,
// refused and counted until flows exist (ROCKETNET_STATUS_FLOW_ID_RESERVED).
static bool rn_server_destination_refused( uint64_t destination_id )
{
    if ( ( destination_id & ROCKETNET_FLOW_ID_BIT ) == 0 )
        return false;
    rocketnet_api_misuse_record();
    rocketnet_error( "server send refused: %s (destination id has the flow bit set)", rocketnet_status_string( ROCKETNET_STATUS_FLOW_ID_RESERVED ) );
    return true;
}

// a slot index for a client index: in range of max_clients while running
static bool rn_server_slot( const struct rocketnet_server_t * server, uint64_t client_index, int * slot )
{
    if ( !server->running || client_index >= (uint64_t) server->max_clients )
        return false;
    *slot = (int) client_index;
    return true;
}

static void rn_server_config_from_transport_current( struct rocketnet_server_config_t * config, const struct rocketnet_server_transport_config_t * transport )
{
    memset( config, 0, sizeof( *config ) );
    config->protocol_id = transport->protocol_id;
    config->max_clients = transport->max_clients;
    memcpy( config->filter_value, transport->filter_value, sizeof( config->filter_value ) );
    memcpy( config->backend_public_key, transport->backend_public_key, sizeof( config->backend_public_key ) );
    memcpy( config->server_public_key, transport->server_public_key, sizeof( config->server_public_key ) );
    memcpy( config->server_secret_key, transport->server_secret_key, sizeof( config->server_secret_key ) );
    config->num_xdp_queues = transport->num_xdp_queues;
    config->num_os_cpus = transport->num_os_cpus;
    config->max_payload_bytes = transport->max_payload_bytes;
    config->num_reassembly_slots = transport->num_reassembly_slots;
    config->max_timeout_seconds = transport->max_timeout_seconds;
    config->handshake_deadline_seconds = transport->handshake_deadline_seconds;
    config->simulated_loss_percent = transport->simulated_loss_percent;
    config->simulated_latency_ms = transport->simulated_latency_ms;
    config->simulated_jitter_ms = transport->simulated_jitter_ms;
    config->simulated_duplicate_percent = transport->simulated_duplicate_percent;
    config->transport = NULL;
    config->public_address = NULL;
    config->per_client_send_kbps = ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS;
    struct rocketnet_message_config_t message;
    rocketnet_message_default_config( &message );
#define RN_COPY( name ) config->name = message.name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY
}

static void rn_server_default_config_current( struct rocketnet_server_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_server_transport_config_t transport;
    rocketnet_server_transport_default_config_current( &transport );
    rn_server_config_from_transport_current( config, &transport );
}

void rocketnet_server_default_config( struct rocketnet_server_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_server_config_t defaults;
    rn_server_default_config_current( &defaults );
    memcpy( config, &defaults, ROCKETNET_SERVER_CONFIG_BYTES_1 );
}

int rocketnet_server_default_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;
    if ( !RN_SERVER_CONFIG_SIZE_KNOWN( config_bytes ) )
        return 0;
    struct rocketnet_server_config_t defaults;
    rn_server_default_config_current( &defaults );
    memcpy( config, &defaults, config_bytes );
    return 1;
}

static void rn_server_dev_config_current( struct rocketnet_server_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_server_transport_config_t transport;
    memset( &transport, 0, sizeof(transport) );
    rocketnet_server_transport_dev_config( &transport );
    rn_server_config_from_transport_current( config, &transport );
}

void rocketnet_server_dev_config( struct rocketnet_server_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_server_config_t current;
    rn_server_dev_config_current( &current );
    memcpy( config, &current, ROCKETNET_SERVER_CONFIG_BYTES_1 );
}

int rocketnet_server_dev_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;
    if ( !RN_SERVER_CONFIG_SIZE_KNOWN( config_bytes ) )
        return 0;
    struct rocketnet_server_config_t current;
    rn_server_dev_config_current( &current );
    memcpy( config, &current, config_bytes );
    return 1;
}

static const rocketnet_server_config_t * rn_server_config_read( rocketnet_server_config_t * current,
                                                                    const rocketnet_server_config_t * config,
                                                                    size_t config_bytes )
{
    if ( !config )
        return NULL;
    rn_server_default_config_current( current );
    memcpy( current, config, config_bytes );
    return current;
}

static int rn_server_validate_config_current( const struct rocketnet_server_config_t * config )

{
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_server_split( config, false, &transport, &message, &kbps ) )
        return 0;
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "server", &transport, fields, num_fields ) )
        return 0;
    return rocketnet_message_validate_config( &message );
}

int rocketnet_server_validate_config( const struct rocketnet_server_config_t * config )
{
    struct rocketnet_server_config_t current;
    return rn_server_validate_config_current( rn_server_config_read( &current, config, ROCKETNET_SERVER_CONFIG_BYTES_1 ) );
}

int rocketnet_server_validate_config_sized( const struct rocketnet_server_config_t * config, size_t config_bytes )
{
    if ( config && !RN_SERVER_CONFIG_SIZE_KNOWN( config_bytes ) )
        return 0;
    struct rocketnet_server_config_t current;
    return rn_server_validate_config_current( rn_server_config_read( &current, config, config_bytes ) );
}

static uint64_t rn_server_required_memory_current( const struct rocketnet_server_config_t * config )

{
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_server_split( config, true, &transport, &message, &kbps ) )
        return 0;
    const uint64_t running_bytes = rocketnet_message_server_required_memory( &transport, &message );
    sodium_memzero( &transport, sizeof( transport ) );
    return running_bytes == 0 ? 0 : rn_server_header_bytes + running_bytes;
}

uint64_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config )
{
    struct rocketnet_server_config_t current;
    return rn_server_required_memory_current( rn_server_config_read( &current, config, ROCKETNET_SERVER_CONFIG_BYTES_1 ) );
}

uint64_t rocketnet_server_required_memory_sized( const struct rocketnet_server_config_t * config, size_t config_bytes )
{
    if ( config && !RN_SERVER_CONFIG_SIZE_KNOWN( config_bytes ) )
        return 0;
    struct rocketnet_server_config_t current;
    return rn_server_required_memory_current( rn_server_config_read( &current, config, config_bytes ) );
}

static struct rocketnet_server_t * rn_server_create_current( void * context, const char * address, const struct rocketnet_server_config_t * config, void * memory, uint64_t memory_bytes )
{
    if ( !rocketnet_api_null_guard( address ) )
        return NULL;
    if ( strlen( address ) >= ROCKETNET_MAX_ADDRESS_STRING_LENGTH )
    {
        rocketnet_error( "server create refused: the address is too long" );
        return NULL;
    }
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_server_split( config, true, &transport, &message, &kbps ) )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        return NULL;
    }
    const uint64_t running_bytes = rocketnet_message_server_required_memory( &transport, &message );
    if ( running_bytes == 0 )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        return NULL;
    }
    const uint64_t total = rn_server_header_bytes + running_bytes;
    uint8_t * base = (uint8_t *) memory;
    if ( base )
    {
        if ( memory_bytes < total || ( (uintptr_t) base % ROCKETNET_ALLOCATION_ALIGNMENT ) != 0 )
        {
            rocketnet_error( "server create refused: the memory block is short or not %d-aligned", ROCKETNET_ALLOCATION_ALIGNMENT );
            sodium_memzero( &transport, sizeof( transport ) );
            return NULL;
        }
    }
    else
    {
        base = (uint8_t *) rocketnet_malloc( context, (size_t) total );
        if ( !base )
        {
            sodium_memzero( &transport, sizeof( transport ) );
            return NULL;
        }
    }
    struct rocketnet_server_t * server = (struct rocketnet_server_t *) base;
    memset( server, 0, sizeof( *server ) );
    server->context = context;
    strncpy( server->address, address, sizeof( server->address ) - 1 );
    server->transport = transport;
    server->message = message;
    server->kbps = kbps;
    server->max_clients = transport.max_clients;
    server->owns_block = memory ? 0 : 1;
    server->block = base + rn_server_header_bytes;
    server->block_bytes = running_bytes;
    sodium_memzero( &transport, sizeof( transport ) );
    return server;
}

struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, void * memory, uint64_t memory_bytes )
{
    struct rocketnet_server_config_t current;
    return rn_server_create_current( context, address,
        rn_server_config_read( &current, config, ROCKETNET_SERVER_CONFIG_BYTES_1 ), memory, memory_bytes );
}

struct rocketnet_server_t * rocketnet_server_create_sized( void * context, const char * address, const struct rocketnet_server_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes )
{
    if ( config && !RN_SERVER_CONFIG_SIZE_KNOWN( config_bytes ) )
        return NULL;
    if ( !config || config_bytes == sizeof( struct rocketnet_server_config_t ) )
        return rn_server_create_current( context, address, config, memory, memory_bytes );
    struct rocketnet_server_config_t current;
    rn_server_default_config_current( &current );
    memcpy( &current, config, config_bytes );
    return rn_server_create_current( context, address, &current, memory, memory_bytes );
}

enum rocketnet_status_t rocketnet_server_start( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    if ( server->running )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "server start refused: the server is already running" );
        return ROCKETNET_STATUS_SERVER_NOT_STOPPED;
    }
    int status = ROCKETNET_STATUS_OK;
    server->running = rocketnet_message_server_create_ex( server->context, server->address, &server->transport, &server->message,
                                                          server->block, server->block_bytes, server->kbps, &status );
    if ( server->running )
        return ROCKETNET_STATUS_OK;
    return status == ROCKETNET_STATUS_OK ? ROCKETNET_STATUS_INVALID_ARGUMENT : (enum rocketnet_status_t) status;
}

static uint64_t rn_server_running_counter( const struct rocketnet_server_t * server, int counter );

void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( !transport )
        return;
    for ( int i = 0; i < server->max_clients; i++ )
    {
        if ( rocketnet_server_transport_is_client_connected( transport, i ) )
            rocketnet_server_transport_disconnect_client( transport, i );
    }
}

void rocketnet_server_stop( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    if ( !server->running )
        return;
    // every slot: the redundant disconnects and the slot's reset (EndClient);
    // the disconnect packets are staged, so flush them before the transport
    // goes
    rocketnet_server_disconnect_all_clients( server );
    rocketnet_server_transport_send_packets( rn_server_transport( server ) );
    for ( int i = 0; i < ROCKETNET_SERVER_COUNTER_COUNT; i++ )
        server->lifetime[i] += rn_server_running_counter( server, i );
    rocketnet_message_server_destroy( server->running );
    server->running = NULL;
}

int rocketnet_server_running( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return server->running ? 1 : 0;
}

void rocketnet_server_destroy( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    rocketnet_server_stop( server );
    void * context = server->context;
    const int owns_block = server->owns_block;
    sodium_memzero( server, sizeof( *server ) );
    if ( owns_block )
        rocketnet_free( context, server );
}

int rocketnet_server_update( struct rocketnet_server_t * server, double time )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_MESSAGE_OK;
    if ( !server->running )
    {
        rocketnet_api_misuse_record();
        return ROCKETNET_MESSAGE_OK;
    }
    return rocketnet_message_server_update( server->running, time );
}

void rocketnet_server_send_messages( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    if ( !server->running )
    {
        rocketnet_api_misuse_record();
        return;
    }
    rocketnet_message_server_send_packets( server->running );
}

uint16_t rocketnet_server_port( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !server->running )
        return 0;
    return rocketnet_server_transport_port( rn_server_transport( server ) );
}

const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    if ( !server->running )
        return NULL;
    return rocketnet_server_transport_driver( rn_server_transport( server ) );
}

int rocketnet_server_max_clients( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return server->max_clients;
}

int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !server->running )
        return 0;
    return rocketnet_server_transport_num_connected_clients( rn_server_transport( server ) );
}

int rocketnet_server_client_connected( const struct rocketnet_server_t * server, uint64_t client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return 0;
    return rocketnet_message_server_client_connected( rn_server( server ), slot );
}

uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, uint64_t client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return 0;
    return rocketnet_server_transport_client_id( rn_server_transport( server ), slot );
}

void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, uint64_t client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return;
    rocketnet_server_transport_disconnect_client( rn_server_transport( server ), slot );
}

int rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, uint64_t client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_MESSAGE_ERROR_NONE;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return ROCKETNET_MESSAGE_ERROR_NONE;
    return rocketnet_message_server_disconnect_reason( rn_server( server ), slot );
}

uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, int reliability, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    if ( rn_server_destination_refused( destination_id ) )
        return NULL;
    int slot;
    if ( !rn_server_slot( server, destination_id, &slot ) )
    {
        rocketnet_api_misuse_record();
        return NULL;
    }
    return rocketnet_message_server_start( rn_server( server ), slot, channel, reliability, bytes );
}

uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, uint64_t destination_id, int bytes_written )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( rn_server_destination_refused( destination_id ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, destination_id, &slot ) )
    {
        rocketnet_api_misuse_record();
        return 0;
    }
    return rocketnet_message_server_finish( rn_server( server ), slot, bytes_written );
}

void rocketnet_server_abort_message( struct rocketnet_server_t * server, uint64_t destination_id )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    if ( rn_server_destination_refused( destination_id ) )
        return;
    int slot;
    if ( !rn_server_slot( server, destination_id, &slot ) )
        return;
    rocketnet_message_server_abort( rn_server( server ), slot );
}

int rocketnet_server_receive_message( struct rocketnet_server_t * server, uint64_t client_index, int channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( data ) || !rocketnet_api_null_guard( bytes ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return 0;
    return rocketnet_message_server_receive( rn_server( server ), slot, channel, data, bytes );
}

void rocketnet_server_release_message( struct rocketnet_server_t * server, uint64_t client_index, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( data ) )
        return;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
    {
        rocketnet_api_misuse_record();
        return;
    }
    rocketnet_message_server_release( rn_server( server ), slot, data );
}

int rocketnet_server_receipts( struct rocketnet_server_t * server, uint64_t client_index, struct rocketnet_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return 0;
    return rocketnet_message_server_receipts( rn_server( server ), slot, (struct rocketnet_message_receipt_t *) out, capacity );
}

int rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, uint64_t client_index, int kbps )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return 0;
    return rocketnet_message_server_set_send_kbps( rn_server( server ), slot, rn_budget_kbps( kbps ) );
}

static uint64_t rn_server_running_counter( const struct rocketnet_server_t * server, int counter )
{
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( !transport )
        return 0;
    const struct rocketnet_counters_t * transport_counters = rocketnet_server_transport_counters( transport );
    const struct rocketnet_counters_t * message_counters = rocketnet_message_server_counters( rn_server( server ) );
    struct rocketnet_server_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_stats( transport, &stats );
    switch ( counter )
    {
        case ROCKETNET_SERVER_COUNTER_PACKETS_SENT:             return stats.packets_sent;
        case ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED:         return stats.packets_received;
        case ROCKETNET_SERVER_COUNTER_BYTES_SENT:               return stats.bytes_sent;
        case ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED:           return stats.bytes_received;
        case ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_CONNECTED );
        case ROCKETNET_SERVER_COUNTER_CLIENTS_DENIED_FULL:      return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS ) +
                                                                       rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS );
        case ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET:   return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED );
        case ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_PAYLOAD_DELIVERED );
        case ROCKETNET_SERVER_COUNTER_MESSAGES_RESENT:          return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
        case ROCKETNET_SERVER_COUNTER_MESSAGES_DUPLICATE:       return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
        case ROCKETNET_SERVER_COUNTER_RECEIPTS_ACKED:           return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED );
        case ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST:            return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
        case ROCKETNET_SERVER_COUNTER_BUDGET_DROPS:             return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        case ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED:    return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED );
        default:                                                return 0;
    }
}

uint64_t rocketnet_server_counter( const struct rocketnet_server_t * server, enum rocketnet_server_counter_t counter )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( (int) counter < 0 || (int) counter >= ROCKETNET_SERVER_COUNTER_COUNT )
        return 0;
    return server->lifetime[counter] + rn_server_running_counter( server, (int) counter );
}

double rocketnet_server_stat( const struct rocketnet_server_t * server, enum rocketnet_server_stat_t stat )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0.0;
    if ( !server->running )
        return 0.0;
    struct rocketnet_server_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_stats( rn_server_transport( server ), &stats );
    switch ( stat )
    {
        case ROCKETNET_SERVER_STAT_PACKETS_SENT_PER_SECOND:     return (double) stats.packets_sent_per_second;
        case ROCKETNET_SERVER_STAT_PACKETS_RECEIVED_PER_SECOND: return (double) stats.packets_received_per_second;
        case ROCKETNET_SERVER_STAT_BANDWIDTH_SENT:              return stats.bandwidth_sent_kbps;
        case ROCKETNET_SERVER_STAT_BANDWIDTH_RECEIVED:          return stats.bandwidth_received_kbps;
        default:                                                return 0.0;
    }
}

static bool rn_server_client_stats( const struct rocketnet_server_t * server, uint64_t client_index, struct rocketnet_server_transport_client_stats_t * stats )
{
    memset( stats, 0, sizeof( *stats ) );
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return false;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( !rocketnet_server_transport_is_client_connected( transport, slot ) )
        return false;
    rocketnet_server_transport_client_stats( transport, slot, stats );
    return true;
}

uint64_t rocketnet_server_client_counter( const struct rocketnet_server_t * server, uint64_t client_index, enum rocketnet_server_client_counter_t counter )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    struct rocketnet_server_transport_client_stats_t stats;
    if ( !rn_server_client_stats( server, client_index, &stats ) )
        return 0;
    switch ( counter )
    {
        case ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT:      return stats.packets_sent;
        case ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_RECEIVED:  return stats.packets_received;
        case ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT:        return stats.bytes_sent;
        case ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED:    return stats.bytes_received;
        default:                                                return 0;
    }
}

double rocketnet_server_client_stat( const struct rocketnet_server_t * server, uint64_t client_index, enum rocketnet_server_client_stat_t stat )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0.0;
    struct rocketnet_server_transport_client_stats_t stats;
    if ( !rn_server_client_stats( server, client_index, &stats ) )
        return 0.0;
    switch ( stat )
    {
        case ROCKETNET_SERVER_CLIENT_STAT_RTT:                  return stats.rtt;
        case ROCKETNET_SERVER_CLIENT_STAT_JITTER:               return stats.jitter;
        case ROCKETNET_SERVER_CLIENT_STAT_PACKET_LOSS:          return stats.packet_loss;
        case ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_SENT:       return stats.bandwidth_sent_kbps;
        case ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_RECEIVED:   return stats.bandwidth_received_kbps;
        case ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_ACKED:      return stats.bandwidth_acked_kbps;
        default:                                                return 0.0;
    }
}
