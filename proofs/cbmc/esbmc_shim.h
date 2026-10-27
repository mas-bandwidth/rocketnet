/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Shared compatibility header mapping the CBMC harness vocabulary
    (__CPROVER_assert, __CPROVER_assume, __CPROVER_same_object,
    __CPROVER_r_ok, __CPROVER_w_ok, __CPROVER_havoc_slice, and nondet_*
    generators) onto ESBMC 8.5+.

    Why rocketnet_esbmc_r_ok uses __ESBMC_alloc_size and __ESBMC_get_object_size:
    1. ESBMC 8.5 does not define a builtin __ESBMC_r_ok(void *, unsigned long);
       declaring it as an external symbol leaves it unimplemented, so ESBMC
       models its return value as an unconstrained nondeterministic boolean
       that can be false on valid in-bounds slices.
    2. ESBMC 8.5's C library __CPROVER_r_ok calls __ESBMC_get_object_size(p),
       which resolves static/stack array sizes but lowers to sizeof(void *) (8)
       on symbolic-size heap allocations from malloc(n). Dynamic object bounds
       are tracked in __ESBMC_alloc_size[__ESBMC_POINTER_OBJECT(p)].
    Checking uoff <= sz && n <= sz - uoff against __ESBMC_alloc_size[obj] for
    dynamic objects and __ESBMC_get_object_size(p) for non-dynamic objects is
    sound for both symbolic-size heap buffers and stack buffers without
    unsigned wrap/overflow.
*/

#pragma once

#ifndef ROCKETNET_PROOFS_CBMC_ESBMC_SHIM_H
#define ROCKETNET_PROOFS_CBMC_ESBMC_SHIM_H


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#include <cmath>
using std::isfinite;
extern "C"
{
#endif

void __ESBMC_assume( bool );
void __ESBMC_assert( bool, const char * );
__attribute__( ( annotate( "__ESBMC_inf_size" ) ) ) extern bool __ESBMC_alloc[1];
__attribute__( ( annotate( "__ESBMC_inf_size" ) ) ) extern bool __ESBMC_is_dynamic[1];
__attribute__( ( annotate( "__ESBMC_inf_size" ) ) ) extern size_t __ESBMC_alloc_size[1];
size_t __ESBMC_POINTER_OBJECT( const void * );
long __ESBMC_POINTER_OFFSET( const void * );
size_t __ESBMC_get_object_size( const void * );

int rocketnet_esbmc_nondet_int( void );
unsigned int rocketnet_esbmc_nondet_uint( void );
unsigned char rocketnet_esbmc_nondet_uchar( void );
uint16_t rocketnet_esbmc_nondet_ushort( void );
uint64_t rocketnet_esbmc_nondet_uint64( void );

#ifdef __cplusplus
}
#endif

#define __CPROVER__ 1

#define __CPROVER_assume( condition ) __ESBMC_assume( (bool) ( condition ) )
#define __CPROVER_assert( condition, message ) __ESBMC_assert( (bool) ( condition ), ( message ) )
#define __CPROVER_same_object( a, b ) __ESBMC_same_object( ( a ), ( b ) )

static inline bool rocketnet_esbmc_r_ok( const void * p, size_t n )
{
    if ( n == 0 )
        return true;
    if ( p == NULL )
        return false;
    size_t obj = __ESBMC_POINTER_OBJECT( p );
    if ( obj == 0 )
        return false;
    if ( __ESBMC_is_dynamic[obj] && !__ESBMC_alloc[obj] )
        return false;
    long off = __ESBMC_POINTER_OFFSET( p );
    if ( off < 0 )
        return false;
    size_t sz = __ESBMC_is_dynamic[obj]
                    ? __ESBMC_alloc_size[obj]
                    : __ESBMC_get_object_size( p );
    size_t uoff = (size_t) off;
    return uoff <= sz && n <= sz - uoff;
}

#define __CPROVER_r_ok( p, n ) rocketnet_esbmc_r_ok( (const void *) ( p ), (size_t) ( n ) )
#define __CPROVER_w_ok( p, n ) rocketnet_esbmc_r_ok( (const void *) ( p ), (size_t) ( n ) )

#define nondet_int rocketnet_esbmc_nondet_int
#define nondet_uint rocketnet_esbmc_nondet_uint
#define nondet_uchar rocketnet_esbmc_nondet_uchar
#define nondet_uint8 rocketnet_esbmc_nondet_uchar
#define nondet_ushort rocketnet_esbmc_nondet_ushort
#define nondet_uint64 rocketnet_esbmc_nondet_uint64

static inline void rocketnet_esbmc_havoc_slice( void * p, size_t n )
{
    unsigned char * bytes = (unsigned char *) p;
    for ( size_t i = 0; i < n; ++i )
        bytes[i] = rocketnet_esbmc_nondet_uchar();
}
#define __CPROVER_havoc_slice( p, n ) rocketnet_esbmc_havoc_slice( ( p ), (size_t) ( n ) )


#endif // ROCKETNET_PROOFS_CBMC_ESBMC_SHIM_H
