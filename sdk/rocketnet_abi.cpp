/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet.h"
#include <string.h>

struct abi_field_t { const char * name; size_t offset; size_t size; };
struct abi_type_t { const char * name; size_t size; const abi_field_t * fields; int count; };

// Offsets and sizes come from the compiler, including target ABI padding.
#define FIELD(name) { #name, offsetof(ABI_TYPE, name), sizeof(((ABI_TYPE*)0)->name) },
#define INT_FIELD(name, def, min, max, flags) FIELD(name)
#define UINT64_FIELD(name, def, flags) FIELD(name)
#define BYTES_FIELD(name, size, def, flags) FIELD(name)
#define STRING_FIELD(name, capacity, def, flags) FIELD(name)

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
