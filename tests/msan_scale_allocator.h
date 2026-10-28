/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The allocator a full-capacity case (1000 connected slots, a server and its
// clients in one process) runs under in a MemorySanitizer build.
//
// The SDK plans a 1000-slot world up front: about 36 GiB in blocks of up to
// 18.7 GiB (the message server object, sdk/rocketnet_message_object.cpp), of
// which the case touches about 1 GiB (tests/test_unit_server_stats_no_worker.cpp
// measures it). That is the design (the server's cost is paid at create,
// untouched pages cost nothing), and it holds under MSan only for the
// application pages: MSan's malloc poisons every byte it hands out, writing
// one shadow byte and, with origin tracking, one origin byte per byte, so
// each planned byte becomes two resident bytes before the case runs. A
// 1000-slot world under MSan is then about 72 GiB resident, which no fleet
// runner has. Up to clang 18 MSan refused any single malloc over 8 GiB as a
// fatal error, so these cases died at the server create with MSan's exit code
// and were never checked; clang 21 grants the block and the poison writes
// fill the runner (measured 2026-10-28 on amd-2: one case passing 10 GiB in
// 4 s, inside the server create's malloc).
//
// So in an MSan build such a case allocates a block of a mebibyte or more
// from a fresh anonymous mapping, which MSan treats as initialized memory
// (its mmap interceptor clears the shadow by remapping, touching no page),
// exactly as the kernel hands those pages out: zero-filled. Smaller blocks
// still come from malloc, poisoned. The cost: an uninitialized read inside a
// large plan block is not reported by these cases; the same code paths run
// poisoned in every smaller-world case of the suite. Outside MSan the scope
// does nothing and the case runs on the default allocator.
//
// The scope terminates the worker's library and initializes it with this
// allocator, as tests/test_unit_driver_create_out_of_memory.cpp installs its
// allocator, and at its end terminates and puts the default allocator back.

#pragma once

#ifndef ROCKETNET_TESTS_MSAN_SCALE_ALLOCATOR_H
#define ROCKETNET_TESTS_MSAN_SCALE_ALLOCATOR_H

#include "rocketnet_internal.h"
#include "test_unit_registry.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined( __has_feature )
#if __has_feature( memory_sanitizer )
#define ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR 1
#endif // #if __has_feature( memory_sanitizer )
#endif // #if defined( __has_feature )

#if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )

#include <sys/mman.h>

enum
{
    MSAN_SCALE_HEADER_BYTES = 64,               // keeps the block 64-byte aligned
    MSAN_SCALE_MAPPED_BYTES = 1024 * 1024,      // blocks this size and up are mapped
};

struct msan_scale_header_t
{
    size_t mapped_bytes;                        // 0: the block came from malloc
};

inline void * msan_scale_allocate( void *, size_t bytes )
{
    if ( bytes > SIZE_MAX - MSAN_SCALE_HEADER_BYTES )
        return NULL;

    const size_t total = bytes + MSAN_SCALE_HEADER_BYTES;
    uint8_t * base = NULL;
    size_t mapped_bytes = 0;
    if ( bytes >= MSAN_SCALE_MAPPED_BYTES )
    {
        void * mapping = mmap( NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0 );
        if ( mapping == MAP_FAILED )
            return NULL;
        base = (uint8_t *) mapping;
        mapped_bytes = total;
    }
    else
    {
        base = (uint8_t *) malloc( total );
        if ( !base )
            return NULL;
    }

    msan_scale_header_t * header = (msan_scale_header_t *) base;
    header->mapped_bytes = mapped_bytes;
    return base + MSAN_SCALE_HEADER_BYTES;
}

inline void msan_scale_release( void *, void * pointer )
{
    if ( !pointer )
        return;

    uint8_t * base = (uint8_t *) pointer - MSAN_SCALE_HEADER_BYTES;
    const msan_scale_header_t * header = (const msan_scale_header_t *) base;
    if ( header->mapped_bytes )
        munmap( base, header->mapped_bytes );
    else
        free( base );
}

#endif // #if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )

struct msan_scale_allocator_scope_t
{
#if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )
    msan_scale_allocator_scope_t()
    {
        rocketnet_unit_check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
        int status = -1;
        rocketnet_unit_check( rocketnet_init_ex( NULL, msan_scale_allocate, msan_scale_release, &status ) );
        rocketnet_unit_check( status == ROCKETNET_STATUS_OK );
    }

    ~msan_scale_allocator_scope_t() noexcept( false ) // check() throws under the expected-fail harness
    {
        rocketnet_unit_check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
        rocketnet_unit_check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    }
#else // #if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )
    msan_scale_allocator_scope_t() {} // user-provided, so a case's scope is never an unused variable
#endif // #if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )
};

#endif // ROCKETNET_TESTS_MSAN_SCALE_ALLOCATOR_H
