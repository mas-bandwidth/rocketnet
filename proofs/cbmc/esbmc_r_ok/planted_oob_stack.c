/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Planted 1-byte out-of-bounds harness for the non-dynamic branch of
    proofs/cbmc/esbmc_shim.h (the extent from __ESBMC_get_object_size, which
    every PROOF_R_OK / PROOF_W_OK on a stack array in the connect-token
    harness goes through). For a 64-byte stack array and symbolic k <= 64 it
    asserts __CPROVER_r_ok(buf + k, (64 - k) + 1), one byte past the end of
    the array. Must fail verification (VERIFICATION FAILED); planted_oob.c is
    the same plant on a symbolic malloc.
*/

#include <stddef.h>
#include <stdint.h>

#include "esbmc_shim.h"

int main( void )
{
    uint8_t buf[64];

    uint16_t k = nondet_ushort();
    __CPROVER_assume( k <= sizeof( buf ) );

    // Planted 1-byte OOB: (64 - k) + 1 extends 1 byte past buf + 64.
    __CPROVER_assert( __CPROVER_r_ok( buf + k, ( sizeof( buf ) - k ) + 1 ),
                      "planted 1-byte OOB past a stack array must fail" );

    return 0;
}
