/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The test seam over the vendored sodium pair's runtime dispatch
// (tests/sodium_dispatch_seam.c). Test code only.

#pragma once

#ifndef ROCKETNET_TESTS_SODIUM_DISPATCH_SEAM_H
#define ROCKETNET_TESTS_SODIUM_DISPATCH_SEAM_H

#ifdef __cplusplus
extern "C"
{
#endif

int sodium_dispatch_init( void );
int sodium_dispatch_has( const char * feature );
const char * sodium_dispatch_chacha20( void );
const char * sodium_dispatch_poly1305( void );
const char * sodium_dispatch_blake2b( void );
const char * sodium_dispatch_aead( void );
int sodium_dispatch_built( const char * primitive, const char * name );
int sodium_dispatch_select_chacha20( const char * name );
int sodium_dispatch_select_poly1305( const char * name );
int sodium_dispatch_select_blake2b( const char * name );
int sodium_dispatch_select_aead( const char * name );
void sodium_dispatch_force_no_avx2( int no_avx2 );
void sodium_dispatch_repick( void );
void sodium_dispatch_count_ietf_xor( int on );
long sodium_dispatch_ietf_xor_calls( void );
int sodium_dispatch_fused_length_ok( unsigned long long length );
int sodium_dispatch_misused( int open, unsigned long long length );

#ifdef __cplusplus
}
#endif

#endif // ROCKETNET_TESTS_SODIUM_DISPATCH_SEAM_H
