/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// tests/time_scale.h -- the one scale on the suites' wall-clock bounds.
//
// A wall-clock bound in the suites (the live matrix's 10 s pass bound and 30 s
// window, the runner's 120 s kill) is a PERFORMANCE bound measured on a Release
// build. A Debug build runs the same work unoptimised with every assert on, and
// every sanitizer leg here (asan+ubsan, tsan) is a Debug build: measured
// 2026-10-19, the live matrix takes ~2 s in Release and 9-11 s in Debug on an
// idle amd-1, 25-28 s under asan and tsan. So a build without NDEBUG scales
// each such bound by ROCKETNET_FUNCTIONAL_DEBUG_TIME_SCALE; a Release build
// keeps the bound exactly as written (scale 1). No check is removed: a Debug
// run over the scaled bound still fails. The runner's kill (tests/suite_runner.h)
// serves the unit binary too, so a Debug or sanitizer unit run is killed at 600 s,
// not 120 s: a stated loosening for those builds only.

#ifndef ROCKETNET_TEST_TIME_SCALE_H
#define ROCKETNET_TEST_TIME_SCALE_H

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
