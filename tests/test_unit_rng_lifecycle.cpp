/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */

#include "test_unit_registry.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_connect_token.h"

#include <stdint.h>
#include <stdlib.h>

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

// The bundled-RNG descriptor lifecycle case reads the live process's open
// descriptors through proc_pidinfo, which is OS state, so it is a functional
// case: test_bundled_rng_closes_and_reopens_on_mac in
// functional/platform_primitives.cpp. The system-sodium case below replaces
// the RNG with a fake and reads nothing from the OS, so it stays a unit case.

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#define check rocketnet_unit_check

static int rng_lifecycle_system_close_calls = 0;
static const char * rng_lifecycle_name() { return "rocketnet-test-rng"; }
static uint32_t rng_lifecycle_random() { return 42; }
static void rng_lifecycle_buf( void * p, size_t bytes )
{
    unsigned char * out = (unsigned char *) p;
    for ( size_t i = 0; i < bytes; ++i )
    {
        out[i] = (unsigned char) ( i + 1 );
    }
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
    check( randombytes_set_implementation( &rng ) == 0 );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rng_lifecycle_system_close_calls == 0 );

    unsigned char output[4] = {};
    randombytes_buf( output, sizeof( output ) );
    check( output[0] == 1 && output[1] == 2 && output[2] == 3 && output[3] == 4 );

    // keep the implementation alive through the worker's final term

    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_system_sodium_rng_remains_application_owned );
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )
