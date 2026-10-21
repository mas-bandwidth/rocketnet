/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */

#include "test_unit_registry.h"
#include "sdk/rocketnet_internal.h"
#include "sdk/rocketnet_connect_token.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#endif

#if defined(ROCKETNET_USE_SYSTEM_SODIUM)
#include <sodium.h>
#endif

#define check rocketnet_unit_check

#if defined(__APPLE__) && !defined(ROCKETNET_USE_SYSTEM_SODIUM)
static long rng_lifecycle_fd_count()
{
    const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, NULL, 0);
    if (bytes <= 0) return -1;
    void * entries = malloc((size_t)bytes);
    if (!entries) return -1;
    const int used = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, entries, bytes);
    free(entries);
    return used < 0 ? -1 : (long)(used / (int)PROC_PIDLISTFD_SIZE);
}

static void test_bundled_rng_closes_and_reopens_on_mac()
{
    // The unit worker begins initialized. Establish the uninitialized baseline
    // after the first completed term, then exercise several distinct cycles.
    check(rocketnet_term() == ROCKETNET_STATUS_OK);
    const long baseline = rng_lifecycle_fd_count();
    check(baseline >= 0);

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        check(rocketnet_init(NULL, NULL, NULL) == ROCKETNET_STATUS_OK);
        uint8_t random_data[32];
        rocketnet_random_bytes(random_data, sizeof(random_data));
        uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
        uint8_t secret_key[sizeof(rocketnet_dev_server_secret_key)];
        check(rocketnet_generate_server_keypair(public_key, secret_key) == ROCKETNET_STATUS_OK);

        if (cycle == 1)
        {
            rocketnet_client_config_t config;
            check(rocketnet_client_default_config(&config) == ROCKETNET_STATUS_OK);
            config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
            config.max_fragmented_messages = 1;
            rocketnet_status_t status = ROCKETNET_STATUS_OK;
            rocketnet_client_t * client = rocketnet_client_create(NULL, &config, &status);
            check(client != NULL && status == ROCKETNET_STATUS_OK);
            const long with_client = rng_lifecycle_fd_count();
            check(with_client >= 0);
            check(rocketnet_term() == ROCKETNET_STATUS_LIVE_ALLOCATIONS);
            check(rng_lifecycle_fd_count() == with_client);
            rocketnet_client_destroy(client);
        }

        check(rocketnet_term() == ROCKETNET_STATUS_OK);
        check(rng_lifecycle_fd_count() == baseline);
    }
    // Restore the worker's initialized state for its ordinary final term.
    check(rocketnet_init(NULL, NULL, NULL) == ROCKETNET_STATUS_OK);
}
REGISTER_UNIT_TEST(test_bundled_rng_closes_and_reopens_on_mac);
#endif

#if defined(ROCKETNET_USE_SYSTEM_SODIUM)
static int rng_lifecycle_system_close_calls = 0;
static const char * rng_lifecycle_name() { return "rocketnet-test-rng"; }
static uint32_t rng_lifecycle_random() { return 42; }
static void rng_lifecycle_buf(void * p, size_t bytes)
{
    unsigned char * out = (unsigned char *)p;
    for (size_t i = 0; i < bytes; ++i) out[i] = (unsigned char)(i + 1);
}
static int rng_lifecycle_close()
{
    ++rng_lifecycle_system_close_calls;
    return 0;
}

static void test_system_sodium_rng_remains_application_owned()
{
    static randombytes_implementation rng = {};
    rng.implementation_name = rng_lifecycle_name;
    rng.random = rng_lifecycle_random;
    rng.buf = rng_lifecycle_buf;
    rng.close = rng_lifecycle_close;
    check(randombytes_set_implementation(&rng) == 0);
    check(rocketnet_term() == ROCKETNET_STATUS_OK);
    check(rng_lifecycle_system_close_calls == 0);
    unsigned char output[4] = {};
    randombytes_buf(output, sizeof(output));
    check(output[0] == 1 && output[1] == 2 && output[2] == 3 && output[3] == 4);
    // Keep the implementation alive through the worker's final term.
    check(rocketnet_init(NULL, NULL, NULL) == ROCKETNET_STATUS_OK);
}
REGISTER_UNIT_TEST(test_system_sodium_rng_remains_application_owned);
#endif
