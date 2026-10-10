/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_H
#define ROCKETNET_TESTS_H

#include "rocketnet.h"

ROCKETNET_EXPORT_FUNC void rocketnet_run_tests();
ROCKETNET_EXPORT_FUNC void rocketnet_test_overflow_counter();

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );         \
    }                                                                                                           \
} while(0)

ROCKETNET_EXPORT_FUNC void rocketnet_check_handler( const char * condition, const char * function, const char * file, int line );

#endif // #ifndef ROCKETNET_TESTS_H
