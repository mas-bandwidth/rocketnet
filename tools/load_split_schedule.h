#ifndef ROCKETNET_LOAD_SPLIT_SCHEDULE_H
#define ROCKETNET_LOAD_SPLIT_SCHEDULE_H

#include <stdint.h>

/* Keep an absolute one-second phase. One eligible visit offers at most one
 * message; every whole due slot missed before that visit is counted. */
static inline int split_offer_due( double offer_now, double traffic_end,
                                   double * next_due, uint64_t * missed_due_slots )
{
    if ( offer_now >= traffic_end || offer_now < *next_due ) return 0;
    const uint64_t skipped = (uint64_t) (offer_now - *next_due);
    *missed_due_slots += skipped;
    *next_due += (double) skipped + 1.0;
    return 1;
}

/* The profiles. default is the light profile: 32-byte messages at 1 Hz each
 * way. space is the space game server's traffic at 60 Hz frames: each client
 * sends one 360-byte UNRELIABLE input a frame; the server sends each client
 * one 100-byte UNRELIABLE player message a frame and, every third frame, one
 * 1184-byte UNRELIABLE_WITH_ACK_RECEIPT delta, staggered by client index
 * mod 3. The delta fits one packet. */

enum { SPLIT_PROFILE_DEFAULT = 0, SPLIT_PROFILE_SPACE = 1 };

#define SPLIT_DEFAULT_CLIENT_CAP_KBPS 100

#define SPLIT_SPACE_FRAME_HZ 60
#define SPLIT_SPACE_C2S_BYTES 360
#define SPLIT_SPACE_PLAYER_BYTES 100
#define SPLIT_SPACE_DELTA_BYTES 1184
#define SPLIT_SPACE_DELTA_STRIDE 3
#define SPLIT_SPACE_DELTA_HZ ( SPLIT_SPACE_FRAME_HZ / SPLIT_SPACE_DELTA_STRIDE )

/* The space per-client caps, strict, in kbps of wire bytes including IP/UDP
 * headers. Each packet is allowed 96 bytes beyond its message bytes: 28 of
 * IPv4/UDP, 26 of RocketNet packet (prefilter header, sequence, tag) and 42
 * for the acknowledgement block, payload form and record headers. The worst
 * case sends every delta in a packet of its own:
 *
 *   client to server  60 * (360 + 96) * 8 / 1000                   = 218.9 kbps
 *   server to client  (60 * (100 + 96) + 20 * (1184 + 96)) * 8 / 1000 = 298.9 kbps
 *
 * rounded up to the next 50 kbps. */
#define SPLIT_SPACE_PACKET_ALLOWANCE_BYTES 96
#define SPLIT_SPACE_CLIENT_UP_CAP_KBPS 250
#define SPLIT_SPACE_CLIENT_DOWN_CAP_KBPS 350

/* Frame decisions read the clock this much late, so a wake at a frame's
 * due time runs that frame rather than spinning on the rounding of
 * (now - traffic_start) * rate_hz. */
#define SPLIT_FRAME_TOLERANCE_S 2.0e-6

/* An absolute phase of rate_hz frames a second from traffic_start. Frame s
 * is due at traffic_start + s / rate_hz, and the window holds frames
 * frames. A visit before traffic_end runs the latest due frame once, puts it
 * in *frame, and counts every whole frame skipped since the last one it ran.
 * A visit before the next frame is due runs nothing. */
static inline int split_frame_due( double now, double traffic_start, double traffic_end, int rate_hz,
                                   uint64_t frames, uint64_t * next_frame, uint64_t * missed_frames,
                                   uint64_t * frame )
{
    if ( now >= traffic_end || *next_frame >= frames ) return 0;
    now += SPLIT_FRAME_TOLERANCE_S;
    if ( now < traffic_start ) return 0;
    uint64_t current = (uint64_t) ((now - traffic_start) * (double) rate_hz);
    if ( current >= frames ) current = frames - 1;
    if ( current < *next_frame ) return 0;
    *missed_frames += current - *next_frame;
    *frame = current;
    *next_frame = current + 1;
    return 1;
}

/* When the next frame after now is due: traffic_start itself before it. */
static inline double split_frame_next_time( double now, double traffic_start, int rate_hz )
{
    now += SPLIT_FRAME_TOLERANCE_S;
    if ( now < traffic_start ) return traffic_start;
    const double elapsed = (now - traffic_start) * (double) rate_hz;
    return traffic_start + ((double) (uint64_t) elapsed + 1.0) / (double) rate_hz;
}

/* The game staggers deltas across frames by client index mod 3. */
static inline int split_space_delta_due( uint64_t frame, uint32_t client )
{
    return frame % SPLIT_SPACE_DELTA_STRIDE == client % SPLIT_SPACE_DELTA_STRIDE;
}

/* Deltas one client is offered across frames 0 .. frames - 1. */
static inline uint64_t split_space_delta_offers( uint32_t client, uint64_t frames )
{
    return frames / SPLIT_SPACE_DELTA_STRIDE +
        ((uint64_t) (client % SPLIT_SPACE_DELTA_STRIDE) < frames % SPLIT_SPACE_DELTA_STRIDE ? 1u : 0u);
}

static inline uint64_t split_space_frames( int duration )
{
    return (uint64_t) duration * SPLIT_SPACE_FRAME_HZ;
}

/* The application offers one role must make: a client shard counts its
 * client-to-server offers, the server its server-to-client offers, for the
 * global clients first .. first + count - 1. */
static inline uint64_t split_expected_offers( int profile, int server, int first, int count, int duration )
{
    if ( profile != SPLIT_PROFILE_SPACE ) return (uint64_t) count * (uint64_t) duration;
    const uint64_t frames = split_space_frames(duration);
    uint64_t offers = (uint64_t) count * frames;
    if ( server )
        for ( int i = 0; i < count; ++i )
            offers += split_space_delta_offers((uint32_t) (first + i), frames);
    return offers;
}

#endif
