/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A translation unit that includes only rocketnet.h, so the public header is
// compiled the way every game that includes it compiles it, at the strictest
// warning level each compiler has: clang -Weverything (less the stylistic
// -Wpadded), MSVC /Wall, gcc's strict set. CMakeLists.txt builds it as C here
// and as C++ through header_warnings.cpp, with warnings as errors, in the
// default build; it is never linked.

#include "rocketnet.h"
