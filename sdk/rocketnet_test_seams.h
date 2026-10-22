/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// ROCKETNET_TEST_SEAMS: 1 in a development build (ROCKETNET_DEV=1) and in
// the unit suite's hook library (which defines it), 0 in a production build
// (ROCKETNET_DEV=0). What only tests, tools and the development flow use --
// the network simulator, the published development server secret key and its
// dev config, and the unit seams -- compiles only where this is 1, so a
// production rocketnet and rocketnet_shared carry none of it.

#ifndef ROCKETNET_TEST_SEAMS_H
#define ROCKETNET_TEST_SEAMS_H

#include "rocketnet.h"

#if !defined( ROCKETNET_TEST_SEAMS )
#if ROCKETNET_DEV
#define ROCKETNET_TEST_SEAMS 1
#else // #if ROCKETNET_DEV
#define ROCKETNET_TEST_SEAMS 0
#endif // #if ROCKETNET_DEV
#endif // #if !defined( ROCKETNET_TEST_SEAMS )

#endif // #ifndef ROCKETNET_TEST_SEAMS_H
