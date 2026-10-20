#include <rocketnet.h>
#include <stdio.h>
#include <string.h>

#if defined(EXPECT_SHARED) && !defined(ROCKETNET_SHARED)
#error Shared consumer did not inherit the DLL import contract
#endif
#if !defined(EXPECT_SHARED) && defined(ROCKETNET_SHARED)
#error Static consumer inherited the DLL import contract
#endif
/* This consumer builds in the package's configuration, so its NDEBUG is the
   library's: the library asserts on a NULL query exactly when NDEBUG is off. */
#ifdef NDEBUG
#define EXPECTED_NULL_ASSERTIONS 0
#else
#define EXPECTED_NULL_ASSERTIONS 2
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "consumer failed: %s\n", #c); return 1; } } while (0)

static int expected_null_assertions;
static void count_null_assertion(const char * condition, const char * function, const char * file, int line)
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    ++expected_null_assertions;
}

int main(void)
{
    CHECK(rocketnet_abi_version() == ROCKETNET_ABI_VERSION);
    CHECK(rocketnet_version() == ROCKETNET_VERSION);
    CHECK(rocketnet_abi_type_size("rocketnet_client_config_t") == sizeof(struct rocketnet_client_config_t));
    CHECK(rocketnet_abi_field_offset("rocketnet_client_config_t", "state_change_callback") == offsetof(struct rocketnet_client_config_t, state_change_callback));
    CHECK(rocketnet_abi_field_size("rocketnet_server_config_t", "transport") == sizeof(const char *));
    CHECK(rocketnet_abi_field_count("rocketnet_receipt_t") == 2);
    const uint64_t misuse_before = rocketnet_api_misuse_count();
    /* Deliberate NULL queries retain their counted Debug assertion contract. */
    rocketnet_set_assert_callback(count_null_assertion);
    CHECK(rocketnet_abi_type_size(NULL) == SIZE_MAX);
    CHECK(rocketnet_abi_field_count("missing") == -1);
    CHECK(rocketnet_abi_field_offset("rocketnet_receipt_t", "missing") == SIZE_MAX);
    CHECK(rocketnet_abi_field_size("missing", NULL) == SIZE_MAX);
    rocketnet_set_assert_callback(NULL);
    CHECK(rocketnet_api_misuse_count() == misuse_before + 2);
    CHECK(expected_null_assertions == EXPECTED_NULL_ASSERTIONS);
    CHECK(rocketnet_client_state_name(ROCKETNET_CLIENT_STATE_CONNECTED) != NULL);
    CHECK(rocketnet_init(NULL) == ROCKETNET_STATUS_OK);
    rocketnet_set_log_level(ROCKETNET_LOG_LEVEL_ERROR);
    struct rocketnet_server_config_t config;
    /* Public TEST VECTORS, never production credentials. This consumer only
       creates and destroys a loopback server; it sends no authenticated traffic.
       X25519 Alice: https://www.rfc-editor.org/rfc/rfc7748#section-6.1
       Ed25519 test 1: https://www.rfc-editor.org/rfc/rfc8032#section-7.1 */
    static const uint8_t test_server_secret_key[32] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a
    };
    static const uint8_t test_server_public_key[32] = {
        0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
        0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
        0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
        0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a
    };
    static const uint8_t test_backend_public_key[32] = {
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
        0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
        0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a
    };
    rocketnet_server_default_config(&config);
    memcpy(config.server_secret_key, test_server_secret_key, sizeof(config.server_secret_key));
    memcpy(config.server_public_key, test_server_public_key, sizeof(config.server_public_key));
    memcpy(config.backend_public_key, test_backend_public_key, sizeof(config.backend_public_key));
    config.protocol_id = 1;
    config.max_clients = 1;
    config.transport = "portable";
    CHECK(rocketnet_server_validate_config(&config) == ROCKETNET_STATUS_OK);
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &config, NULL, 0, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK(server != NULL);
    CHECK(strcmp(rocketnet_server_transport_name(server), "portable") == 0);
    CHECK(rocketnet_server_port(server) != 0);
    rocketnet_server_destroy(server);
    rocketnet_term();
    puts("installed SDK consumer passed");
    return 0;
}
