/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once
#include <stddef.h>
#include <stdint.h>

// A producer reservation is all-or-zero. Retain ownership on zero, publish
// every descriptor before submission on success, then relinquish the batch.
// Shared by the XDP worker and its nonprivileged conservation test.
template<size_t Capacity> struct rocketnet_pending_frames_t
{
    uint64_t frames[Capacity];
    size_t count = 0;

    size_t available() const { return Capacity - count; }

    bool append(const uint64_t * source, size_t length)
    {
        if (length > available()) return false;
        for (size_t i = 0; i < length; ++i) frames[count + i] = source[i];
        count += length;
        return true;
    }

    template<class Reserve, class Write, class Submit>
    bool flush(Reserve reserve, Write write, Submit submit)
    {
        if (!count) return true;
        uint32_t index = 0;
        if (reserve((uint32_t)count, &index) != count) return false;
        for (size_t i = 0; i < count; ++i) write(index + (uint32_t)i, frames[i]);
        submit((uint32_t)count);
        count = 0;
        return true;
    }
};

// One pass over the RX descriptors a peek returned: peeked of them, the first
// at ring index index. While staging_full() is false, take(index + i, taken)
// hands descriptor i to the staging buffer; the pass stops at the first full
// check. The peek advanced the ring's cached consumer past every peeked
// descriptor, so cancel(peeked - taken) moves it back over the descriptors
// not taken: the next peek returns them first, and the caller's
// release(taken) returns exactly the descriptors read. Returns taken.
// Shared by the XDP worker and test_unit_xdp_receive_ring.cpp; the ring's
// frame ownership is modelled in tla/RNXdpReceiveRing.tla.
template<class Full, class Take, class Cancel>
uint32_t rocketnet_xdp_receive_take(uint32_t index, uint32_t peeked, Full staging_full, Take take, Cancel cancel)
{
    uint32_t taken = 0;
    for (uint32_t i = 0; i < peeked; ++i)
    {
        if (staging_full()) break;
        take(index + i, taken);
        ++taken;
    }
    if (taken < peeked) cancel(peeked - taken);
    return taken;
}
