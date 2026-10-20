// Deliberately incompatible library for native ABI regression checks. Never shipped.
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

// The fixture has no SDK lifecycle or diagnostic counter. Invalid ABI query
// arguments must fail loudly instead of depending on the production library.
void rocketnet_api_misuse_record() { abort(); }
static void fixture_assert( const char *, const char *, const char *, int ) { abort(); }
void (*rn_assert_function)( const char *, const char *, const char *, int ) = fixture_assert;

static bool mode(const char * name)
{
    const char * value = getenv("ROCKETNET_ABI_TEST_MODE");
    return value && strcmp(value, name) == 0;
}
extern "C" ROCKETNET_EXPORT uint32_t rocketnet_abi_version(void)
{ return real_abi_version() + (mode("revision") ? 1 : 0); }
extern "C" ROCKETNET_EXPORT size_t rocketnet_abi_type_size(const char * type)
{ return real_abi_type_size(type) + (mode("size") ? 8 : 0); }
extern "C" ROCKETNET_EXPORT int rocketnet_abi_field_count(const char * type)
{ return real_abi_field_count(type) + (mode("field count") ? 1 : 0); }
extern "C" ROCKETNET_EXPORT size_t rocketnet_abi_field_offset(const char * type, const char * field)
{ return real_abi_field_offset(type, field) + (mode("offset") ? 1 : 0); }
extern "C" ROCKETNET_EXPORT size_t rocketnet_abi_field_size(const char * type, const char * field)
{ return real_abi_field_size(type, field) + (mode("width") ? 1 : 0); }

// If a guard disappears, the test dies here instead of accepting a fake success.
extern "C" ROCKETNET_EXPORT void rocketnet_client_transport_default_config(rocketnet_client_transport_config_t *) { abort(); }
