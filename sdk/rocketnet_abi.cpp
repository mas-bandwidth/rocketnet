/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_internal.h"
#include <string.h>

struct abi_field_t { const char * name; size_t offset; size_t size; };
struct abi_type_t { const char * name; size_t size; const abi_field_t * fields; int count; };

// Offsets and sizes come from the compiler, including target ABI padding.
#define FIELD(name) { #name, offsetof(ABI_TYPE, name), sizeof(((ABI_TYPE*)0)->name) },
#define INT_FIELD(name, def, min, max, flags) FIELD(name)
#define UINT64_FIELD(name, def, flags) FIELD(name)
#define BYTES_FIELD(name, size, def, flags) FIELD(name)
#define STRING_FIELD(name, capacity, def, flags) FIELD(name)

#define ABI_TYPE rocketnet_client_config_t
static const abi_field_t client_config_fields[] =
{
    FIELD(filter_value) FIELD(max_payload_bytes) FIELD(num_reassembly_slots) FIELD(transport)
    FIELD(state_change_callback) FIELD(time_callback) FIELD(send_kbps)
    FIELD(num_channels) FIELD(window_messages) FIELD(receive_ring_bytes) FIELD(max_message_bytes)
    FIELD(send_buffer_bytes) FIELD(send_queue_messages) FIELD(packets_in_flight) FIELD(max_packets_per_update)
    FIELD(unreliable_deliveries) FIELD(sequenced_holds) FIELD(receipt_ring) FIELD(rto_initial_ms)
    FIELD(rto_min_ms) FIELD(rto_max_ms) FIELD(reliable_timeout_ms)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_server_config_t
static const abi_field_t server_config_fields[] =
{
    FIELD(protocol_id) FIELD(max_clients) FIELD(filter_value) FIELD(backend_public_key) FIELD(server_public_key)
    FIELD(server_secret_key) FIELD(num_xdp_queues) FIELD(num_os_cpus) FIELD(max_payload_bytes)
    FIELD(num_reassembly_slots) FIELD(max_timeout_seconds) FIELD(handshake_deadline_seconds)
    FIELD(transport) FIELD(public_address) FIELD(per_client_send_kbps)
    FIELD(num_channels) FIELD(window_messages) FIELD(receive_ring_bytes) FIELD(max_message_bytes)
    FIELD(send_buffer_bytes) FIELD(send_queue_messages) FIELD(packets_in_flight) FIELD(max_packets_per_update)
    FIELD(unreliable_deliveries) FIELD(sequenced_holds) FIELD(receipt_ring) FIELD(rto_initial_ms)
    FIELD(rto_min_ms) FIELD(rto_max_ms) FIELD(reliable_timeout_ms)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_receipt_t
static const abi_field_t receipt_fields[] =
{
    FIELD(serial)
    FIELD(status)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_client_transport_config_t
static const abi_field_t client_transport_config_fields[] =
{
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE(INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD)
    FIELD(state_change_callback)
    FIELD(time_callback)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_server_transport_config_t
static const abi_field_t server_transport_config_fields[] =
{
    ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE(INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_client_transport_stats_t
static const abi_field_t client_transport_stats_fields[] =
{
    FIELD(rtt)
    FIELD(jitter)
    FIELD(packet_loss)
    FIELD(packets_sent_per_second)
    FIELD(packets_received_per_second)
    FIELD(bandwidth_sent_kbps)
    FIELD(bandwidth_received_kbps)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_server_transport_stats_t
static const abi_field_t server_transport_stats_fields[] =
{
    FIELD(packets_sent_per_second)
    FIELD(packets_received_per_second)
    FIELD(bandwidth_sent_kbps)
    FIELD(bandwidth_received_kbps)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_server_transport_client_stats_t
static const abi_field_t server_transport_client_stats_fields[] =
{
    FIELD(rtt)
    FIELD(jitter)
    FIELD(packet_loss)
    FIELD(bandwidth_sent_kbps)
    FIELD(bandwidth_received_kbps)
    FIELD(bandwidth_acked_kbps)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_packet_context_t
static const abi_field_t packet_context_fields[] =
{
    FIELD(frame)
    FIELD(payload_index)
    FIELD(payload_capacity)
    FIELD(packet_id)
    FIELD(sequence)
    FIELD(client_index)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_message_config_t
static const abi_field_t message_config_fields[] =
{
    ROCKETNET_MESSAGE_CONFIG_TABLE(INT_FIELD, UINT64_FIELD, BYTES_FIELD, STRING_FIELD)
};
#undef ABI_TYPE

#define ABI_TYPE rocketnet_frame_sync_stats_t
static const abi_field_t frame_sync_stats_fields[] =
{
    FIELD(frame)
    FIELD(dropped_frames)
    FIELD(priority_held)
    FIELD(spinning)
    FIELD(guard_seconds)
    FIELD(wakeup_error_seconds)
};
#undef ABI_TYPE

#define TYPE(name) { "rocketnet_" #name "_t", sizeof(rocketnet_##name##_t), name##_fields, int(sizeof(name##_fields)/sizeof(abi_field_t)) },
static const abi_type_t types[] =
{
    TYPE(client_config)
    TYPE(server_config)
    TYPE(receipt)
    TYPE(client_transport_config)
    TYPE(server_transport_config)
    TYPE(client_transport_stats)
    TYPE(server_transport_stats)
    TYPE(server_transport_client_stats)
    TYPE(packet_context)
    TYPE(frame_sync_stats)
    TYPE(message_config)
};

static const abi_type_t * find_type(const char * name)
{
    if (name)
        for (const auto & type : types)
            if (strcmp(name, type.name) == 0)
                return &type;
    return NULL;
}

static const abi_field_t * find_field(const char * type_name, const char * field_name)
{
    const auto * type = find_type(type_name);
    if (type && field_name)
        for (int i = 0; i < type->count; ++i)
            if (strcmp(field_name, type->fields[i].name) == 0)
                return &type->fields[i];
    return NULL;
}

uint32_t rocketnet_abi_version(void) { return ROCKETNET_ABI_VERSION; }

const char * rocketnet_abi_field_name(const char * type_name, int index)
{
    const auto * type = find_type(type_name);
    return type && index >= 0 && index < type->count ? type->fields[index].name : NULL;
}
size_t rocketnet_abi_type_size(const char * name)
{
    if ( !rocketnet_api_null_guard( name ) )
        return SIZE_MAX;
    const auto * type = find_type(name);
    return type ? type->size : SIZE_MAX;
}
int rocketnet_abi_field_count(const char * name)
{
    if ( !rocketnet_api_null_guard( name ) )
        return -1;
    const auto * type = find_type(name);
    return type ? type->count : -1;
}
size_t rocketnet_abi_field_offset(const char * type_name, const char * field_name)
{
    if ( !rocketnet_api_null_guard( type_name ) )
        return SIZE_MAX;
    if ( !rocketnet_api_null_guard( field_name ) )
        return SIZE_MAX;
    const auto * field = find_field(type_name, field_name);
    return field ? field->offset : SIZE_MAX;
}
size_t rocketnet_abi_field_size(const char * type_name, const char * field_name)
{
    if ( !rocketnet_api_null_guard( type_name ) )
        return SIZE_MAX;
    if ( !rocketnet_api_null_guard( field_name ) )
        return SIZE_MAX;
    const auto * field = find_field(type_name, field_name);
    return field ? field->size : SIZE_MAX;
}

#if !defined( ROCKETNET_ABI_QUERIES_ONLY )

// The ABI queries alone build into the deliberately incompatible test library
// (tools/abi_mismatch.cpp, ROCKETNET_ABI_QUERIES_ONLY); the sized entry points
// below build into the SDK.

#include <sodium.h>
#include <inttypes.h>

// Sized configs (docs/ABI-EVOLUTION.md). A config struct's latest revision is
// today's struct: every revision size in the header must be one the compiler's
// layout has, and the latest must equal sizeof, or a size formula is wrong for
// this target.

static_assert( ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2 == sizeof( struct rocketnet_client_transport_config_t ), "client transport config revision 2 size" );
static_assert( ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_2 == sizeof( struct rocketnet_server_transport_config_t ), "server transport config revision 2 size" );
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

#endif // #if !defined( ROCKETNET_ABI_QUERIES_ONLY )
