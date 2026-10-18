// SPDX-FileCopyrightText: 2026 Mas Bandwidth LLC
// SPDX-License-Identifier: AGPL-3.0-only
// Shared compatibility surface for the nightly SDK-including ESBMC harnesses.
// Include before the SDK .cpp. Define ROCKETNET_ESBMC only for an ESBMC run.
#pragma once

#if defined(ROCKETNET_ESBMC)

#include <stddef.h>
#include <stdint.h>
#include <cmath>
using std::isfinite;

#define __CPROVER__ 1

extern "C" void __ESBMC_assume(bool);
extern "C" void __ESBMC_assert(bool, const char *);
extern "C" bool __ESBMC_r_ok(void *, unsigned long);

#define __CPROVER_assume(c) __ESBMC_assume((bool)(c))
#define __CPROVER_assert(c, m) __ESBMC_assert((bool)(c), (m))
#define __CPROVER_r_ok(p, n) __ESBMC_r_ok((void *)(p), (unsigned long)(n))
// ESBMC exposes an extent predicate, not a separate writable predicate.
// Actual writes remain subject to its default pointer and bounds checks.
#define __CPROVER_w_ok(p, n) __ESBMC_r_ok((void *)(p), (unsigned long)(n))

// Unimplemented external returns are symbolic in ESBMC. Keep distinct names
// so plain-compiler fallbacks in each harness cannot accidentally satisfy them.
extern "C" int rocketnet_esbmc_nondet_int(void);
extern "C" unsigned rocketnet_esbmc_nondet_uint(void);
extern "C" unsigned char rocketnet_esbmc_nondet_uchar(void);
extern "C" uint64_t rocketnet_esbmc_nondet_uint64(void);
#define nondet_int rocketnet_esbmc_nondet_int
#define nondet_uint rocketnet_esbmc_nondet_uint
#define nondet_uchar rocketnet_esbmc_nondet_uchar
#define nondet_uint64 rocketnet_esbmc_nondet_uint64

static inline void rocketnet_esbmc_havoc_slice(void *p, size_t n)
{
    unsigned char *bytes = (unsigned char *)p;
    for (size_t i = 0; i < n; ++i)
        bytes[i] = rocketnet_esbmc_nondet_uchar();
}
#define __CPROVER_havoc_slice(p, n) rocketnet_esbmc_havoc_slice((p), (size_t)(n))

#endif
