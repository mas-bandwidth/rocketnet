/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The live-message verify scheme, shared as C99 static inline so the
    example programs and the loopback functional test use one generator and
    one checker. No allocation, no syscall, no global state.

    docs/LIVE-XDP.md section 3 (verify scheme) and section 4 (sizes). The
    sequence-derived pair of the owner's soak (mas-bandwidth/reliable
    soak.c, generate_packet_data / check_packet_data) ported to messages:

    - The stream header (section 3): byte 0 is the tag
      `0xA0 | direction << 3 | mode`; byte 1 is the channel; byte 2 is the
      connection's cycle mod 256; bytes 3..6 are the stream sequence, u32
      little endian; bytes 7 on are `(i + sequence) % 256`. A message
      shorter than 7 bytes carries the first L bytes of the header and
      nothing else.
    - The length of a stream's sequence s follows section 4's schedule: slot
      r = (s + channel) % 32 picks the boundary list (r == 0) or a size
      bucket (small, medium, large), each spread by the seed, clipped to the
      stream's effective maximum. The checker recomputes the length from the
      sequence it reads, so a message with the right bytes but the wrong
      length fails.
*/

#ifndef ROCKETNET_LIVE_VERIFY_H
#define ROCKETNET_LIVE_VERIFY_H

#include <stdint.h>

#define ROCKETNET_LIVE_TAG_BASE  0xA0u
#define ROCKETNET_LIVE_HEADER    7

enum
{
    ROCKETNET_LIVE_OK = 0,
    ROCKETNET_LIVE_SHORT,
    ROCKETNET_LIVE_BAD_LENGTH,
    ROCKETNET_LIVE_BAD_HEADER,
    ROCKETNET_LIVE_BAD_BYTE,
};

// the tag byte: `0xA0 | direction << 3 | mode` (section 3). Direction 0 is
// client to server, 1 server to client.
static inline uint8_t rocketnet_live_tag( int direction, int mode )
{
    return (uint8_t) ( ROCKETNET_LIVE_TAG_BASE |
                        ( ( (uint8_t) ( direction & 0x07 ) ) << 3 ) |
                        ( (uint8_t) ( mode & 0x07 ) ) );
}

// the single-record edge E_m of a mode (section 4). Values are the mode's
// single-record boundary: 1411 for codes 0 and 5, 1401 for codes 3 and 7,
// 1408 for code 2, 1399 for code 4; code 1's payload edges start at 1404.
static inline uint32_t rocketnet_live_edge( int mode )
{
    switch ( mode )
    {
        case 0:
        case 5: return 1411u;
        case 2: return 1408u;
        case 3:
        case 7: return 1401u;
        case 4: return 1399u;
        default: return 1404u;   // mode 1
    }
}

// a stream's effective maximum message length (section 4): mode 0 caps at
// its single-record limit, mode 1 at its one-RECORDS-payload cap, and the
// reliable modes at the configured max_message_bytes. The mode 1 cap is the
// transport payload maximum less the 10-byte header and the form byte
// (sdk/rocketnet_endpoint.cpp:150-157).
static inline uint32_t rocketnet_live_mode_max( int mode, uint32_t max_m )
{
    switch ( mode )
    {
        case 0:
        {
            uint32_t e = rocketnet_live_edge( mode );
            return e < max_m ? e : max_m;
        }
        case 1:
        {
            uint32_t c = 1405u;
            return c < max_m ? c : max_m;
        }
        default:
            return max_m;
    }
}

static inline void rocketnet_live_b_add( uint32_t * b, int * n, uint64_t v, uint32_t max )
{
    if ( v >= 1 && v <= max )
        b[(*n)++] = (uint32_t) v;
}

// the boundary list B of section 4, clipped to the stream's effective
// maximum: the header prefix, the single-record edge and its neighbours, the
// block stride F and its neighbours, and the near-maximum values. Returns the
// populated count.
static inline int rocketnet_live_boundaries( int mode, uint32_t max_m, uint32_t * b )
{
    const uint32_t F = 1152u;
    const uint32_t m = rocketnet_live_mode_max( mode, max_m );
    const uint32_t e = rocketnet_live_edge( mode );
    int n = 0;
    for ( uint32_t i = 1; i <= 7 && i <= m; i++ )
        b[n++] = i;
    rocketnet_live_b_add( b, &n, (uint64_t) e - 1, m );
    rocketnet_live_b_add( b, &n, e, m );
    rocketnet_live_b_add( b, &n, (uint64_t) e + 1, m );
    for ( uint32_t k = 1; k <= 3; k++ )
    {
        uint64_t fv = (uint64_t) k * (uint64_t) F;
        rocketnet_live_b_add( b, &n, fv - 1, m );
        rocketnet_live_b_add( b, &n, fv, m );
        rocketnet_live_b_add( b, &n, fv + 1, m );
    }
    if ( m >= F + 1 ) rocketnet_live_b_add( b, &n, (uint64_t) m - F - 1, m );
    if ( m >= F ) rocketnet_live_b_add( b, &n, (uint64_t) m - F, m );
    if ( m >= 1 ) rocketnet_live_b_add( b, &n, (uint64_t) m - 1, m );
    rocketnet_live_b_add( b, &n, m, m );
    return n;
}

// the length of sequence s on a stream (section 4): slot r = (s + channel) %
// 32 selects the boundary list or a size bucket, spread by the seed and
// clipped to the effective maximum.
static inline uint32_t rocketnet_live_stream_len( int mode, int channel, uint32_t seed, uint32_t s, uint32_t max_m )
{
    const uint32_t m = rocketnet_live_mode_max( mode, max_m );
    uint32_t e = rocketnet_live_edge( mode );
    if ( e > m ) e = m;

    uint32_t b[64];
    const int nb = rocketnet_live_boundaries( mode, max_m, b );
    const uint32_t r = ( s + (uint32_t) channel ) % 32u;

    if ( r == 0 )
    {
        if ( nb == 0 ) return 1;
        const uint32_t q = s / 32u + (uint32_t) channel * ( ( (uint32_t) nb + 31u ) / 32u );
        return b[ q % (uint32_t) nb ];
    }

    uint32_t lo, hi;
    if ( r <= 23u ) { lo = 7u; hi = e; }
    else if ( r <= 29u ) { lo = e + 1u; hi = m; if ( lo > hi ) lo = hi = m; }
    else { lo = 7u; hi = e; }

    const uint64_t span = (uint64_t) hi - lo + 1u;
    uint64_t val = lo + ( ( ( (uint64_t) s + seed ) * 1023ULL ) % span );
    if ( val > m ) val = m;
    if ( val < 1 ) val = 1;
    return (uint32_t) val;
}

// generate a message for (direction, mode, channel, sequence) into buf, which
// must hold capacity bytes. Returns the message length, or 0 when the length
// cannot fit.
static inline int rocketnet_live_generate( int direction, int mode, int channel, uint32_t cycle,
                                           uint32_t s, uint32_t seed, uint32_t max_m,
                                           uint8_t * buf, int capacity )
{
    const uint32_t len = rocketnet_live_stream_len( mode, channel, seed, s, max_m );
    if ( len < 1 || (int) len > capacity ) return 0;
    const uint8_t tag = rocketnet_live_tag( direction, mode );
    const uint8_t ch = (uint8_t) ( channel & 0xFF );
    const uint8_t cy = (uint8_t) ( cycle & 0xFF );

    if ( len >= ROCKETNET_LIVE_HEADER )
    {
        buf[0] = tag;
        buf[1] = ch;
        buf[2] = cy;
        buf[3] = (uint8_t) ( s & 0xFF );
        buf[4] = (uint8_t) ( ( s >> 8 ) & 0xFF );
        buf[5] = (uint8_t) ( ( s >> 16 ) & 0xFF );
        buf[6] = (uint8_t) ( ( s >> 24 ) & 0xFF );
        for ( uint32_t i = 7; i < len; i++ )
            buf[i] = (uint8_t) ( ( i + s ) % 256u );
    }
    else
    {
        buf[0] = tag;
        if ( len > 1 ) buf[1] = ch;
        if ( len > 2 ) buf[2] = cy;
        if ( len > 3 ) buf[3] = (uint8_t) ( s & 0xFF );
        if ( len > 4 ) buf[4] = (uint8_t) ( ( s >> 8 ) & 0xFF );
        if ( len > 5 ) buf[5] = (uint8_t) ( ( s >> 16 ) & 0xFF );
    }
    return (int) len;
}

// check a received message and return a named result (ok, short, bad length,
// bad header, bad byte at offset). The checker recomputes the length from the
// sequence it reads, so a wrong-length message fails even when its bytes are
// right. bad_offset, when non-NULL, receives the offset of a bad byte.
static inline int rocketnet_live_check( const uint8_t * buf, int bytes, int direction, int mode,
                                        int channel, uint32_t cycle, uint32_t seed, uint32_t max_m,
                                        int * bad_offset )
{
    if ( bytes < 1 ) return ROCKETNET_LIVE_SHORT;
    if ( buf[0] != rocketnet_live_tag( direction, mode ) )
        return ROCKETNET_LIVE_BAD_HEADER;
    if ( bytes >= 2 && buf[1] != (uint8_t) ( channel & 0xFF ) )
        return ROCKETNET_LIVE_BAD_HEADER;
    if ( bytes >= 3 && buf[2] != (uint8_t) ( cycle & 0xFF ) )
        return ROCKETNET_LIVE_BAD_HEADER;

    if ( bytes >= ROCKETNET_LIVE_HEADER )
    {
        const uint32_t s = (uint32_t) buf[3] | ( (uint32_t) buf[4] << 8 ) |
                            ( (uint32_t) buf[5] << 16 ) | ( (uint32_t) buf[6] << 24 );
        const uint32_t expect = rocketnet_live_stream_len( mode, channel, seed, s, max_m );
        if ( (uint32_t) bytes != expect ) return ROCKETNET_LIVE_BAD_LENGTH;
        for ( int i = 7; i < bytes; i++ )
        {
            if ( buf[i] != (uint8_t) ( ( (uint32_t) i + s ) % 256u ) )
            {
                if ( bad_offset ) *bad_offset = i;
                return ROCKETNET_LIVE_BAD_BYTE;
            }
        }
    }
    return ROCKETNET_LIVE_OK;
}

#endif // ROCKETNET_LIVE_VERIFY_H
