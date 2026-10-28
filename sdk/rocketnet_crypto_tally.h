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

extern thread_local struct rocketnet_crypto_tally_t rocketnet_crypto_tally;

static inline struct rocketnet_crypto_tally_t rocketnet_crypto_tally_get( void )
{
    return rocketnet_crypto_tally;
}

#define ROCKETNET_CRYPTO_TALLY( field ) ( rocketnet_crypto_tally.field++ )

#else // #if ROCKETNET_DEV

#define ROCKETNET_CRYPTO_TALLY( field ) ( (void) 0 )

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_CRYPTO_TALLY_H
