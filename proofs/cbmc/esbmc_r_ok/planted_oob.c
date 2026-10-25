/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Planted 1-byte out-of-bounds harness for proofs/cbmc/esbmc_shim.h.
    Allocates uint8_t *buf = malloc(n) for symbolic n in 1..64 and asserts
    __CPROVER_r_ok(buf + k, (n - k) + 1) for symbolic k <= n (1 byte past the
    end of the allocated object). Must fail verification (VERIFICATION FAILED).
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esbmc_shim.h"

int main( void )
{
    uint16_t n = nondet_ushort();
    __CPROVER_assume( n >= 1 && n <= 64 );

    uint8_t * buf = (uint8_t *) malloc( (size_t) n );
    __CPROVER_assume( buf != NULL );

    uint16_t k = nondet_ushort();
    __CPROVER_assume( k <= n );

    /* Planted 1-byte OOB: (n - k) + 1 extends 1 byte past buf + n. */
    __CPROVER_assert( __CPROVER_r_ok( buf + k, (size_t) ( n - k ) + 1 ),
                      "planted 1-byte OOB past symbolic malloc(n) must fail" );

    free( buf );
    return 0;
}
