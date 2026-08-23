/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Internal declarations shared between the library's translation units.

    Nothing in this header is public API. The public contract is rocketnet.h
    and only rocketnet.h; these symbols exist for the implementation and may
    change at any time.

    Implementation language per the founding record (CLAUDE.md): C-like C++.
    Internals may use what C++ genuinely gives them; the public seam stays C.
*/

#ifndef ROCKETNET_INTERNAL_H
#define ROCKETNET_INTERNAL_H

#include "rocketnet.h"

// Internal logging conveniences over rocketnet_printf. Moved from flow's
// flow_info/flow_warn/flow_error/flow_debug, which flow EXPORTED; rocketnet.h
// deliberately exports only rocketnet_printf, so these live here as internal
// helpers instead of at the seam.

void rocketnet_info( const char * format, ... );

void rocketnet_warn( const char * format, ... );

void rocketnet_error( const char * format, ... );

void rocketnet_debug( const char * format, ... );

// Moved from flow_copy_string. Bounded string copy that always terminates and
// zero-fills the destination. Returns the number of characters copied. Not at
// the public seam because a transport has no business exporting string utils.

size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size );

// The context passed to rocketnet_init, handed to the allocator hooks for
// allocations the library makes on its own behalf (none yet in stage 1).
// Per-object allocations use that object's own create context, following
// flow's per-object context pattern.

extern void * rocketnet_global_context;

#endif // #ifndef ROCKETNET_INTERNAL_H
