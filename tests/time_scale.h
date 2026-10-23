/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// tests/time_scale.h -- the one scale on the suites' wall-clock bounds.
//
// Functional cases target 60 seconds and have a fixed 120-second completion
// limit, including Debug, sanitizer and emulated builds. This is a correctness
// test budget, not a networking performance requirement. Other individual
// waits and the unit runner retain their existing build-dependent scaling.

#ifndef ROCKETNET_TEST_TIME_SCALE_H
#define ROCKETNET_TEST_TIME_SCALE_H

#define ROCKETNET_FUNCTIONAL_CASE_TARGET_SECONDS 60.0
#define ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS 120.0

#define ROCKETNET_FUNCTIONAL_DEBUG_TIME_SCALE 5.0

// a sanitizer build is scaled even with NDEBUG (gcc's __SANITIZE_*__, clang's __has_feature)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ROCKETNET_FUNCTIONAL_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define ROCKETNET_FUNCTIONAL_SANITIZED 1
#endif
#endif

#if defined(NDEBUG) && !defined(ROCKETNET_FUNCTIONAL_SANITIZED)
#define ROCKETNET_FUNCTIONAL_TIME_SCALE 1.0
#else
#define ROCKETNET_FUNCTIONAL_TIME_SCALE ROCKETNET_FUNCTIONAL_DEBUG_TIME_SCALE
#endif

// Full-system emulation runs a Release build, but one case receives only a
// fraction of a native CPU. Scale case completion bounds without lengthening
// workload durations such as the live matrix's measurement window. The
// full-system workflow opts in explicitly; native Release remains 1.

#if defined(ROCKETNET_TEST_EMULATED)
#define ROCKETNET_TEST_CASE_TIME_SCALE ROCKETNET_FUNCTIONAL_DEBUG_TIME_SCALE
#else
#define ROCKETNET_TEST_CASE_TIME_SCALE ROCKETNET_FUNCTIONAL_TIME_SCALE
#endif

#endif // ROCKETNET_TEST_TIME_SCALE_H
