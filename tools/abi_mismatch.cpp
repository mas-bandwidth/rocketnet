// Deliberately incompatible library for the managed load gate. Never shipped.
#define rocketnet_abi_version real_abi_version
#define rocketnet_abi_type_size real_abi_type_size
#define rocketnet_abi_field_count real_abi_field_count
#define rocketnet_abi_field_offset real_abi_field_offset
#define rocketnet_abi_field_size real_abi_field_size
#include "../sdk/rocketnet_abi.cpp"
#undef rocketnet_abi_version
#undef rocketnet_abi_type_size
#undef rocketnet_abi_field_count
#undef rocketnet_abi_field_offset
#undef rocketnet_abi_field_size
#include <stdlib.h>

static bool mode(const char * name)
{
    const char * value = getenv("ROCKETNET_ABI_TEST_MODE");
    return value && strcmp(value, name) == 0;
}
ROCKETNET_EXPORT_FUNC uint32_t rocketnet_abi_version(void)
{ return real_abi_version() + (mode("revision") ? 1 : 0); }
ROCKETNET_EXPORT_FUNC size_t rocketnet_abi_type_size(const char * type)
{ return real_abi_type_size(type) + (mode("size") ? 8 : 0); }
ROCKETNET_EXPORT_FUNC int rocketnet_abi_field_count(const char * type)
{ return real_abi_field_count(type) + (mode("field count") ? 1 : 0); }
ROCKETNET_EXPORT_FUNC size_t rocketnet_abi_field_offset(const char * type, const char * field)
{ return real_abi_field_offset(type, field) + (mode("offset") ? 1 : 0); }
ROCKETNET_EXPORT_FUNC size_t rocketnet_abi_field_size(const char * type, const char * field)
{ return real_abi_field_size(type, field) + (mode("width") ? 1 : 0); }

// If a guard disappears, the test dies here instead of accepting a fake success.
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_default_config(rocketnet_client_transport_config_t *) { abort(); }
