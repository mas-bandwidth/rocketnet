#include <rocketnet.h>
#include <rocketnet_address.h>
#include <rocketnet_connect_token.h>
#include <rocketnet_platform.h>
#include <rocketnet_counters.h>
#include <stdio.h>
#include <string.h>

#if defined(EXPECT_SHARED) && !defined(ROCKETNET_SHARED)
#error Shared consumer did not inherit the DLL import contract
#endif
#if !defined(EXPECT_SHARED) && defined(ROCKETNET_SHARED)
#error Static consumer inherited the DLL import contract
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "consumer failed: %s\n", #c); return 1; } } while (0)

int main(void)
{
    CHECK(rocketnet_abi_version() == ROCKETNET_ABI_VERSION);
    CHECK(rocketnet_abi_type_size("rocketnet_client_transport_config_t") == sizeof(struct rocketnet_client_transport_config_t));
    CHECK(rocketnet_abi_field_offset("rocketnet_client_transport_config_t", "state_change_callback") == offsetof(struct rocketnet_client_transport_config_t, state_change_callback));
    CHECK(rocketnet_abi_field_size("rocketnet_server_transport_config_t", "driver") == ROCKETNET_DRIVER_STRING_LENGTH);
    CHECK(rocketnet_abi_field_count("rocketnet_packet_context_t") == 6);
    CHECK(rocketnet_abi_type_size(NULL) == SIZE_MAX);
    CHECK(rocketnet_abi_field_count("missing") == -1);
    CHECK(rocketnet_abi_field_offset("rocketnet_packet_context_t", "missing") == SIZE_MAX);
    CHECK(rocketnet_abi_field_size("missing", NULL) == SIZE_MAX);
    CHECK(rocketnet_assert_function_pointer != NULL);
    CHECK(rocketnet_num_counters() >= ROCKETNET_NUM_COUNTERS);
    CHECK(rocketnet_init(NULL));
    rocketnet_forbid_xdp_commit();
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_ERROR);
    struct rocketnet_address_t address;
    CHECK(rocketnet_address_parse(&address, "127.0.0.1:40000"));
    CHECK(address.port == 40000);
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    config.max_clients = 1;
    strcpy(config.driver, "portable");
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create(NULL, "127.0.0.1:0", &config, 0.0);
    CHECK(server != NULL);
    CHECK(strcmp(rocketnet_server_transport_driver(server), "portable") == 0);
    rocketnet_server_transport_destroy(server);
    rocketnet_term();
    puts("installed SDK consumer passed");
    return 0;
}
