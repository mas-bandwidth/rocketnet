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
#include "rocketnet_test_seams.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_message_object.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_fragment.h"
#include "rocketnet_base64.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"

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

static_assert( ROCKETNET_SEND_BUDGET_MAX_KBPS == ROCKETNET_SEND_KBPS_UNLIMITED - 1, "every budget below the unlimited sentinel is a budget" );
static_assert( ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS <= ROCKETNET_SEND_BUDGET_MAX_KBPS && ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS <= ROCKETNET_SEND_BUDGET_MAX_KBPS, "the default budgets are valid budgets" );

uint32_t rocketnet_version( void )
{
    return ROCKETNET_VERSION;
}

// ---------------------------------------------------------------
//                          the config split
// ---------------------------------------------------------------

// every message field, one row each, in the order of rocketnet_message_config_t

#define RN_PUBLIC_MESSAGE_FIELDS( X ) \
    X( num_channels ) X( reliable_messages_in_flight ) X( receive_message_buffer_bytes ) X( max_message_bytes ) X( send_message_buffer_bytes ) \
    X( send_queue_messages ) X( packets_in_flight ) X( packets_per_update ) X( unreliable_messages_per_update )      \
    X( sequenced_messages_held ) X( max_message_receipts ) X( message_resend_delay_initial_ms ) X( message_resend_delay_min_ms ) X( message_resend_delay_max_ms ) X( reliable_message_timeout_ms )

// the send budget fields: one int each, read under the endpoint's env name.
// 0 is the directional default rate, ROCKETNET_SEND_KBPS_UNLIMITED is no
// budget, and a negative rate is refused as INVALID_ARGUMENT
// (rn_public_send_kbps).

static const struct rocketnet_config_field_t rn_client_budget_fields[] =
{
    { "send_kbps", ROCKETNET_CONFIG_KIND_INT, 0, (int) sizeof(int), (int64_t) INT_MIN, (int64_t) ROCKETNET_SEND_KBPS_UNLIMITED, 0 },
};

static const struct rocketnet_config_field_t rn_server_budget_fields[] =
{
    { "per_client_send_kbps", ROCKETNET_CONFIG_KIND_INT, 0, (int) sizeof(int), (int64_t) INT_MIN, (int64_t) ROCKETNET_SEND_KBPS_UNLIMITED, 0 },
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
    return kbps <= 0 ? 0u : (uint32_t) kbps;                   // 0: uncapped
}

// The public send rate to the message layer's: 0 is the default rate (the
// value default_config writes), ROCKETNET_SEND_KBPS_UNLIMITED is no budget,
// which the message layer holds as 0 (uncapped, with no create-time reserve
// for it), and a negative rate is refused.
static bool rn_public_send_kbps( const char * endpoint, const char * field, int kbps, int default_kbps, int * resolved, enum rocketnet_status_t * failure )
{
    if ( kbps < 0 )
    {
        rocketnet_error( "%s config refused: %s is %d; a send rate is 0 (the default), a budget in kbps, or ROCKETNET_SEND_KBPS_UNLIMITED", endpoint, field, kbps );
        if ( failure ) *failure = ROCKETNET_STATUS_INVALID_ARGUMENT;
        return false;
    }
    if ( kbps == 0 )
        kbps = default_kbps;
    *resolved = kbps == ROCKETNET_SEND_KBPS_UNLIMITED ? 0 : kbps;
    return true;
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

// the message table's two budget rows, client_send_kbps and server_send_kbps,
// take no variable here: the endpoint's budget is its one budget field above,
// ROCKETNET_CLIENT_SEND_KBPS or ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS, which
// rn_client_split (:135) and rn_server_split (:173) resolve for the create

static enum rocketnet_status_t rn_apply_message_and_budget_env( const char * endpoint, struct rocketnet_message_config_t * message, int * kbps, const struct rocketnet_config_field_t * budget_fields )
{
    int num_message_fields = 0;
    const struct rocketnet_config_field_t * message_fields = rocketnet_message_config_fields( &num_message_fields );
    for ( int i = 0; i < num_message_fields; i++ )
    {
        if ( strcmp( message_fields[i].name, "client_send_kbps" ) == 0 || strcmp( message_fields[i].name, "server_send_kbps" ) == 0 )
            continue;
        if ( !rocketnet_config_apply_env( endpoint, message, &message_fields[i], 1 ) )
            return ROCKETNET_STATUS_CONFIG_ENV_INVALID;
    }
    if ( !rocketnet_config_apply_env( endpoint, kbps, budget_fields, 1 ) )
        return ROCKETNET_STATUS_CONFIG_ENV_INVALID;
    return rocketnet_config_validate( endpoint, kbps, budget_fields, 1 ) ?
        ROCKETNET_STATUS_OK : ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
}

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_ADDRESS_STRING_LENGTH == ROCKETNET_MAX_ADDRESS_STRING_BYTES, "one address string bound, public and internal" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_FILTER_VALUE_BYTES == ROCKETNET_PACKET_FILTER_BYTES, "one packet filter size, public and wire" );

// The development network simulator's environment form, read at create in a
// development library only: ROCKETNET_DEV_CLIENT_LATENCY_MS and the rest; an
// unset or empty variable leaves the field, a bad value refuses the create.
#if ROCKETNET_DEV
// one variable: found is rocketnet_platform_getenv's answer (1 set, 0 unset,
// -1 too long) and text its value
static bool rn_dev_env_int( const char * name, int found, const char * text, int min_value, int max_value, int * value )
{
    if ( found == 0 || ( found == 1 && text[0] == '\0' ) )
        return true;
    char * end = NULL;
    const long parsed = found == 1 ? strtol( text, &end, 10 ) : 0;
    if ( found != 1 || end == text || *end != '\0' || parsed < min_value || parsed > max_value )
    {
        rocketnet_error( "%s=\"%s\" refused: expected %d .. %d", name, text, min_value, max_value );
        return false;
    }
    *value = (int) parsed;
    return true;
}
#endif // ROCKETNET_DEV

template <typename transport_config_t> static bool rn_dev_simulator_env( const char * side, transport_config_t * transport )
{
#if ROCKETNET_DEV
    char text[64];
    if ( side[0] == 'c' )
        return rn_dev_env_int( "ROCKETNET_DEV_CLIENT_LATENCY_MS", rocketnet_platform_getenv( "ROCKETNET_DEV_CLIENT_LATENCY_MS", text, sizeof( text ) ), text, 0, 60000, &transport->simulated_latency_ms ) &&
               rn_dev_env_int( "ROCKETNET_DEV_CLIENT_JITTER_MS", rocketnet_platform_getenv( "ROCKETNET_DEV_CLIENT_JITTER_MS", text, sizeof( text ) ), text, 0, 60000, &transport->simulated_jitter_ms ) &&
               rn_dev_env_int( "ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT", rocketnet_platform_getenv( "ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT", text, sizeof( text ) ), text, 0, 100, &transport->simulated_loss_percent ) &&
               rn_dev_env_int( "ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT", rocketnet_platform_getenv( "ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT", text, sizeof( text ) ), text, 0, 100, &transport->simulated_duplicate_percent );
    return rn_dev_env_int( "ROCKETNET_DEV_SERVER_LATENCY_MS", rocketnet_platform_getenv( "ROCKETNET_DEV_SERVER_LATENCY_MS", text, sizeof( text ) ), text, 0, 60000, &transport->simulated_latency_ms ) &&
           rn_dev_env_int( "ROCKETNET_DEV_SERVER_JITTER_MS", rocketnet_platform_getenv( "ROCKETNET_DEV_SERVER_JITTER_MS", text, sizeof( text ) ), text, 0, 60000, &transport->simulated_jitter_ms ) &&
           rn_dev_env_int( "ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT", rocketnet_platform_getenv( "ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT", text, sizeof( text ) ), text, 0, 100, &transport->simulated_loss_percent ) &&
           rn_dev_env_int( "ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT", rocketnet_platform_getenv( "ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT", text, sizeof( text ) ), text, 0, 100, &transport->simulated_duplicate_percent );
#else
    (void) side;
    (void) transport;
    return true;
#endif
}

static void rn_client_default_config_current( struct rocketnet_client_config_t * config );
static void rn_server_default_config_current( struct rocketnet_server_config_t * config );

static bool rn_client_split( const struct rocketnet_client_config_t * config, bool read_env,
                             struct rocketnet_client_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps, enum rocketnet_status_t * failure = NULL,
                             const struct rocketnet_client_testing_t * testing = NULL )
{
    struct rocketnet_client_config_t defaults;
    if ( !config )
    {
        rn_client_default_config_current( &defaults );
        config = &defaults;
    }

    rocketnet_client_transport_default_config_current( transport );
    memcpy( transport->packet_filter, config->packet_filter, sizeof( transport->packet_filter ) );
    transport->max_payload_bytes = config->max_payload_bytes;
    transport->max_fragmented_messages = config->max_fragmented_messages;
    if ( !rn_copy_config_string( transport->transport, sizeof( transport->transport ), config->transport, "auto", "client", "transport" ) )
    {
        if ( failure ) *failure = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return false;
    }
    transport->state_change_callback = config->state_change_callback;
    if ( testing )
    {
        transport->time_callback = testing->time_callback;
        transport->step_driven = testing->step_driven ? 1 : 0;
    }
    if ( read_env && !rn_dev_simulator_env( "client", transport ) )
    {
        if ( failure ) *failure = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return false;
    }

    rocketnet_message_default_config( message );
#define RN_COPY( name ) message->name = config->name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY

    int send_kbps = config->send_kbps;
    const enum rocketnet_status_t env_status = read_env ?
        rn_apply_message_and_budget_env( "client", message, &send_kbps, rn_client_budget_fields ) : ROCKETNET_STATUS_OK;
    if ( env_status != ROCKETNET_STATUS_OK )
    {
        if ( failure ) *failure = env_status;
        return false;
    }
    if ( !read_env && !rocketnet_config_validate( "client", &send_kbps, rn_client_budget_fields, 1 ) )
    {
        if ( failure ) *failure = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return false;
    }
    int resolved = 0;
    if ( !rn_public_send_kbps( "client", "send_kbps", send_kbps, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, &resolved, failure ) )
        return false;
    message->client_send_kbps = resolved;
    *kbps = rn_budget_kbps( resolved );
    return true;
}

static bool rn_server_split( const struct rocketnet_server_config_t * config, bool read_env,
                             struct rocketnet_server_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps, enum rocketnet_status_t * failure = NULL )
{
    struct rocketnet_server_config_t defaults;
    if ( !config )
    {
        rn_server_default_config_current( &defaults );
        config = &defaults;
    }

    rocketnet_server_transport_default_config_current( transport );
    transport->protocol_id = config->protocol_id;
    transport->max_clients = config->max_clients;
    memcpy( transport->packet_filter, config->packet_filter, sizeof( transport->packet_filter ) );
    memcpy( transport->backend_public_key, config->backend_public_key, sizeof( transport->backend_public_key ) );
    transport->num_xdp_queues = config->num_xdp_queues;
    transport->num_os_cpus = config->num_os_cpus;
    transport->max_payload_bytes = config->max_payload_bytes;
    transport->max_fragmented_messages = config->max_fragmented_messages;
    transport->max_timeout_seconds = config->max_timeout_seconds;
    transport->handshake_deadline_seconds = config->handshake_deadline_seconds;
    if ( read_env && !rn_dev_simulator_env( "server", transport ) )
    {
        if ( failure ) *failure = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
        return false;
    }
    if ( !rn_copy_config_string( transport->transport, sizeof( transport->transport ), config->transport, "auto", "server", "transport" ) )
    {
        if ( failure ) *failure = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return false;
    }
    // the served addresses come from create's arguments (rn_server_addresses)
    transport->public_address[0] = '\0';
    transport->second_public_address[0] = '\0';

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
            {
                if ( failure ) *failure = ROCKETNET_STATUS_CONFIG_ENV_INVALID;
                return false;
            }
    }

    int send_kbps = config->per_client_send_kbps;
    const enum rocketnet_status_t env_status = read_env ?
        rn_apply_message_and_budget_env( "server", message, &send_kbps, rn_server_budget_fields ) : ROCKETNET_STATUS_OK;
    if ( env_status != ROCKETNET_STATUS_OK )
    {
        if ( failure ) *failure = env_status;
        return false;
    }
    if ( !read_env && !rocketnet_config_validate( "server", &send_kbps, rn_server_budget_fields, 1 ) )
    {
        if ( failure ) *failure = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return false;
    }
    int resolved = 0;
    if ( !rn_public_send_kbps( "server", "per_client_send_kbps", send_kbps, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, &resolved, failure ) )
        return false;
    message->server_send_kbps = resolved;
    *kbps = rn_budget_kbps( resolved );
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
// block by connect (the model's Connect) and ended by disconnect, destroy, or
// the next connect after the server ended it (End, then Reset). Every end
// runs the connection reset of sdk/rocketnet_connection_reset.h (the
// protocol's at the terminal transition, the message object's here), then
// destroys the connection, so the
// next connect starts from a block in which nothing of the last connection
// remains.
//
// The socket and the I/O thread are per connection: the token decides the
// server's address family, and the socket binds to that family after the
// token parses, so a client that connects to an IPv4 server and then an IPv6
// one needs a socket of each.

// The counts each public stats struct carries, indexed so a connection's
// counts move into the lifetime counts in one loop. Internal: the public form
// is struct rocketnet_client_stats_t and struct rocketnet_server_stats_t.
enum rn_client_count_t
{
    RN_CLIENT_PACKETS_SENT,
    RN_CLIENT_PACKETS_RECEIVED,
    RN_CLIENT_BYTES_SENT,
    RN_CLIENT_BYTES_RECEIVED,
    RN_CLIENT_PAYLOADS_RECEIVED,
    RN_CLIENT_MESSAGES_RESENT,
    RN_CLIENT_MESSAGES_DUPLICATE,
    RN_CLIENT_RECEIPTS_ACKED,
    RN_CLIENT_RECEIPTS_LOST,
    RN_CLIENT_RECEIPTS_DROPPED,
    RN_CLIENT_BUDGET_DROPS,
    RN_CLIENT_SEND_DEFERRED,
    RN_CLIENT_NOT_CONNECTED_DROPS,
    RN_CLIENT_COUNT
};

enum rn_server_count_t
{
    RN_SERVER_PACKETS_SENT,
    RN_SERVER_PACKETS_RECEIVED,
    RN_SERVER_BYTES_SENT,
    RN_SERVER_BYTES_RECEIVED,
    RN_SERVER_CLIENTS_CONNECTED,
    RN_SERVER_CLIENTS_DENIED_FULL,
    RN_SERVER_HANDSHAKES_OVER_BUDGET,
    RN_SERVER_PAYLOADS_RECEIVED,
    RN_SERVER_MESSAGES_RESENT,
    RN_SERVER_MESSAGES_DUPLICATE,
    RN_SERVER_RECEIPTS_ACKED,
    RN_SERVER_RECEIPTS_LOST,
    RN_SERVER_RECEIPTS_DROPPED,
    RN_SERVER_BUDGET_DROPS,
    RN_SERVER_NOT_CONNECTED_DROPS,
    RN_SERVER_COUNT
};

struct rocketnet_client_t
{
    void * context;
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps;
    int owns_block;                                     // 1: this struct and the block are one rocketnet_malloc
    uint8_t * block;                                    // the connection's memory, ROCKETNET_ALLOCATION_ALIGNMENT aligned
    uint64_t block_bytes;
    int block_used;                                     // 1 once a connect handed the block to a connection
    struct rocketnet_message_client_t * connection;     // NULL while DISCONNECTED or after a refused connect
    struct rocketnet_message_client_t * retiring;       // retained until worker completion and synchronous reclamation
    int refused_state;                                  // token refusal or terminal state retained during retry, 0 otherwise
    uint64_t lifetime[RN_CLIENT_COUNT];                 // the counts of every ended connection
    struct rocketnet_message_batch_t batch;             // the one receive batch, refilled by each receive (in the create-time block)
};

static inline uint64_t rn_round_up_alignment( uint64_t bytes )
{
    return ( bytes + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) / ROCKETNET_ALLOCATION_ALIGNMENT * ROCKETNET_ALLOCATION_ALIGNMENT;
}

static const uint64_t rn_client_header_bytes = rn_round_up_alignment( sizeof( struct rocketnet_client_t ) );

struct rocketnet_message_client_t * rocketnet_client_message_client( struct rocketnet_client_t * client )
{
    return client ? ( client->connection ? client->connection : client->retiring ) : NULL;
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
    config->config_bytes = sizeof( *config );
    struct rocketnet_client_transport_config_t transport;
    rocketnet_client_transport_default_config_current( &transport );
    memcpy( config->packet_filter, transport.packet_filter, sizeof( config->packet_filter ) );
    config->max_payload_bytes = transport.max_payload_bytes;
    config->max_fragmented_messages = transport.max_fragmented_messages;
    config->transport = NULL;
    config->state_change_callback = transport.state_change_callback;
    config->send_kbps = ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS;
    struct rocketnet_message_config_t message;
    rocketnet_message_default_config( &message );
#define RN_COPY( name ) config->name = message.name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY
}

// docs/ABI-EVOLUTION.md: a public config's first field, config_bytes, is the
// size of the struct the caller compiled against, set by the header's inline
// default_config. Below the first field (a zeroed config included) is refused:
// the config was never initialized. A newer caller's larger struct reads as
// far as this library knows and its tail is ignored; an older caller's smaller
// struct reads its prefix and the rest takes the defaults. Every byte the
// library reads or writes is below the caller's config_bytes.
static enum rocketnet_status_t rn_public_config_bytes( const char * endpoint, size_t bytes, size_t current, size_t * use )
{
    if ( bytes < sizeof( size_t ) )
    {
        rocketnet_error( "%s config refused: config_bytes is %d; fill the config with rocketnet_%s_default_config first", endpoint, (int) bytes, endpoint );
        return ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED;
    }
    *use = bytes < current ? bytes : current;
    return ROCKETNET_STATUS_OK;
}

// copies [config_bytes's end, use) from source to dest: every field after the
// size, never the size itself
static void rn_public_config_copy( void * dest, const void * source, size_t use )
{
    if ( use > sizeof( size_t ) )
        memcpy( (uint8_t *) dest + sizeof( size_t ), (const uint8_t *) source + sizeof( size_t ), use - sizeof( size_t ) );
}

enum rocketnet_status_t rocketnet_client_default_config_internal( struct rocketnet_client_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    size_t use = 0;
    const enum rocketnet_status_t status = rn_public_config_bytes( "client", config->config_bytes, sizeof( struct rocketnet_client_config_t ), &use );
    if ( status != ROCKETNET_STATUS_OK )
        return status;
    struct rocketnet_client_config_t defaults;
    rn_client_default_config_current( &defaults );
    rn_public_config_copy( config, &defaults, use );
    return ROCKETNET_STATUS_OK;
}

// the caller's config over the defaults, as far as both know; NULL is the
// defaults (read stays NULL)
static enum rocketnet_status_t rn_client_config_read( rocketnet_client_config_t * current,
                                                      const rocketnet_client_config_t * config,
                                                      const rocketnet_client_config_t ** read )
{
    *read = NULL;
    if ( !config )
        return ROCKETNET_STATUS_OK;
    size_t use = 0;
    const enum rocketnet_status_t status = rn_public_config_bytes( "client", config->config_bytes, sizeof( *current ), &use );
    if ( status != ROCKETNET_STATUS_OK )
        return status;
    rn_client_default_config_current( current );
    rn_public_config_copy( current, config, use );
    *read = current;
    return ROCKETNET_STATUS_OK;
}

// The one validation behind create: the transport table's
// ranges, the public link simulator's posture and the transport list's parse,
// each the check connect runs on the same values
// (rocketnet_client_transport_create_internal),
// then the message config's rules. Each refusal logs the rule it broke.
static enum rocketnet_status_t rn_client_config_valid( const struct rocketnet_client_transport_config_t * transport, const struct rocketnet_message_config_t * message )
{
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "client", transport, fields, num_fields ) )
        return ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
    if ( !rocketnet_config_validate_public_link_simulator( "client", transport, fields, num_fields, rocketnet_public_link_simulator_development() ) )
        return ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
    if ( rocketnet_config_packet_filter_refused( "client", transport->packet_filter ) )
        return ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION;
    const int requested_payload = transport->max_payload_bytes > 0 ? transport->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int requested_slots = transport->max_fragmented_messages > 0 ? transport->max_fragmented_messages : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    if ( !rocketnet_reassembly_memory_allowed( 1, requested_slots, requested_payload, rocketnet_client_reassembly_delivered_capacity( requested_slots ) ) )
        return ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED;
    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );
    struct rocketnet_transport_ladder_t ladder;
    if ( !rocketnet_transport_ladder_parse( transport->transport, available, num_available, &ladder ) )
        return ROCKETNET_STATUS_INVALID_TRANSPORT_LIST;
    return rocketnet_message_validate_config( message ) != 0 ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
}

static size_t rn_client_required_memory_current( const struct rocketnet_client_config_t * config )

{
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_client_split( config, true, &transport, &message, &kbps ) )
        return 0;
    const uint64_t connection_bytes = rocketnet_message_client_required_memory( &message );
    const uint64_t total = connection_bytes == 0 ? 0 : rn_client_header_bytes + connection_bytes;
    return total > (uint64_t) SIZE_MAX ? 0 : (size_t) total;
}

size_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config )
{
    struct rocketnet_client_config_t current;
    const struct rocketnet_client_config_t * read = NULL;
    if ( rn_client_config_read( &current, config, &read ) != ROCKETNET_STATUS_OK )
        return 0;
    return rn_client_required_memory_current( read );
}

static struct rocketnet_client_t * rn_client_create_current( void * context, const struct rocketnet_client_config_t * config, const struct rocketnet_client_testing_t * testing, void * memory, size_t memory_bytes, enum rocketnet_status_t * status )
{
    if ( status )
        *status = ROCKETNET_STATUS_OK;
    // the lifecycle state is read once here, before any allocation; a create
    // before rocketnet_init is refused, logged and counted, as the transport
    // create refuses it at connect (rocketnet_client_transport_create_internal)
    if ( !rocketnet_library_initialized() )
    {
        rocketnet_error( "rocketnet_client_create: rocketnet_init must be called first" );
        rocketnet_api_misuse_record();
        if ( status )
            *status = ROCKETNET_STATUS_NOT_INITIALIZED;
        return NULL;
    }
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    enum rocketnet_status_t failure = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
    if ( !rn_client_split( config, true, &transport, &message, &kbps, &failure, testing ) )
    {
        if ( status )
            *status = failure;
        return NULL;
    }
    const enum rocketnet_status_t valid = rn_client_config_valid( &transport, &message );
    if ( valid != ROCKETNET_STATUS_OK )
    {
        if ( status )
            *status = valid;
        return NULL;
    }
    const uint64_t connection_bytes = rocketnet_message_client_required_memory( &message );
    if ( connection_bytes == 0 )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return NULL;
    }
    const uint64_t total = rn_client_header_bytes + connection_bytes;
    uint8_t * base = (uint8_t *) memory;
    if ( base )
    {
        if ( memory_bytes < total || ( (uintptr_t) base % ROCKETNET_ALLOCATION_ALIGNMENT ) != 0 )
        {
            rocketnet_error( "client create refused: the memory block is short or not %d-aligned", ROCKETNET_ALLOCATION_ALIGNMENT );
            if ( status )
                *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
            return NULL;
        }
    }
    else
    {
        base = (uint8_t *) rocketnet_malloc( context, (size_t) total );
        if ( !base )
        {
            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
            return NULL;
        }
    }
    struct rocketnet_client_t * client = (struct rocketnet_client_t *) base;
    // the receive batch is last and is written only as far as each receive
    // fills it, so its pages are touched only by use
    memset( client, 0, offsetof( struct rocketnet_client_t, batch ) );
    client->batch.num_messages = 0;
    client->context = context;
    client->transport = transport;
    client->message = message;
    client->kbps = kbps;
    client->owns_block = memory ? 0 : 1;
    client->block = base + rn_client_header_bytes;
    client->block_bytes = connection_bytes;
    return client;
}

static struct rocketnet_client_t * rn_client_create( void * context, const struct rocketnet_client_config_t * config, const struct rocketnet_client_testing_t * testing, enum rocketnet_status_t * status )
{
    if ( status )
        *status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_config_t current;
    const struct rocketnet_client_config_t * read = NULL;
    const enum rocketnet_status_t read_status = rn_client_config_read( &current, config, &read );
    if ( read_status != ROCKETNET_STATUS_OK )
    {
        if ( status )
            *status = read_status;
        return NULL;
    }
    return rn_client_create_current( context, read, testing, NULL, 0, status );
}

struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, enum rocketnet_status_t * status )
{
    return rn_client_create( context, config, NULL, status );
}

#if ROCKETNET_TEST_SEAMS
struct rocketnet_client_t * rocketnet_client_create_testing( void * context, const struct rocketnet_client_config_t * config, const struct rocketnet_client_testing_t * testing, enum rocketnet_status_t * status )
{
    return rn_client_create( context, config, testing, status );
}
#endif // #if ROCKETNET_TEST_SEAMS

static void rn_client_connection_counts( struct rocketnet_message_client_t * message_client, uint64_t counts[RN_CLIENT_COUNT], struct rocketnet_client_transport_stats_t * transport_stats );

// End then Reset (tla/ClientSession.tla): the connection's counters move into
// the lifetime counters, its reason is kept, and its destroy -- which sends
// the redundant disconnects from a live state and runs the connection reset
// -- leaves the block holding nothing of it.
static void rn_client_end_connection( struct rocketnet_client_t * client )
{
    if ( !client->retiring )
        return;
    rocketnet_assert( rocketnet_client_disconnected( client ) );
    uint64_t counts[RN_CLIENT_COUNT];
    rn_client_connection_counts( client->retiring, counts, NULL );
    for ( int i = 0; i < RN_CLIENT_COUNT; i++ )
        client->lifetime[i] += counts[i];
    rocketnet_client_connection_reset( &client->retiring->object );
    rocketnet_message_client_destroy( client->retiring );
    client->retiring = NULL;
}

static int rn_client_state( const struct rocketnet_client_t * client )
{
    if ( client->connection )
        return rocketnet_message_client_state( client->connection );
    return client->refused_state;
}

// the token string to its bytes: a bounded scan for the NUL (never strlen on
// a caller's string), exactly the token's base64 length, then the decode
static bool rn_connect_token_decode( const char * connect_token, uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES] )
{
    const char * end = (const char *) memchr( connect_token, '\0', ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES );
    if ( !end || end - connect_token != ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 )
    {
        rocketnet_error( "client connect refused: the connect token string is not %d base64 characters", ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
        return false;
    }
    if ( rocketnet_base64_decode_data( connect_token, token, ROCKETNET_CONNECT_TOKEN_BYTES ) != ROCKETNET_CONNECT_TOKEN_BYTES )
    {
        rocketnet_error( "client connect refused: the connect token string is not base64 of one token" );
        return false;
    }
    return true;
}

enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const char * connect_token_string )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( connect_token_string ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    // connect reads no clock: the connection's clock starts at its first
    // update, as create's always has (documented in rocketnet.h)

    if ( rn_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "client connect refused: the client is not disconnected" );
        return ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED;
    }

    // Reject a malformed string before changing a terminal attempt. The
    // caller submits the token again after an old worker completes; no token
    // or key material is retained across that pending interval.
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rn_connect_token_decode( connect_token_string, connect_token ) )
    {
        sodium_memzero( connect_token, sizeof( connect_token ) );
        return ROCKETNET_STATUS_INVALID_CONNECT_TOKEN;
    }

    // Every terminal state may retry. Preserve its public state while the
    // old worker retires; the explicit disconnect API still clears it to
    // DISCONNECTED. A DISCONNECTED message object may carry a more specific
    // server/message reason through rocketnet_client_state.
    if ( client->connection )
    {
        const int terminal_state = rocketnet_client_state( client );
        rocketnet_client_disconnect( client );
        client->refused_state = terminal_state;
    }

    if ( client->retiring )
    {
        if ( !rocketnet_client_disconnected( client ) )
        {
            sodium_memzero( connect_token, sizeof( connect_token ) );
            return ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED;
        }
        rn_client_end_connection( client );
    }

    // A fresh attempt starts here. A later socket/thread failure must leave
    // DISCONNECTED, while invalid/expired token failures set their own state.
    client->refused_state = ROCKETNET_CLIENT_STATE_DISCONNECTED;
    int status = ROCKETNET_STATUS_OK;
    client->block_used = 1;
    client->connection = rocketnet_message_client_create_ex( client->context, connect_token, &client->transport, &client->message,
                                                              client->block, client->block_bytes, client->kbps, &status, client->message.client_send_kbps <= 0 );
    sodium_memzero( connect_token, sizeof( connect_token ) );
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
    if ( !client )
        return;                                         // as free(NULL): silent, uncounted
    rocketnet_assert( rocketnet_client_disconnected( client ) );
    if ( !rocketnet_client_disconnected( client ) )
        return;
    rn_client_end_connection( client );
    // the header and the connection block after it hold the last
    // connection's message bytes: all of it is wiped, then freed or given
    // back (an adopted client is the header alone). A block no connect ever
    // used holds nothing, and wiping it would touch every page it plans.
    void * context = client->context;
    const int owns_block = client->owns_block;
    const size_t bytes = client->block ? (size_t) ( rn_client_header_bytes + ( client->block_used ? client->block_bytes : 0 ) ) : sizeof( *client );
    sodium_memzero( client, bytes );
    if ( owns_block )
        rocketnet_free( context, client );
}

void rocketnet_client_update( struct rocketnet_client_t * client, double time )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    if ( client->retiring )
    {
        struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( client->retiring );
        if ( transport )
            rocketnet_client_transport_update( transport, time );
        return;
    }
    if ( !client->connection )
        return;
    (void) rocketnet_message_client_update( client->connection, time );
}

void rocketnet_client_send_packets( struct rocketnet_client_t * client )
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
    if ( client->connection )
    {
        if ( rocketnet_message_client_state( client->connection ) > ROCKETNET_CLIENT_STATE_DISCONNECTED )
            rn_message_object_kick( &client->connection->object, 0, ROCKETNET_DISCONNECT_REASON_NONE );
        client->retiring = client->connection;
        client->connection = NULL;
        struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( client->retiring );
        if ( transport )
            rocketnet_client_transport_stop( transport );
    }
    client->refused_state = ROCKETNET_CLIENT_STATE_DISCONNECTED;
}

bool rocketnet_client_disconnected( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) || client->connection )
        return false;
    struct rocketnet_client_transport_t * transport = client->retiring ? rocketnet_message_client_transport( client->retiring ) : NULL;
    return !transport || rocketnet_client_transport_stopped( transport );
}

enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
    {
        rocketnet_error( "rocketnet_client_state: the client is NULL" );
        return ROCKETNET_CLIENT_STATE_DISCONNECTED;
    }
    const int state = rn_client_state( client );
    if ( state != ROCKETNET_CLIENT_STATE_DISCONNECTED || !client->connection )
        return (enum rocketnet_client_state_t) state;
    // a connection that ended after CONNECTED is DISCONNECTED in the message
    // layer with its reason: the public state is that reason's own negative
    // state, kept until disconnect or the next connect ends the connection
    switch ( rocketnet_message_client_disconnect_reason( client->connection ) )
    {
        case ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED:       return ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED;
        case ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT:          return ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT;
        case ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION:        return ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION;
        case ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH:           return ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH;
        case ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED:  return ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED;
        case ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED:      return ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED;
        default:                                                    return ROCKETNET_CLIENT_STATE_DISCONNECTED;
    }
}

// the enum suffix, upper case, or "UNKNOWN" for a value that is not a state
const char * rocketnet_client_state_name( enum rocketnet_client_state_t state )
{
    switch ( (int) state )
    {
        case ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED:           return "MESSAGE_NOT_RECEIVED";
        case ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED:       return "MESSAGE_MEMORY_EXHAUSTED";
        case ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH:                return "CONFIG_MISMATCH";
        case ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION:             return "PROTOCOL_VIOLATION";
        case ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT:             return "RELIABLE_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED:            return "SERVER_DISCONNECTED";
        case ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED:          return "CONNECT_TOKEN_EXPIRED";
        case ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN:          return "INVALID_CONNECT_TOKEN";
        case ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT:           return "CONNECTION_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT:  return "CONNECTION_RESPONSE_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT:   return "CONNECTION_REQUEST_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT:       return "COOKIE_REQUEST_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_CONNECTION_DENIED:              return "CONNECTION_DENIED";
        case ROCKETNET_CLIENT_STATE_DISCONNECTED:                   return "DISCONNECTED";
        case ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE:              return "REQUESTING_COOKIE";
        case ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST:     return "SENDING_CONNECTION_REQUEST";
        case ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE:    return "SENDING_CONNECTION_RESPONSE";
        case ROCKETNET_CLIENT_STATE_CONNECTED:                      return "CONNECTED";
        default:                                                    return "UNKNOWN";
    }
}

// the enum suffix, upper case, or "UNKNOWN" for a value that is not a reason
const char * rocketnet_disconnect_reason_name( enum rocketnet_disconnect_reason_t reason )
{
    switch ( (int) reason )
    {
        case ROCKETNET_DISCONNECT_REASON_NONE:                      return "NONE";
        case ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH:           return "CONFIG_MISMATCH";
        case ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION:        return "PROTOCOL_VIOLATION";
        case ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT:        return "RELIABLE_TIMED_OUT";
        case ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED:  return "MESSAGE_MEMORY_EXHAUSTED";
        case ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED:       return "SERVER_DISCONNECTED";
        case ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED:      return "MESSAGE_NOT_RECEIVED";
        default:                                                    return "UNKNOWN";
    }
}

int rocketnet_client_index( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return -1;
    if ( !client->connection )
        return -1;
    // only a CONNECTED client holds a server slot: before then the transport's
    // index is -1 (rocketnet_client_protocol_init)
    if ( rocketnet_message_client_state( client->connection ) != ROCKETNET_CLIENT_STATE_CONNECTED )
        return -1;
    const int index = (int) rocketnet_client_transport_client_index( rocketnet_message_client_transport( client->connection ) );
    rocketnet_assert( index >= 0 && index < ROCKETNET_MAX_CLIENTS );
    return index;
}

const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    if ( !client->connection )
        return NULL;
    return rocketnet_client_transport_io_name( rocketnet_message_client_transport( client->connection ) );
}

// A message is sent only while the client is CONNECTED (standard/MESSAGES.md
// m1.5; tla/ClientSession.tla DropSend). In every other state a start is
// dropped: NULL, no log, not misuse, nothing held for later, counted once in
// NOT_CONNECTED_DROPS. With no connection object the count goes straight to
// the lifetime counters; with one, the message layer drops it and counts
// MESSAGE_SEND_NOT_CONNECTED, which the public counter reads.
uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int max_bytes )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    if ( !client->connection )
    {
        client->lifetime[RN_CLIENT_NOT_CONNECTED_DROPS]++;
        return NULL;
    }
    return rocketnet_message_client_start( client->connection, channel, (int) reliability, max_bytes );
}

// a finish with no start pending (a dropped start included) returns 0,
// counted nowhere; a refused bytes_written aborts the pending start
// (rn_message_object_finish)
uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !client->connection )
        return 0;
    return rocketnet_message_client_finish( client->connection, bytes_written );
}

void rocketnet_client_abort_message( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    if ( !client->connection )
        return;
    rocketnet_message_client_abort( client->connection );
}


const struct rocketnet_message_batch_t * rocketnet_client_receive_messages( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    client->batch.num_messages = 0;
    if ( client->connection )
        (void) rocketnet_message_client_receive_messages( client->connection, &client->batch );
    return client->batch.num_messages > 0 ? &client->batch : NULL;
}

int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    if ( !client->connection )
        return 0;
    return rocketnet_message_client_receipts( client->connection, (struct rocketnet_message_receipt_t *) out, capacity );
}

// a negative rate is an invalid argument; 0 is the endpoint's default rate. A connection that is not
// CONNECTED has no budget to set (the config's rate is its starting rate)
static enum rocketnet_status_t rn_send_kbps_runtime( const char * endpoint, int kbps, int default_kbps, int32_t * resolved )
{
    if ( kbps < 0 )
    {
        rocketnet_error( "%s set_send_kbps refused: %d kbps is negative", endpoint, kbps );
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    }
    // ROCKETNET_SEND_KBPS_UNLIMITED is no send budget: the message layer
    // holds that as 0, which uncaps the connection
    *resolved = (int32_t) ( kbps == 0 ? default_kbps : ( kbps == ROCKETNET_SEND_KBPS_UNLIMITED ? 0 : kbps ) );
    return ROCKETNET_STATUS_OK;
}

enum rocketnet_status_t rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    int32_t resolved = 0;
    const enum rocketnet_status_t range = rn_send_kbps_runtime( "client", kbps, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, &resolved );
    if ( range != ROCKETNET_STATUS_OK )
        return range;
    // not connected: nothing to set, a plain no-op
    if ( rn_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
        return ROCKETNET_STATUS_OK;
    return rocketnet_message_client_set_send_kbps( client->connection, resolved ) ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_INVALID_ARGUMENT;
}

static uint64_t rn_counter_sum( const struct rocketnet_counters_t * a, const struct rocketnet_counters_t * b, int counter )
{
    return ( a ? rocketnet_counter_value( a, counter ) : 0 ) + ( b ? rocketnet_counter_value( b, counter ) : 0 );
}

// One read of the connection's transport stats and counter blocks: every
// count of one read comes from the same moment.
static void rn_client_connection_counts( struct rocketnet_message_client_t * message_client, uint64_t counts[RN_CLIENT_COUNT], struct rocketnet_client_transport_stats_t * transport_stats )
{
    struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( message_client );
    const struct rocketnet_counters_t * transport_counters = rocketnet_client_transport_counters( transport );
    const struct rocketnet_counters_t * message_counters = rocketnet_message_client_counters( message_client );
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( transport, &stats );
    counts[RN_CLIENT_PACKETS_SENT] = stats.packets_sent;
    counts[RN_CLIENT_PACKETS_RECEIVED] = stats.packets_received;
    counts[RN_CLIENT_BYTES_SENT] = stats.bytes_sent;
    counts[RN_CLIENT_BYTES_RECEIVED] = stats.bytes_received;
    counts[RN_CLIENT_PAYLOADS_RECEIVED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED );
    counts[RN_CLIENT_MESSAGES_RESENT] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
    counts[RN_CLIENT_MESSAGES_DUPLICATE] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
    counts[RN_CLIENT_RECEIPTS_ACKED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED );
    counts[RN_CLIENT_RECEIPTS_LOST] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
    counts[RN_CLIENT_RECEIPTS_DROPPED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL );
    counts[RN_CLIENT_BUDGET_DROPS] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
    counts[RN_CLIENT_SEND_DEFERRED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED );
    counts[RN_CLIENT_NOT_CONNECTED_DROPS] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );
    if ( transport_stats )
        *transport_stats = stats;
}

void rocketnet_client_stats( const struct rocketnet_client_t * client, struct rocketnet_client_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( stats ) )
        return;
    memset( stats, 0, sizeof( *stats ) );
    const bool null_client = !rocketnet_api_null_guard( client );
    stats->api_misuses = rocketnet_api_misuse_count();
    stats->thread_priority_refusals = rocketnet_get_thread_priority_refusals();
    if ( null_client )
        return;
    uint64_t counts[RN_CLIENT_COUNT];
    memcpy( counts, client->lifetime, sizeof( counts ) );
    struct rocketnet_message_client_t * held = client->connection ? client->connection : client->retiring;
    if ( held )
    {
        uint64_t connection[RN_CLIENT_COUNT];
        struct rocketnet_client_transport_stats_t transport_stats;
        rn_client_connection_counts( held, connection, &transport_stats );
        for ( int i = 0; i < RN_CLIENT_COUNT; i++ )
            counts[i] += connection[i];
        if ( client->connection )
        {
            stats->rtt_ms = (double) transport_stats.rtt;
            stats->jitter_ms = (double) transport_stats.jitter;
            stats->packet_loss_percent = (double) transport_stats.packet_loss;
            stats->packets_sent_per_second = transport_stats.packets_sent_per_second;
            stats->packets_received_per_second = transport_stats.packets_received_per_second;
            stats->bandwidth_sent_kbps = transport_stats.bandwidth_sent_kbps;
            stats->bandwidth_received_kbps = transport_stats.bandwidth_received_kbps;
        }
    }
    stats->packets_sent = counts[RN_CLIENT_PACKETS_SENT];
    stats->packets_received = counts[RN_CLIENT_PACKETS_RECEIVED];
    stats->bytes_sent = counts[RN_CLIENT_BYTES_SENT];
    stats->bytes_received = counts[RN_CLIENT_BYTES_RECEIVED];
    stats->payloads_received = counts[RN_CLIENT_PAYLOADS_RECEIVED];
    stats->messages_resent = counts[RN_CLIENT_MESSAGES_RESENT];
    stats->messages_duplicated = counts[RN_CLIENT_MESSAGES_DUPLICATE];
    stats->receipts_acked = counts[RN_CLIENT_RECEIPTS_ACKED];
    stats->receipts_lost = counts[RN_CLIENT_RECEIPTS_LOST];
    stats->receipts_dropped = counts[RN_CLIENT_RECEIPTS_DROPPED];
    stats->messages_dropped_over_budget = counts[RN_CLIENT_BUDGET_DROPS];
    stats->messages_dropped_not_connected = counts[RN_CLIENT_NOT_CONNECTED_DROPS];
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
    int block_used;                                         // 1 once a start handed the block to a message server
    struct rocketnet_message_server_t * retiring;           // retained until stop completion
    struct rocketnet_message_server_t * running;            // NULL while stopped
    uint64_t lifetime[RN_SERVER_COUNT];                     // the counts of every earlier run
    struct rocketnet_message_batch_t batch;                 // the one receive batch across every client (in the create-time block)
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
    return server ? ( server->running ? server->running : server->retiring ) : NULL;
}

bool rocketnet_server_served_address( const struct rocketnet_server_t * server, int family, char * address, size_t address_bytes )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( address ) || address_bytes == 0 )
        return false;
    const char * served[2] = { server->address, server->transport.second_public_address };
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_address_t parsed;
        if ( served[i][0] != '\0' && rocketnet_address_parse( &parsed, served[i] ) && parsed.type == family )
        {
            rocketnet_copy_string( address, served[i], address_bytes );
            return true;
        }
    }
    return false;
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
    rocketnet_error( "server send refused: %s (destination id has the flow bit set)", rocketnet_status_name( ROCKETNET_STATUS_FLOW_ID_RESERVED ) );
    return true;
}

// a slot index for a client index: in range of max_clients while running
static bool rn_server_slot( const struct rocketnet_server_t * server, int client_index, int * slot )
{
    // an index outside the config's slots is the caller's error, counted;
    // a stopped server's slots are valid indexes that read empty
    if ( client_index < 0 || client_index >= server->max_clients )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "server call refused: client index %d is outside 0 .. %d", client_index, server->max_clients - 1 );
        return false;
    }
    if ( !server->running )
        return false;
    *slot = client_index;
    return true;
}

static void rn_server_config_from_transport_current( struct rocketnet_server_config_t * config, const struct rocketnet_server_transport_config_t * transport )
{
    memset( config, 0, sizeof( *config ) );
    config->config_bytes = sizeof( *config );
    config->protocol_id = transport->protocol_id;
    config->max_clients = transport->max_clients;
    memcpy( config->packet_filter, transport->packet_filter, sizeof( config->packet_filter ) );
    memcpy( config->backend_public_key, transport->backend_public_key, sizeof( config->backend_public_key ) );
    config->num_xdp_queues = transport->num_xdp_queues;
    config->num_os_cpus = transport->num_os_cpus;
    config->max_payload_bytes = transport->max_payload_bytes;
    config->max_fragmented_messages = transport->max_fragmented_messages;
    config->max_timeout_seconds = transport->max_timeout_seconds;
    config->handshake_deadline_seconds = transport->handshake_deadline_seconds;
    config->transport = NULL;
    config->per_client_send_kbps = ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS;
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
    sodium_memzero( &transport, sizeof( transport ) );
}

enum rocketnet_status_t rocketnet_server_default_config_internal( struct rocketnet_server_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    size_t use = 0;
    const enum rocketnet_status_t status = rn_public_config_bytes( "server", config->config_bytes, sizeof( struct rocketnet_server_config_t ), &use );
    if ( status != ROCKETNET_STATUS_OK )
        return status;
    struct rocketnet_server_config_t defaults;
    rn_server_default_config_current( &defaults );
    rn_public_config_copy( config, &defaults, use );
    return ROCKETNET_STATUS_OK;
}

static enum rocketnet_status_t rn_server_config_read( rocketnet_server_config_t * current,
                                                      const rocketnet_server_config_t * config,
                                                      const rocketnet_server_config_t ** read )
{
    *read = NULL;
    if ( !config )
        return ROCKETNET_STATUS_OK;
    size_t use = 0;
    const enum rocketnet_status_t status = rn_public_config_bytes( "server", config->config_bytes, sizeof( *current ), &use );
    if ( status != ROCKETNET_STATUS_OK )
        return status;
    rn_server_default_config_current( current );
    rn_public_config_copy( current, config, use );
    *read = current;
    return ROCKETNET_STATUS_OK;
}

// The one validation behind create: the transport table's
// ranges, the public link simulator's posture and the transport list's parse,
// each the check start runs on the same values
// (rocketnet_server_transport_create_internal),
// then the message config's rules. The keys are not checked here; start
// refuses a config with none. Each refusal logs the rule it broke.
static enum rocketnet_status_t rn_server_config_valid( const struct rocketnet_server_transport_config_t * transport, const struct rocketnet_message_config_t * message )
{
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "server", transport, fields, num_fields ) )
        return ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
    if ( !rocketnet_config_validate_public_link_simulator( "server", transport, fields, num_fields, rocketnet_public_link_simulator_development() ) )
        return ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
    if ( rocketnet_config_packet_filter_refused( "server", transport->packet_filter ) )
        return ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION;
    const int requested_payload = transport->max_payload_bytes > 0 ? transport->max_payload_bytes : ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;
    const int requested_slots = transport->max_fragmented_messages > 0 ? transport->max_fragmented_messages : ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    const int requested_clients = transport->max_clients > 0 ? transport->max_clients : ROCKETNET_MAX_CLIENTS;
    if ( !rocketnet_reassembly_memory_allowed_delivered( requested_clients, requested_slots, requested_payload,
            rocketnet_server_reassembly_delivered_capacity( requested_clients, requested_slots ),
            rocketnet_server_reassembly_delivered_bytes( requested_clients, requested_slots, requested_payload ) ) )
        return ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED;
    // Resolve the transport env override here as start will, so public
    // create and required_memory validate the same candidate list.
    struct rocketnet_server_transport_config_t effective = *transport;
    for ( int i = 0; i < num_fields; ++i )
        if ( strcmp( fields[i].name, "transport" ) == 0 &&
             !rocketnet_config_apply_env( "server", &effective, &fields[i], 1 ) )
            return ROCKETNET_STATUS_CONFIG_ENV_INVALID;
    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    struct rocketnet_transport_ladder_t ladder;
    if ( !rocketnet_transport_ladder_parse( effective.transport, available, num_available, &ladder ) )
        return ROCKETNET_STATUS_INVALID_TRANSPORT_LIST;
    return rocketnet_message_validate_config( message ) != 0 ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
}

static size_t rn_server_required_memory_current( const struct rocketnet_server_config_t * config )

{
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_server_split( config, true, &transport, &message, &kbps ) )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        return 0;
    }
    if ( rn_server_config_valid( &transport, &message ) != ROCKETNET_STATUS_OK )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        return 0;
    }
    const uint64_t running_bytes = rocketnet_message_server_required_memory( &transport, &message );
    sodium_memzero( &transport, sizeof( transport ) );
    const uint64_t total = running_bytes == 0 ? 0 : rn_server_header_bytes + running_bytes;
    return total > (uint64_t) SIZE_MAX ? 0 : (size_t) total;
}

size_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config )
{
    struct rocketnet_server_config_t current;
    const struct rocketnet_server_config_t * read = NULL;
    size_t result = 0;
    if ( rn_server_config_read( &current, config, &read ) == ROCKETNET_STATUS_OK )
        result = rn_server_required_memory_current( read );

    sodium_memzero( &current, sizeof( current ) );

    return result;
}

// The addresses a server serves: create's IPv4 and IPv6 arguments, each
// overridden by ROCKETNET_SERVER_IPV4_ADDRESS or ROCKETNET_SERVER_IPV6_ADDRESS
// when that is set, for every server created. Either may be NULL, not both;
// each must parse as its own family. The first served address is the one the
// transport binds; with both, the IPv6 one is the second family on the same
// port.
static enum rocketnet_status_t rn_server_addresses( const char * ipv4_address, const char * ipv6_address,
                                                    char * primary, size_t primary_bytes, char * second, size_t second_bytes )
{
    char ipv4_env[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char ipv6_env[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    const int ipv4_found = rocketnet_platform_getenv( "ROCKETNET_SERVER_IPV4_ADDRESS", ipv4_env, sizeof( ipv4_env ) );
    const int ipv6_found = rocketnet_platform_getenv( "ROCKETNET_SERVER_IPV6_ADDRESS", ipv6_env, sizeof( ipv6_env ) );
    if ( ipv4_found < 0 || ipv6_found < 0 )
    {
        rocketnet_error( "server create refused: ROCKETNET_SERVER_IPV%s_ADDRESS is too long", ipv4_found < 0 ? "4" : "6" );
        return ROCKETNET_STATUS_CONFIG_ENV_INVALID;
    }
    if ( ipv4_found == 1 && ipv4_env[0] == '\0' )
        rocketnet_info( "server config ignores empty ROCKETNET_SERVER_IPV4_ADDRESS (set a value, or unset the variable)" );
    if ( ipv6_found == 1 && ipv6_env[0] == '\0' )
        rocketnet_info( "server config ignores empty ROCKETNET_SERVER_IPV6_ADDRESS (set a value, or unset the variable)" );
    if ( ipv4_found == 1 && ipv4_env[0] != '\0' )
    {
        rocketnet_info( "server config override ROCKETNET_SERVER_IPV4_ADDRESS=%s (displaces %s)", ipv4_env, ipv4_address ? ipv4_address : "none" );
        ipv4_address = ipv4_env;
    }
    if ( ipv6_found == 1 && ipv6_env[0] != '\0' )
    {
        rocketnet_info( "server config override ROCKETNET_SERVER_IPV6_ADDRESS=%s (displaces %s)", ipv6_env, ipv6_address ? ipv6_address : "none" );
        ipv6_address = ipv6_env;
    }
    if ( !ipv4_address && !ipv6_address )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "server create refused: no address; pass an IPv4 address, an IPv6 address, or both" );
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    }
    const char * addresses[2] = { ipv4_address, ipv6_address };
    const int families[2] = { ROCKETNET_ADDRESS_IPV4, ROCKETNET_ADDRESS_IPV6 };
    for ( int i = 0; i < 2; i++ )
    {
        if ( !addresses[i] )
            continue;
        struct rocketnet_address_t parsed;
        if ( memchr( addresses[i], '\0', ROCKETNET_MAX_ADDRESS_STRING_BYTES ) == NULL || !rocketnet_address_parse( &parsed, addresses[i] ) || parsed.type != families[i] )
        {
            rocketnet_error( "server create refused: the %s address does not parse as one", i == 0 ? "IPv4" : "IPv6" );
            return ROCKETNET_STATUS_INVALID_ADDRESS;
        }
    }
    const char * first = ipv4_address ? ipv4_address : ipv6_address;
    const char * other = ipv4_address && ipv6_address ? ipv6_address : "";
    rocketnet_copy_string( primary, first, primary_bytes );
    rocketnet_copy_string( second, other, second_bytes );
    return ROCKETNET_STATUS_OK;
}

static struct rocketnet_server_t * rn_server_create_current( void * context, const char * ipv4_address, const char * ipv6_address, const struct rocketnet_server_config_t * config, void * memory, size_t memory_bytes, enum rocketnet_status_t * status )
{
    if ( status )
        *status = ROCKETNET_STATUS_OK;
    // the lifecycle state is read once here, before any allocation; a create
    // before rocketnet_init is refused, logged and counted, as the transport
    // create refuses it at start (rocketnet_server_transport_create_internal)
    if ( !rocketnet_library_initialized() )
    {
        rocketnet_error( "rocketnet_server_create: rocketnet_init must be called first" );
        rocketnet_api_misuse_record();
        if ( status )
            *status = ROCKETNET_STATUS_NOT_INITIALIZED;
        return NULL;
    }
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char second_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    const enum rocketnet_status_t address_status = rn_server_addresses( ipv4_address, ipv6_address, address, sizeof( address ), second_address, sizeof( second_address ) );
    if ( address_status != ROCKETNET_STATUS_OK )
    {
        if ( status )
            *status = address_status;
        return NULL;
    }
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    enum rocketnet_status_t failure = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
    if ( !rn_server_split( config, true, &transport, &message, &kbps, &failure ) )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        if ( status )
            *status = failure;
        return NULL;
    }
    rocketnet_copy_string( transport.second_public_address, second_address, sizeof( transport.second_public_address ) );
    const enum rocketnet_status_t valid = rn_server_config_valid( &transport, &message );
    if ( valid != ROCKETNET_STATUS_OK )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        if ( status )
            *status = valid;
        return NULL;
    }
    const uint64_t running_bytes = rocketnet_message_server_required_memory( &transport, &message );
    if ( running_bytes == 0 )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
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
            if ( status )
                *status = ROCKETNET_STATUS_INVALID_ARGUMENT;
            return NULL;
        }
    }
    else
    {
        base = (uint8_t *) rocketnet_malloc( context, (size_t) total );
        if ( !base )
        {
            sodium_memzero( &transport, sizeof( transport ) );
            if ( status )
                *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
            return NULL;
        }
    }
    // the server's own X25519 keypair, fresh for this object: the game reads
    // the public key (rocketnet_server_public_key) and registers it with its
    // backend; destroy wipes the block, the secret with it
    if ( rocketnet_generate_server_keypair( transport.server_public_key, transport.server_secret_key ) != ROCKETNET_STATUS_OK )
    {
        sodium_memzero( &transport, sizeof( transport ) );
        if ( !memory )
            rocketnet_free( context, base );
        if ( status )
            *status = ROCKETNET_STATUS_CRYPTO_INIT_FAILED;
        return NULL;
    }
    struct rocketnet_server_t * server = (struct rocketnet_server_t *) base;
    memset( server, 0, offsetof( struct rocketnet_server_t, batch ) );
    server->batch.num_messages = 0;
    server->context = context;
    {
        // server is zeroed above, so the copy stays NUL-terminated.
        const size_t address_length = strlen( address );
        const size_t address_capacity = sizeof( server->address ) - 1;
        memcpy( server->address, address, address_length < address_capacity ? address_length : address_capacity );
    }
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

struct rocketnet_server_t * rocketnet_server_create( void * context, const char * ipv4_address, const char * ipv6_address, const struct rocketnet_server_config_t * config, enum rocketnet_status_t * status )
{
    if ( status )
        *status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_config_t current;
    const struct rocketnet_server_config_t * read = NULL;
    const enum rocketnet_status_t read_status = rn_server_config_read( &current, config, &read );
    if ( read_status != ROCKETNET_STATUS_OK )
    {
        sodium_memzero( &current, sizeof( current ) );
        if ( status )
            *status = read_status;
        return NULL;
    }
    struct rocketnet_server_t * result = rn_server_create_current( context, ipv4_address, ipv6_address, read, NULL, 0, status );

    sodium_memzero( &current, sizeof( current ) );

    return result;
}

static void rn_server_finish_stop( struct rocketnet_server_t * server );

enum rocketnet_status_t rocketnet_server_start( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    if ( server->retiring )
    {
        if ( !rocketnet_server_stopped( server ) )
            return ROCKETNET_STATUS_SERVER_NOT_STOPPED;
        rn_server_finish_stop( server );
    }
    if ( server->running )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "server start refused: the server is already running" );
        return ROCKETNET_STATUS_SERVER_NOT_STOPPED;
    }
    int status = ROCKETNET_STATUS_OK;
    server->block_used = 1;
    server->running = rocketnet_message_server_create_ex( server->context, server->address, &server->transport, &server->message,
                                                          server->block, server->block_bytes, server->kbps, &status, server->message.server_send_kbps <= 0 );
    if ( server->running )
        return ROCKETNET_STATUS_OK;
    return status == ROCKETNET_STATUS_OK ? ROCKETNET_STATUS_INVALID_ARGUMENT : (enum rocketnet_status_t) status;
}

static void rn_server_running_counts( const struct rocketnet_server_t * server, uint64_t counts[RN_SERVER_COUNT], struct rocketnet_server_transport_stats_t * transport_stats );

// Every slot as rocketnet_server_disconnect_client ends one: a slot the
// message layer holds CONNECTED is kicked (rn_message_object_kick), which
// cancels its open in-place start,
// marks it KICKED with reason NONE and disconnects it at the transport through
// the link; a slot the transport holds connected that the message layer has
// not yet observed is disconnected at the transport (tla/RNSlots.tla
// PublicDisconnect). The whole loop is one step: every connected slot ends in
// this same call, and a slot still in the handshake is left as it is
// (tla/ServerSession.tla DisconnectAll).
void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( !transport )
        return;
    for ( int i = 0; i < server->max_clients; i++ )
    {
        rn_message_object_kick( &server->running->object, i, ROCKETNET_DISCONNECT_REASON_NONE );
        if ( rocketnet_server_transport_is_client_connected( transport, i ) )
            rocketnet_server_transport_disconnect_client( transport, i );
    }
}

void rocketnet_server_stop( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) || !server->running )
        return;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( transport )
    {
        for ( int i = 0; i < server->max_clients; i++ )
        {
            // Disconnect the transport first on the shutdown-only batch path.
            // The message kick's regular link callback then finds a free slot.
            if ( rocketnet_server_transport_is_client_connected( transport, i ) )
                rocketnet_server_transport_disconnect_client_shutdown( transport, i );
            rn_message_object_kick( &server->running->object, i, ROCKETNET_DISCONNECT_REASON_NONE );
        }
        rocketnet_server_transport_send_packets_shutdown( transport );
    }
    else
    {
        rocketnet_server_disconnect_all_clients( server );
        rocketnet_message_server_send_packets( server->running );
    }
    server->retiring = server->running;
    server->running = NULL;
    if ( transport )
        rocketnet_server_transport_stop( transport );
}

bool rocketnet_server_stopped( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) || server->running )
        return false;
    struct rocketnet_server_transport_t * transport = server->retiring ? rocketnet_message_server_transport( server->retiring ) : NULL;
    return !transport || rocketnet_server_transport_stopped( transport );
}

static void rn_server_finish_stop( struct rocketnet_server_t * server )
{
    if ( !server->retiring )
        return;
    rocketnet_assert( rocketnet_server_stopped( server ) );
    server->running = server->retiring;
    uint64_t counts[RN_SERVER_COUNT];
    rn_server_running_counts( server, counts, NULL );
    for ( int i = 0; i < RN_SERVER_COUNT; i++ )
        server->lifetime[i] += counts[i];
    for ( int i = 0; i < server->max_clients; i++ )
        rocketnet_server_connection_reset( &server->running->object, i );
    rocketnet_message_server_destroy( server->running );
    server->running = NULL;
    server->retiring = NULL;
}

bool rocketnet_server_running( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return false;
    return server->running != NULL;
}

void rocketnet_server_destroy( struct rocketnet_server_t * server )
{
    if ( !server )
        return;                                         // as free(NULL): silent, uncounted
    rocketnet_assert( rocketnet_server_stopped( server ) );
    if ( !rocketnet_server_stopped( server ) )
        return;
    rn_server_finish_stop( server );
    // the header, keys included, and the running block after it, which held
    // every connection's message bytes: all of it is wiped, then freed or
    // given back (an adopted server is the header alone). A block no start
    // ever used holds nothing, and wiping it would touch every page of a plan
    // that can reach 13 GB.
    void * context = server->context;
    const int owns_block = server->owns_block;
    const size_t bytes = server->block ? (size_t) ( rn_server_header_bytes + ( server->block_used ? server->block_bytes : 0 ) ) : sizeof( *server );
    sodium_memzero( server, bytes );
    if ( owns_block )
        rocketnet_free( context, server );
}

void rocketnet_server_update( struct rocketnet_server_t * server, double time )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    // a stopped server has nothing to update or send: a plain no-op
    if ( !server->running )
        return;
    (void) rocketnet_message_server_update( server->running, time );
}

void rocketnet_server_send_packets( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    // a stopped server has nothing to update or send: a plain no-op
    if ( !server->running )
        return;
    rocketnet_message_server_send_packets( server->running );
}

void rocketnet_server_public_key( const struct rocketnet_server_t * server, uint8_t * public_key )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( public_key ) )
        return;
    memcpy( public_key, server->transport.server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES );
}

static bool rn_server_family_address( const struct rocketnet_server_t * server, int family, char * address, size_t address_bytes )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( address ) )
        return false;
    if ( address_bytes > 0 )
        address[0] = '\0';
    if ( !server->running )
        return false;
    struct rocketnet_address_t advertised;
    if ( !rocketnet_server_transport_advertised_address( rn_server_transport( server ), family, &advertised ) )
        return false;
    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_address_to_string( &advertised, buffer );
    const size_t length = strlen( buffer );
    if ( length + 1 > address_bytes )
    {
        rocketnet_error( "server address refused: the buffer is %d bytes, the address needs %d", (int) address_bytes, (int) ( length + 1 ) );
        return false;
    }
    memcpy( address, buffer, length + 1 );
    return true;
}

bool rocketnet_server_ipv4_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes )
{
    return rn_server_family_address( server, ROCKETNET_ADDRESS_IPV4, address, address_bytes );
}

bool rocketnet_server_ipv6_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes )
{
    return rn_server_family_address( server, ROCKETNET_ADDRESS_IPV6, address, address_bytes );
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
    return rocketnet_server_transport_io_name( rn_server_transport( server ) );
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

bool rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return false;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return false;
    return rocketnet_message_server_client_connected( rn_server( server ), slot ) != 0;
}

uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return 0;
    return rocketnet_server_transport_client_id( rn_server_transport( server ), slot );
}

// The slot ends at once, before the next update, in every query and send: a
// slot the message layer holds CONNECTED is kicked (rn_message_object_kick),
// which cancels its open in-place start,
// marks it KICKED with reason NONE, so rocketnet_server_client_connected reads
// false and a start drops as not connected, and disconnects it at the
// transport through the link; a slot the transport holds connected that the
// message layer has not yet observed is disconnected at the transport, which
// returns at once for a slot it does not hold
// (rocketnet_server_transport_disconnect_client; tla/RNSlots.tla
// PublicDisconnect).
void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return;
    rn_message_object_kick( &server->running->object, slot, ROCKETNET_DISCONNECT_REASON_NONE );
    rocketnet_server_transport_disconnect_client( rn_server_transport( server ), slot );
}

enum rocketnet_disconnect_reason_t rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_DISCONNECT_REASON_NONE;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return ROCKETNET_DISCONNECT_REASON_NONE;
    return (enum rocketnet_disconnect_reason_t) rocketnet_message_server_disconnect_reason( rn_server( server ), slot );
}

// A message is sent only to a CONNECTED client (standard/MESSAGES.md m1.5;
// tla/ServerSession.tla ServerDropSend). An index below max_clients with no
// CONNECTED client is dropped: NULL, not misuse, nothing held for later,
// counted once in NOT_CONNECTED_DROPS (the message layer's
// MESSAGE_SEND_NOT_CONNECTED while running, the lifetime counters while
// stopped). An index at or past max_clients is misuse.
uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, enum rocketnet_reliability_t reliability, int max_bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    if ( rn_server_destination_refused( destination_id ) )
        return NULL;
    if ( destination_id >= (uint64_t) server->max_clients )
    {
        rocketnet_api_misuse_record();
        return NULL;
    }
    if ( !server->running )
    {
        server->lifetime[RN_SERVER_NOT_CONNECTED_DROPS]++;
        return NULL;
    }
    return rocketnet_message_server_start( rn_server( server ), (int) destination_id, channel, reliability, max_bytes );
}

// finish and abort act on the one pending start (rn_message_object_t
// pending_connection), so they take no destination. A finish with no start
// pending (a dropped start included) returns 0, counted nowhere; a refused
// bytes_written aborts the pending start (rn_message_object_finish).
static int rn_server_pending_slot( const struct rocketnet_server_t * server )
{
    if ( !server->running )
        return -1;
    return server->running->object.pending_connection;
}

uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, int bytes_written )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    const int slot = rn_server_pending_slot( server );
    if ( slot < 0 )
        return 0;
    return rocketnet_message_server_finish( rn_server( server ), slot, bytes_written );
}

void rocketnet_server_abort_message( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    const int slot = rn_server_pending_slot( server );
    if ( slot < 0 )
        return;
    rocketnet_message_server_abort( rn_server( server ), slot );
}

const struct rocketnet_message_batch_t * rocketnet_server_receive_messages( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    server->batch.num_messages = 0;
    if ( server->running )
        (void) rocketnet_message_server_receive_messages( rn_server( server ), &server->batch );
    return server->batch.num_messages > 0 ? &server->batch : NULL;
}

int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return 0;
    return rocketnet_message_server_receipts( rn_server( server ), slot, (struct rocketnet_message_receipt_t *) out, capacity );
}

enum rocketnet_status_t rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    int32_t resolved = 0;
    const enum rocketnet_status_t range = rn_send_kbps_runtime( "server", kbps, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, &resolved );
    if ( range != ROCKETNET_STATUS_OK )
        return range;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
    {
        // an index outside 0 .. max_clients - 1 is misuse; a stopped server
        // has nothing to set, a plain no-op
        return ( client_index < 0 || client_index >= server->max_clients ) ? ROCKETNET_STATUS_INVALID_ARGUMENT : ROCKETNET_STATUS_OK;
    }
    // a slot with no CONNECTED client has no budget to set, a plain no-op:
    // the slot's reset starts each connection at the config's rate
    if ( !rocketnet_message_server_client_connected( rn_server( server ), slot ) )
        return ROCKETNET_STATUS_OK;
    return rocketnet_message_server_set_send_kbps( rn_server( server ), slot, resolved ) ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_INVALID_ARGUMENT;
}

// One read of the running server's transport stats and counter blocks.
static void rn_server_running_counts( const struct rocketnet_server_t * server, uint64_t counts[RN_SERVER_COUNT], struct rocketnet_server_transport_stats_t * transport_stats )
{
    memset( counts, 0, sizeof( uint64_t ) * RN_SERVER_COUNT );
    if ( transport_stats )
        memset( transport_stats, 0, sizeof( *transport_stats ) );
    struct rocketnet_message_server_t * held = server->running ? server->running : server->retiring;
    struct rocketnet_server_transport_t * transport = held ? rocketnet_message_server_transport( held ) : NULL;
    if ( !transport )
        return;
    const struct rocketnet_counters_t * transport_counters = rocketnet_server_transport_counters( transport );
    const struct rocketnet_counters_t * message_counters = rocketnet_message_server_counters( held );
    struct rocketnet_server_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_stats( transport, &stats );
    counts[RN_SERVER_PACKETS_SENT] = stats.packets_sent;
    counts[RN_SERVER_PACKETS_RECEIVED] = stats.packets_received;
    counts[RN_SERVER_BYTES_SENT] = stats.bytes_sent;
    counts[RN_SERVER_BYTES_RECEIVED] = stats.bytes_received;
    counts[RN_SERVER_CLIENTS_CONNECTED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_CONNECTED );
    counts[RN_SERVER_CLIENTS_DENIED_FULL] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS ) +
                                            rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS );
    counts[RN_SERVER_HANDSHAKES_OVER_BUDGET] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED );
    counts[RN_SERVER_PAYLOADS_RECEIVED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_PAYLOAD_DELIVERED );
    counts[RN_SERVER_MESSAGES_RESENT] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
    counts[RN_SERVER_MESSAGES_DUPLICATE] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
    counts[RN_SERVER_RECEIPTS_ACKED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED );
    counts[RN_SERVER_RECEIPTS_LOST] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
    counts[RN_SERVER_RECEIPTS_DROPPED] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL );
    counts[RN_SERVER_BUDGET_DROPS] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
    counts[RN_SERVER_NOT_CONNECTED_DROPS] = rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );
    if ( transport_stats )
        *transport_stats = stats;
}

void rocketnet_server_stats( const struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( stats ) )
        return;
    memset( stats, 0, sizeof( *stats ) );
    const bool null_server = !rocketnet_api_null_guard( server );
    stats->api_misuses = rocketnet_api_misuse_count();
    stats->thread_priority_refusals = rocketnet_get_thread_priority_refusals();
    if ( null_server )
        return;
    uint64_t counts[RN_SERVER_COUNT];
    struct rocketnet_server_transport_stats_t transport_stats;
    rn_server_running_counts( server, counts, &transport_stats );
    for ( int i = 0; i < RN_SERVER_COUNT; i++ )
        counts[i] += server->lifetime[i];
    if ( server->running )
    {
        stats->packets_sent_per_second = (double) transport_stats.packets_sent_per_second;
        stats->packets_received_per_second = (double) transport_stats.packets_received_per_second;
        stats->bandwidth_sent_kbps = (double) transport_stats.bandwidth_sent_kbps;
        stats->bandwidth_received_kbps = (double) transport_stats.bandwidth_received_kbps;
    }
    stats->packets_sent = counts[RN_SERVER_PACKETS_SENT];
    stats->packets_received = counts[RN_SERVER_PACKETS_RECEIVED];
    stats->bytes_sent = counts[RN_SERVER_BYTES_SENT];
    stats->bytes_received = counts[RN_SERVER_BYTES_RECEIVED];
    stats->connections_accepted = counts[RN_SERVER_CLIENTS_CONNECTED];
    stats->connections_denied_full = counts[RN_SERVER_CLIENTS_DENIED_FULL];
    stats->handshakes_dropped_over_budget = counts[RN_SERVER_HANDSHAKES_OVER_BUDGET];
    stats->payloads_received = counts[RN_SERVER_PAYLOADS_RECEIVED];
    stats->messages_resent = counts[RN_SERVER_MESSAGES_RESENT];
    stats->messages_duplicated = counts[RN_SERVER_MESSAGES_DUPLICATE];
    stats->receipts_acked = counts[RN_SERVER_RECEIPTS_ACKED];
    stats->receipts_lost = counts[RN_SERVER_RECEIPTS_LOST];
    stats->receipts_dropped = counts[RN_SERVER_RECEIPTS_DROPPED];
    stats->messages_dropped_over_budget = counts[RN_SERVER_BUDGET_DROPS];
    stats->messages_dropped_not_connected = counts[RN_SERVER_NOT_CONNECTED_DROPS];
}

#if ROCKETNET_DEV
// The delivery capture and counter blocks of rocketnet_layers.h: development
// builds only, for the load tools; not public API.
void rocketnet_server_delivery_capture( struct rocketnet_server_t * server, int client_index, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( capture ) )
        return;
    memset( capture, 0, sizeof( *capture ) );
    if ( !rocketnet_api_null_guard( server ) )
        return;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) || !server->running )
        return;
    rocketnet_server_transport_delivery_capture( rn_server_transport( server ), slot, capture );
    if ( capture->connected )
        rocketnet_message_server_delivery_capture( server->running, slot, capture );
}

void rocketnet_client_delivery_capture( struct rocketnet_client_t * client, struct rocketnet_delivery_capture_t * capture )
{
    if ( !rocketnet_api_null_guard( capture ) )
        return;
    memset( capture, 0, sizeof( *capture ) );
    if ( !rocketnet_api_null_guard( client ) || !client->connection )
        return;
    rocketnet_client_transport_delivery_capture( rocketnet_message_client_transport( client->connection ), capture );
    if ( capture->connected )
        rocketnet_message_client_delivery_capture( client->connection, capture );
}

const struct rocketnet_counters_t * rocketnet_server_transport_counter_block( struct rocketnet_server_t * server )
{
    struct rocketnet_message_server_t * held = rocketnet_server_message_server( server );
    struct rocketnet_server_transport_t * transport = held ? rocketnet_message_server_transport( held ) : NULL;
    return transport ? rocketnet_server_transport_counters( transport ) : NULL;
}

const struct rocketnet_counters_t * rocketnet_server_message_counter_block( struct rocketnet_server_t * server )
{
    struct rocketnet_message_server_t * held = rocketnet_server_message_server( server );
    return held ? rocketnet_message_server_counters( held ) : NULL;
}

const struct rocketnet_counters_t * rocketnet_client_transport_counter_block( struct rocketnet_client_t * client )
{
    struct rocketnet_message_client_t * connection = client ? client->connection : NULL;
    struct rocketnet_client_transport_t * transport = connection ? rocketnet_message_client_transport( connection ) : NULL;
    return transport ? rocketnet_client_transport_counters( transport ) : NULL;
}

const struct rocketnet_counters_t * rocketnet_client_message_counter_block( struct rocketnet_client_t * client )
{
    struct rocketnet_message_client_t * connection = client ? client->connection : NULL;
    return connection ? rocketnet_message_client_counters( connection ) : NULL;
}
#endif // #if ROCKETNET_DEV

void rocketnet_server_client_stats( const struct rocketnet_server_t * server, int client_index, struct rocketnet_server_client_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( stats ) )
        return;
    memset( stats, 0, sizeof( *stats ) );
    if ( !rocketnet_api_null_guard( server ) )
        return;
    int slot;
    if ( !rn_server_slot( server, client_index, &slot ) )
        return;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( !rocketnet_server_transport_is_client_connected( transport, slot ) )
        return;
    struct rocketnet_server_transport_client_stats_t transport_stats;
    memset( &transport_stats, 0, sizeof( transport_stats ) );
    rocketnet_server_transport_client_stats( transport, slot, &transport_stats );
    stats->packets_sent = transport_stats.packets_sent;
    stats->packets_received = transport_stats.packets_received;
    stats->bytes_sent = transport_stats.bytes_sent;
    stats->bytes_received = transport_stats.bytes_received;
    stats->rtt_ms = (double) transport_stats.rtt;
    stats->jitter_ms = (double) transport_stats.jitter;
    stats->packet_loss_percent = (double) transport_stats.packet_loss;
    stats->bandwidth_sent_kbps = (double) transport_stats.bandwidth_sent_kbps;
    stats->bandwidth_received_kbps = (double) transport_stats.bandwidth_received_kbps;
    stats->bandwidth_acked_kbps = (double) transport_stats.bandwidth_acked_kbps;
}

// ---------------------------------------------------------------------------
// The development network simulator: a development library's calls, and a
// unit-test build's (the test hooks switch the development posture on).
//
// The setting applies at once to a live connection and to a running server:
// the call hands it to the thread that owns the simulator as a config record
// under a tiny mutex, and that thread takes it before its next send. The
// connection's threads do not move, and a production library refuses the
// simulator and carries none of its memory.

#if ROCKETNET_DEV || defined(ROCKETNET_LINK_SIMULATOR_TEST_HOOKS)

static bool rn_network_simulator_valid( const char * who, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent )
{
    if ( !rocketnet_public_link_simulator_development() )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "%s network simulator refused: a production library has no network simulator", who );
        return false;
    }
    if ( latency_ms < 0 || latency_ms > 60000 || jitter_ms < 0 || jitter_ms > 60000 ||
         packet_loss_percent < 0 || packet_loss_percent > 100 || duplicate_percent < 0 || duplicate_percent > 100 )
    {
        rocketnet_api_misuse_record();
        rocketnet_error( "%s network simulator refused: latency and jitter are 0 .. 60000 ms, loss and duplicate 0 .. 100 percent", who );
        return false;
    }
    return true;
}

void rocketnet_client_network_simulator( struct rocketnet_client_t * client, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    if ( !rn_network_simulator_valid( "client", latency_ms, jitter_ms, packet_loss_percent, duplicate_percent ) )
        return;
    client->transport.simulated_latency_ms = latency_ms;
    client->transport.simulated_jitter_ms = jitter_ms;
    client->transport.simulated_loss_percent = packet_loss_percent;
    client->transport.simulated_duplicate_percent = duplicate_percent;
    if ( client->connection )
        (void) rocketnet_client_transport_network_simulator( rocketnet_message_client_transport( client->connection ), latency_ms, jitter_ms, packet_loss_percent, duplicate_percent );
}

void rocketnet_server_network_simulator( struct rocketnet_server_t * server, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    if ( !rn_network_simulator_valid( "server", latency_ms, jitter_ms, packet_loss_percent, duplicate_percent ) )
        return;
    server->transport.simulated_latency_ms = latency_ms;
    server->transport.simulated_jitter_ms = jitter_ms;
    server->transport.simulated_loss_percent = packet_loss_percent;
    server->transport.simulated_duplicate_percent = duplicate_percent;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( transport )
        (void) rocketnet_server_transport_network_simulator( transport, latency_ms, jitter_ms, packet_loss_percent, duplicate_percent );
}

#endif // ROCKETNET_DEV || defined(ROCKETNET_LINK_SIMULATOR_TEST_HOOKS)
