/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// header_warnings.c for the form of the header a shipped game compiles: a
// production library is ROCKETNET_DEV=0 (the release version macros, no
// development API), and header_warnings.c sees the default ROCKETNET_DEV=1.
// CMakeLists.txt builds both in the same object library, at the same
// strictest warning level, with warnings as errors; it is never linked.

#define ROCKETNET_DEV 0

#include "rocketnet.h"
