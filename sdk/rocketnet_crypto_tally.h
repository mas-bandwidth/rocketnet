/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CRYPTO_TALLY_H
#define ROCKETNET_SDK_ROCKETNET_CRYPTO_TALLY_H

#include <stdint.h>

#include "rocketnet.h"

// Counts expensive cryptography on receive paths so tests can prove that
// cheap refusals spend none. Keyed hashes (cookie MAC, token identifier,
// key derivation) are the cheap tier and are not counted here. A test probe:
// ROCKETNET_CRYPTO_TALLY( field ) increments under ROCKETNET_DEV and is
// nothing in a production build.
#if ROCKETNET_DEV

struct rocketnet_crypto_tally_t
{
    uint64_t keypair;
    uint64_t scalarmult;
    uint64_t aead_seal;
    uint64_t aead_open;
    uint64_t signature_verify;
    uint64_t seal_open;
};

// The tally is thread-local and static to sdk/rocketnet_protocol.cpp, and
// every other file reaches it through these functions, defined there. An
// extern thread_local read from another file can leave a weak initializer
// unresolved at address 0 in optimized sanitizer builds (GCC 15 -O2 with
// -fsanitize=undefined took it for a null object), as
// rocketnet_server_cookie_mac_count says for its own tally.
// Run: g++ 15.2.0 RelWithDebInfo with -fsanitize=address,undefined on Linux
// (hetzner1, 2026-11-13) reproduced the null tally in two tests, and both pass
// with these functions. Other compilers and sanitizer configurations were
// read, not run.
struct rocketnet_crypto_tally_t * rocketnet_crypto_tally_this_thread( void );

static inline struct rocketnet_crypto_tally_t rocketnet_crypto_tally_get( void )
{
    return *rocketnet_crypto_tally_this_thread();
}

static inline void rocketnet_crypto_tally_reset( void )
{
    const struct rocketnet_crypto_tally_t zero = { 0, 0, 0, 0, 0, 0 };
    *rocketnet_crypto_tally_this_thread() = zero;
}

#define ROCKETNET_CRYPTO_TALLY( field ) ( rocketnet_crypto_tally_this_thread()->field++ )

#else // #if ROCKETNET_DEV

#define ROCKETNET_CRYPTO_TALLY( field ) ( (void) 0 )

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_CRYPTO_TALLY_H
