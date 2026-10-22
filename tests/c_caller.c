/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Calls into the public API from C, for the unit suite. A C caller may hand
// any int where the API takes an enum; in C++ a value outside the enum's range
// of values cannot be formed without undefined behaviour, so the cases that
// pass one go through here.

#include "rocketnet.h"

void rocketnet_test_c_caller_log_level( int level );
const char * rocketnet_test_c_caller_log_level_name( int level );

void rocketnet_test_c_caller_log_level( int level )
{
    rocketnet_log_level( (enum rocketnet_log_level_t) level );
}

const char * rocketnet_test_c_caller_log_level_name( int level )
{
    return rocketnet_log_level_name( (enum rocketnet_log_level_t) level );
}
