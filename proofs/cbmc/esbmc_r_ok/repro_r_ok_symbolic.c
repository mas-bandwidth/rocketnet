/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Standalone reproducer for __CPROVER_r_ok / __CPROVER_w_ok on a symbolic-size
    heap allocation under ESBMC 8.5:
      - Allocates uint8_t *buf = malloc(n) for symbolic n in 1..64.
      - Asserts __CPROVER_r_ok(buf, n) and __CPROVER_r_ok(buf + k, n - k)
        for symbolic k <= n, as well as __CPROVER_w_ok on both spans.
      - Also checks a stack buffer tail slice of symbolic length in 0..64.

    Build modes:
      - Default (uses proofs/cbmc/esbmc_shim.h): VERIFICATION SUCCESSFUL
      - -DTEST_PRE_FIX_ESBMC_R_OK (maps __CPROVER_r_ok to undeclared builtin
        bool __ESBMC_r_ok(void *, unsigned long)): VERIFICATION FAILED
      - -DTEST_LIBC_CPROVER_R_OK (uses ESBMC 8.5 builtin_libs.c __CPROVER_r_ok
        which calls __ESBMC_get_object_size(p) == 8 on malloc(n)): VERIFICATION FAILED
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined( TEST_PRE_FIX_ESBMC_R_OK )
#include <stdbool.h>
#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus
void __ESBMC_assume( bool );
void __ESBMC_assert( bool, const char * );
bool __ESBMC_r_ok( void *, unsigned long );
uint16_t rocketnet_esbmc_nondet_ushort( void );
#ifdef __cplusplus
}
#endif // #ifdef __cplusplus
#define __CPROVER_assume( c ) __ESBMC_assume( (bool) ( c ) )
#define __CPROVER_assert( c, m ) __ESBMC_assert( (bool) ( c ), ( m ) )
#define __CPROVER_r_ok( p, n ) __ESBMC_r_ok( (void *) ( p ), (unsigned long) ( n ) )
#define __CPROVER_w_ok( p, n ) __ESBMC_r_ok( (void *) ( p ), (unsigned long) ( n ) )
#define nondet_ushort rocketnet_esbmc_nondet_ushort
#elif defined( TEST_LIBC_CPROVER_R_OK )
#include <stdbool.h>
#define __CPROVER_assume( c ) __ESBMC_assume( (bool) ( c ) )
#define __CPROVER_assert( c, m ) __ESBMC_assert( (bool) ( c ), ( m ) )
uint16_t nondet_ushort( void );
#else
#include "esbmc_shim.h"
#endif // #if defined( TEST_PRE_FIX_ESBMC_R_OK )

int main( void )
{
    uint16_t n;
    n = nondet_ushort();
    __CPROVER_assume( n >= 1 && n <= 64 );

    uint8_t * buf = (uint8_t *) malloc( (size_t) n );
    __CPROVER_assume( buf != NULL );

    __CPROVER_assert( __CPROVER_r_ok( buf, (size_t) n ),
                      "full symbolic malloc extent is readable" );
    __CPROVER_assert( __CPROVER_w_ok( buf, (size_t) n ),
                      "full symbolic malloc extent is writable" );

    uint16_t k = nondet_ushort();
    __CPROVER_assume( k <= n );

    __CPROVER_assert( __CPROVER_r_ok( buf + k, (size_t) ( n - k ) ),
                      "symbolic suffix slice [k, n) of malloc(n) is readable" );
    __CPROVER_assert( __CPROVER_w_ok( buf + k, (size_t) ( n - k ) ),
                      "symbolic suffix slice [k, n) of malloc(n) is writable" );

    uint8_t stack_buf[64];
    uint16_t sb = nondet_ushort();
    __CPROVER_assume( sb <= 64 );

    const uint8_t * tail = stack_buf + ( 64 - sb );
    __CPROVER_assert( __CPROVER_r_ok( tail, (size_t) sb ),
                      "symbolic tail slice of stack buffer is readable" );

    free( buf );

    return 0;
}
