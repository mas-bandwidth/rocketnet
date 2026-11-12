/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The allocator a full-capacity case (1000 connected slots, a server and its
// clients in one process) runs under in a MemorySanitizer build and under the
// MSVC debug CRT.
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
// poisoned in every smaller-world case of the suite.
//
// The MSVC debug CRT (a Debug build with MSVC or clang-cl, _DEBUG) does to a
// planned block what MSan does: its malloc fills every byte it hands out
// (0xCD, the debug heap's clean-land fill), so every planned page is resident
// before the case runs. Measured 2026-11-11 on captain (Windows 11, MSVC 2022
// Debug, ROCKETNET_DEV=ON): test_server_stats_current_every_update peaked at
// 28,638 MiB against its 669 MiB bound of touched pages, and
// test_server_disconnect_full_queue_never_bypasses_budget at 41,258 MiB,
// 3.4 % over its whole plan, the debug heap's block headers on top of the
// fill. Neither figure is the library's: Release on the same host holds both
// bounds. So under the debug CRT such a case takes every block of a page or
// more from VirtualAlloc, demand-zero pages resident only once touched, as a
// Release build's heap leaves the untouched pages of a large block. A
// mebibyte threshold, as under MSan, was not enough: the stats case still
// peaked at 1,355 MiB, its per-client blocks under a mebibyte filled whole.
// Blocks under a page still come from the debug heap, filled and guarded.
// The cost: an overrun of a block of a page or more is not caught by the
// debug heap's guard bytes in these cases; every smaller-world case of the
// suite runs on the debug heap.
//
// In every other build the scope does nothing and the case runs on the
// default allocator.
//
// The scope terminates the worker's library and initializes it with this
// allocator, as tests/test_unit_driver_create_out_of_memory.cpp installs its
// allocator, and at its end terminates and puts the default allocator back.
// The test loopback hub (tests/loopback_hub.h) a case makes inside the scope
// is a block of this allocator, so the scope releases it before each
// terminate: the worker's release after the case would run after the term
// that finds it still allocated, and free a mapped block with free.

#pragma once

#ifndef ROCKETNET_TESTS_MSAN_SCALE_ALLOCATOR_H
#define ROCKETNET_TESTS_MSAN_SCALE_ALLOCATOR_H

#include "rocketnet_internal.h"
#include "test_unit_registry.h"
#include "loopback_hub.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined( __has_feature )
#if __has_feature( memory_sanitizer )
#define ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR 1
#endif // #if __has_feature( memory_sanitizer )
#endif // #if defined( __has_feature )

#if defined( _WIN32 ) && defined( _MSC_VER ) && defined( _DEBUG ) && !defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )
#define ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR 1
#define ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC 1
#endif // #if defined( _WIN32 ) && defined( _MSC_VER ) && defined( _DEBUG ) && !defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )

#if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )

#if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // #ifndef WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif // #if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )

enum
{
    MSAN_SCALE_HEADER_BYTES = 64,               // keeps the block 64-byte aligned
#if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
    MSAN_SCALE_MAPPED_BYTES = 4096,             // blocks of a page and up are mapped (the header comment)
#else
    MSAN_SCALE_MAPPED_BYTES = 1024 * 1024,      // blocks this size and up are mapped
#endif // #if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
};

struct msan_scale_header_t
{
    size_t mapped_bytes;                        // 0: the block came from malloc
};

inline void * msan_scale_allocate( void *, size_t bytes )
{
    if ( bytes > SIZE_MAX - MSAN_SCALE_HEADER_BYTES )
    {
        return NULL;
    }

    const size_t total = bytes + MSAN_SCALE_HEADER_BYTES;
    uint8_t * base = NULL;
    size_t mapped_bytes = 0;
    if ( bytes >= MSAN_SCALE_MAPPED_BYTES )
    {
#if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
        void * mapping = VirtualAlloc( NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
        if ( mapping == NULL )
        {
            return NULL;
        }
#else
        void * mapping = mmap( NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0 );
        if ( mapping == MAP_FAILED )
        {
            return NULL;
        }
#endif // #if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
        base = (uint8_t *) mapping;
        mapped_bytes = total;
    }
    else
    {
        base = (uint8_t *) malloc( total );
        if ( !base )
        {
            return NULL;
        }
    }

    msan_scale_header_t * header = (msan_scale_header_t *) base;
    header->mapped_bytes = mapped_bytes;
    return base + MSAN_SCALE_HEADER_BYTES;
}

inline void msan_scale_release( void *, void * pointer )
{
    if ( !pointer )
    {
        return;
    }

    uint8_t * base = (uint8_t *) pointer - MSAN_SCALE_HEADER_BYTES;
    const msan_scale_header_t * header = (const msan_scale_header_t *) base;
    if ( header->mapped_bytes )
    {
#if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
        VirtualFree( base, 0, MEM_RELEASE );
#else
        munmap( base, header->mapped_bytes );
#endif // #if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
    }
    else
    {
        free( base );
    }
}

#endif // #if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )

struct msan_scale_allocator_scope_t
{
#if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )
    msan_scale_allocator_scope_t()
    {
        rocketnet_unit_check( rocketnet_test_loopback_hub_release() );
        rocketnet_unit_check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
        int status = -1;
        rocketnet_unit_check( rocketnet_init_internal( NULL, msan_scale_allocate, msan_scale_release, &status ) );
        rocketnet_unit_check( status == ROCKETNET_STATUS_OK );
    }

    ~msan_scale_allocator_scope_t() noexcept( false ) // check() throws under the expected-fail harness
    {
        rocketnet_unit_check( rocketnet_test_loopback_hub_release() );
        rocketnet_unit_check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
        rocketnet_unit_check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    }
#else // #if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )
    msan_scale_allocator_scope_t() {} // user-provided, so a case's scope is never an unused variable
#endif // #if defined( ROCKETNET_TEST_MSAN_SCALE_ALLOCATOR )
};

// The same scope for a case whose large plan fits MSan but not the MSVC debug
// CRT's fill (a 1000-slot light server, the loopback hub sized for every
// build slot): the scale allocator under the debug CRT, nothing in every other
// build, MSan included, so those cases keep MSan's poisoning of every block.

#if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )
typedef msan_scale_allocator_scope_t debug_crt_scale_allocator_scope_t;
#else
struct debug_crt_scale_allocator_scope_t
{
    debug_crt_scale_allocator_scope_t() {} // user-provided, so a case's scope is never an unused variable
};
#endif // #if defined( ROCKETNET_TEST_SCALE_ALLOCATOR_VIRTUAL_ALLOC )

#endif // ROCKETNET_TESTS_MSAN_SCALE_ALLOCATOR_H
