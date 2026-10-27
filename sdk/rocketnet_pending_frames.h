/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_PENDING_FRAMES_H
#define ROCKETNET_SDK_ROCKETNET_PENDING_FRAMES_H

#include <stddef.h>
#include <stdint.h>

// A producer reservation is all-or-zero. Retain ownership on zero, publish
// every descriptor before submission on success, then relinquish the batch.
// Shared by the XDP worker and its nonprivileged conservation test.
template <size_t Capacity> struct rocketnet_pending_frames_t
{
    uint64_t frames[Capacity];
    size_t count = 0;

    size_t available() const { return Capacity - count; }

    bool append( const uint64_t * source, size_t length )
    {
        if ( length > available() )
            return false;

        for ( size_t i = 0; i < length; ++i )
            frames[count + i] = source[i];

        count += length;

        return true;
    }

    template <class Reserve, class Write, class Submit>
    bool flush( Reserve reserve, Write write, Submit submit )
    {
        if ( !count )
            return true;

        uint32_t index = 0;
        if ( reserve( (uint32_t) count, &index ) != count )
            return false;

        for ( size_t i = 0; i < count; ++i )
            write( index + (uint32_t) i, frames[i] );

        submit( (uint32_t) count );
        count = 0;

        return true;
    }
};

#endif // #ifndef ROCKETNET_SDK_ROCKETNET_PENDING_FRAMES_H
