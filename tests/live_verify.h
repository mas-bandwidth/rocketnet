/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The live-message verify scheme and size schedule (docs/LIVE-XDP.md
    sections 3 and 4), shared as C99 static inline so the example programs
    (C) and the loopback functional test (C++) use one generator and one
    checker. No allocation, no syscall, no global state.

    The scheme is the owner's sequence-derived pair (mas-bandwidth/reliable
    soak.c generate_packet_data / check_packet_data) ported to messages: the
    length comes from the sequence, the sequence rides in the first bytes,
    and byte i is (i + sequence) % 256. The checker recomputes the length from
    the sequence it reads, so a message with the right bytes and the wrong
    length fails, and a lost, duplicated or reordered message is caught by
    its sequence.

    The schedule depends on the limits of the run's config, carried in
    rocketnet_live_config_t: the message config's max_message_bytes and the
    transport's max_payload_bytes.
*/

#ifndef ROCKETNET_LIVE_VERIFY_H
#define ROCKETNET_LIVE_VERIFY_H

#include <stdint.h>

#define ROCKETNET_LIVE_TAG_BASE                 0xA0u
#define ROCKETNET_LIVE_TAG_MASK                 0xF0u
#define ROCKETNET_LIVE_HEADER_BYTES             7
#define ROCKETNET_LIVE_NUM_MODES                8
#define ROCKETNET_LIVE_NUM_CHANNELS             32
#define ROCKETNET_LIVE_MAX_BOUNDARIES           64
#define ROCKETNET_LIVE_BLOCK_STRIDE             1152u      // F, ROCKETNET_MESSAGE_FRAGMENT_BYTES
#define ROCKETNET_LIVE_FRAGMENT_STRIDE          1413u      // the transport fragment stride
#define ROCKETNET_LIVE_CODE1_OVERHEAD           11u        // code 1's 10-byte header and the form byte
#define ROCKETNET_LIVE_MAX_RECORD_BYTES         65535u     // code 1's u16 record length
#define ROCKETNET_LIVE_UNRELIABLE_LIMIT         1411u      // 1415 - 1 - 3, codes 0 and 5 single record
#define ROCKETNET_LIVE_SLOTS                    32u

// The transport's default payload capacity (the SDK's
// ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES, internal to the library): the live
// schedule's payload bound, now that the field is gone.

// The limits the schedule is computed against (section 4, "The modes'
// maxima"), and the run seed of the bucket rule.
#define ROCKETNET_LIVE_DEFAULT_PAYLOAD_BYTES 16384

typedef struct rocketnet_live_config_t
{
    uint32_t seed;
    uint32_t max_message_bytes;         // the message config's, default 262,144
} rocketnet_live_config_t;

// The named results of a check (section 3, "What the checker records").
enum
{
    ROCKETNET_LIVE_OK = 0,
    ROCKETNET_LIVE_SHORT,               // no byte at all
    ROCKETNET_LIVE_BAD_LENGTH,          // the length differs from the rule for its sequence
    ROCKETNET_LIVE_BAD_HEADER,          // the tag: top nibble, direction or mode
    ROCKETNET_LIVE_BAD_CHANNEL,         // the sender's channel byte
    ROCKETNET_LIVE_BAD_CYCLE,           // the connection's cycle byte
    ROCKETNET_LIVE_BAD_BYTE,            // a byte at an offset
};

// The size buckets as named in the results (section 4).
enum
{
    ROCKETNET_LIVE_BUCKET_TINY = 0,
    ROCKETNET_LIVE_BUCKET_SMALL,
    ROCKETNET_LIVE_BUCKET_EDGE,
    ROCKETNET_LIVE_BUCKET_FRAGMENT,
    ROCKETNET_LIVE_BUCKET_MEDIUM,
    ROCKETNET_LIVE_BUCKET_LARGE,
    ROCKETNET_LIVE_BUCKET_MAX,
    ROCKETNET_LIVE_BUCKET_OVER,
    ROCKETNET_LIVE_NUM_BUCKETS
};

static inline const char * rocketnet_live_result_name( int result )
{
    switch ( result )
    {
        case ROCKETNET_LIVE_OK:             return "ok";
        case ROCKETNET_LIVE_SHORT:          return "short";
        case ROCKETNET_LIVE_BAD_LENGTH:     return "bad-length";
        case ROCKETNET_LIVE_BAD_HEADER:     return "bad-header";
        case ROCKETNET_LIVE_BAD_CHANNEL:    return "bad-channel";
        case ROCKETNET_LIVE_BAD_CYCLE:      return "bad-cycle";
        case ROCKETNET_LIVE_BAD_BYTE:       return "bad-byte";
        default:                            return "unknown";
    }
}

static inline const char * rocketnet_live_bucket_name( int bucket )
{
    static const char * const names[ROCKETNET_LIVE_NUM_BUCKETS] = { "tiny", "small", "edge", "fragment", "medium", "large", "max", "over" };
    return bucket >= 0 && bucket < ROCKETNET_LIVE_NUM_BUCKETS ? names[bucket] : "unknown";
}

// Section 3, the message header: byte 0 is 0xA0 | direction << 3 | mode.
// Direction 0 is client to server.
static inline uint8_t rocketnet_live_tag( int direction, int mode )
{
    return (uint8_t) ( ROCKETNET_LIVE_TAG_BASE | ( (uint32_t) ( direction & 1 ) << 3 ) | (uint32_t) ( mode & 7 ) );
}

// Section 4, "The modes' maxima": the largest message a mode accepts.
// Code 0: the single-record unreliable limit. Code 1: the smallest of its
// u16 record length, max_message_bytes and the transport payload cap less
// its 11-byte overhead (channels below 256). The reliable codes and code 5
// (promoted to a reliable block above 1,411): max_message_bytes.
static inline uint32_t rocketnet_live_mode_max( const rocketnet_live_config_t * config, int mode )
{
    const uint32_t mmb = config->max_message_bytes;
    if ( mode == 0 )
        return ROCKETNET_LIVE_UNRELIABLE_LIMIT < mmb ? ROCKETNET_LIVE_UNRELIABLE_LIMIT : mmb;
    if ( mode == 1 )
    {
        uint32_t m = ROCKETNET_LIVE_MAX_RECORD_BYTES;
        if ( mmb < m ) m = mmb;
        const uint32_t payload = ROCKETNET_LIVE_DEFAULT_PAYLOAD_BYTES > ROCKETNET_LIVE_CODE1_OVERHEAD ? ROCKETNET_LIVE_DEFAULT_PAYLOAD_BYTES - ROCKETNET_LIVE_CODE1_OVERHEAD : 1u;
        if ( payload < m ) m = payload;
        return m;
    }
    return mmb;
}

// Section 4, the single-record edge E_m: 1,411 for codes 0 and 5, 1,401 for
// codes 3 and 7, 1,408 for code 2 and 1,399 for code 4 (derived from their
// header widths); code 6 travels as code 2 and takes its edge. Code 1 has no record edge of its own; its edge is the
// largest message whose payload fits one transport packet, 1,415 - 11 =
// 1,404, the first of its transport payload edges. Clipped to the mode's
// maximum.
static inline uint32_t rocketnet_live_edge( const rocketnet_live_config_t * config, int mode )
{
    uint32_t e;
    switch ( mode )
    {
        case 0: case 5: e = 1411u; break;
        case 2: case 6: e = 1408u; break;
        case 3: case 7: e = 1401u; break;
        case 4:         e = 1399u; break;
        default:        e = 1404u; break;
    }
    const uint32_t m = rocketnet_live_mode_max( config, mode );
    return e < m ? e : m;
}

static inline void rocketnet_live_boundary_add( uint32_t * list, int * count, int64_t value, uint32_t max_m )
{
    // section 4: each value is clipped to the mode's maximum and skipped when
    // it is below 1; a value already in the list is not added twice
    if ( value < 1 )
        return;
    const uint32_t v = value > (int64_t) max_m ? max_m : (uint32_t) value;
    for ( int i = 0; i < *count; i++ )
    {
        if ( list[i] == v )
            return;
    }
    if ( *count < ROCKETNET_LIVE_MAX_BOUNDARIES )
        list[(*count)++] = v;
}

// Section 4, the boundary list B of a mode, in the order the section lists
// it: the header prefix, the single-record edge at -1, 0, +1, code 1's
// transport payload edges, the block stride's multiples, and the
// near-maximum values. Returns the count; list holds
// ROCKETNET_LIVE_MAX_BOUNDARIES entries.
static inline int rocketnet_live_boundaries( const rocketnet_live_config_t * config, int mode, uint32_t * list )
{
    const uint32_t m = rocketnet_live_mode_max( config, mode );
    const int64_t e = (int64_t) rocketnet_live_edge( config, mode );
    const int64_t F = (int64_t) ROCKETNET_LIVE_BLOCK_STRIDE;
    int n = 0;
    for ( int64_t i = 1; i <= 7; i++ )
        rocketnet_live_boundary_add( list, &n, i, m );
    rocketnet_live_boundary_add( list, &n, e - 1, m );
    rocketnet_live_boundary_add( list, &n, e, m );
    rocketnet_live_boundary_add( list, &n, e + 1, m );
    if ( mode == 1 )
    {
        const int64_t o = (int64_t) ROCKETNET_LIVE_CODE1_OVERHEAD;
        rocketnet_live_boundary_add( list, &n, 1404, m );
        rocketnet_live_boundary_add( list, &n, 1405, m );
        rocketnet_live_boundary_add( list, &n, 1406, m );
        for ( int64_t k = 2; k <= 3; k++ )
        {
            const int64_t full = k * (int64_t) ROCKETNET_LIVE_FRAGMENT_STRIDE - o;
            rocketnet_live_boundary_add( list, &n, full - 1, m );
            rocketnet_live_boundary_add( list, &n, full, m );
            rocketnet_live_boundary_add( list, &n, full + 1, m );
        }
        const int64_t payload_max = (int64_t) ROCKETNET_LIVE_DEFAULT_PAYLOAD_BYTES - o;
        rocketnet_live_boundary_add( list, &n, payload_max - 1, m );
        rocketnet_live_boundary_add( list, &n, payload_max, m );
        rocketnet_live_boundary_add( list, &n, payload_max + 1, m );
    }
    for ( int64_t k = 1; k <= 3; k++ )
    {
        rocketnet_live_boundary_add( list, &n, k * F - 1, m );
        rocketnet_live_boundary_add( list, &n, k * F, m );
        rocketnet_live_boundary_add( list, &n, k * F + 1, m );
    }
    rocketnet_live_boundary_add( list, &n, 16383, m );
    rocketnet_live_boundary_add( list, &n, 16384, m );
    rocketnet_live_boundary_add( list, &n, 16385, m );
    rocketnet_live_boundary_add( list, &n, (int64_t) m - F - 1, m );
    rocketnet_live_boundary_add( list, &n, (int64_t) m - F, m );
    rocketnet_live_boundary_add( list, &n, (int64_t) m - 1, m );
    rocketnet_live_boundary_add( list, &n, (int64_t) m, m );
    return n;
}

// Section 4, the bucket rule: lo + (((s + seed) * 1023) % (hi - lo + 1)) in
// 64-bit unsigned arithmetic.
static inline uint32_t rocketnet_live_bucket_length( uint32_t lo, uint32_t hi, uint32_t sequence, uint32_t seed )
{
    const uint64_t span = (uint64_t) hi - (uint64_t) lo + 1u;
    return (uint32_t) ( (uint64_t) lo + ( ( ( (uint64_t) sequence + (uint64_t) seed ) * 1023u ) % span ) );
}

// Section 4, the length rule: sequence s of stream (mode, channel) takes slot
// r = (s + channel) % 32. r = 0 is the boundary slot, B[(s / 32 + channel x
// ceil(|B| / 32)) % |B|]; r = 1 to 23 small (7 to E_m); r = 24 to 29 medium
// (E_m + 1 to min(16,384, max_m)); r = 30 and 31 large (16,385 to max_m).
// A bucket that is empty at this mode's maximum falls back to small, as the
// section says for large; a small bucket with no room above the header is
// the maximum itself.
static inline uint32_t rocketnet_live_length( const rocketnet_live_config_t * config, int mode, int channel, uint32_t sequence )
{
    const uint32_t m = rocketnet_live_mode_max( config, mode );
    const uint32_t e = rocketnet_live_edge( config, mode );
    const uint32_t r = (uint32_t) ( ( (uint64_t) sequence + (uint32_t) channel ) % ROCKETNET_LIVE_SLOTS );

    if ( r == 0 )
    {
        uint32_t list[ROCKETNET_LIVE_MAX_BOUNDARIES];
        const int n = rocketnet_live_boundaries( config, mode, list );
        const uint64_t per_channel = ( (uint64_t) n + ROCKETNET_LIVE_SLOTS - 1 ) / ROCKETNET_LIVE_SLOTS;
        const uint64_t index = ( (uint64_t) ( sequence / ROCKETNET_LIVE_SLOTS ) + (uint64_t) channel * per_channel ) % (uint64_t) n;
        return list[index];
    }

    const uint32_t small_lo = 7u < e ? 7u : e;
    if ( r >= 24u && r <= 29u )
    {
        const uint32_t hi = m < 16384u ? m : 16384u;
        if ( e + 1u <= hi )
            return rocketnet_live_bucket_length( e + 1u, hi, sequence, config->seed );
    }
    else if ( r >= 30u )
    {
        if ( m >= 16385u )
            return rocketnet_live_bucket_length( 16385u, m, sequence, config->seed );
    }
    return rocketnet_live_bucket_length( small_lo, e, sequence, config->seed );
}

// The number of sequences a stream sends so that every (direction, mode)
// walks all of B once every channel has passed them: 32 x ceil(|B| / 32)
// (section 4, r = 0).
static inline uint32_t rocketnet_live_sequences_per_stream( const rocketnet_live_config_t * config, int mode )
{
    uint32_t list[ROCKETNET_LIVE_MAX_BOUNDARIES];
    const int n = rocketnet_live_boundaries( config, mode, list );
    return ROCKETNET_LIVE_SLOTS * ( ( (uint32_t) n + ROCKETNET_LIVE_SLOTS - 1 ) / ROCKETNET_LIVE_SLOTS );
}

// Section 4, the size bucket a length of a mode falls in, as the results
// name it.
static inline int rocketnet_live_bucket( const rocketnet_live_config_t * config, int mode, uint32_t length )
{
    const uint32_t m = rocketnet_live_mode_max( config, mode );
    const uint32_t e = rocketnet_live_edge( config, mode );
    if ( length > m )
        return ROCKETNET_LIVE_BUCKET_OVER;
    if ( length == m )
        return ROCKETNET_LIVE_BUCKET_MAX;
    if ( length < ROCKETNET_LIVE_HEADER_BYTES )
        return ROCKETNET_LIVE_BUCKET_TINY;
    if ( length + 1u >= e && length <= e + 1u )
        return ROCKETNET_LIVE_BUCKET_EDGE;
    for ( uint32_t k = 1; k <= 3; k++ )
    {
        const uint32_t f = k * ROCKETNET_LIVE_BLOCK_STRIDE;
        if ( length + 1u >= f && length <= f + 1u )
            return ROCKETNET_LIVE_BUCKET_FRAGMENT;
        if ( mode == 1 && k >= 2 )
        {
            const uint32_t full = k * ROCKETNET_LIVE_FRAGMENT_STRIDE - ROCKETNET_LIVE_CODE1_OVERHEAD;
            if ( length + 1u >= full && length <= full + 1u )
                return ROCKETNET_LIVE_BUCKET_FRAGMENT;
        }
    }
    if ( length <= e )
        return ROCKETNET_LIVE_BUCKET_SMALL;
    if ( length <= 16384u )
        return ROCKETNET_LIVE_BUCKET_MEDIUM;
    return ROCKETNET_LIVE_BUCKET_LARGE;
}

// Section 3: write the message of (direction, mode, channel, cycle, sequence)
// into buffer. A message of 7 bytes or more is the header (tag, channel,
// cycle mod 256, sequence u32 little endian) and then byte i = (i + sequence)
// % 256; a shorter one is the first L bytes of the header and nothing else.
// Returns the length, or 0 when it does not fit capacity.
static inline int rocketnet_live_generate( const rocketnet_live_config_t * config, int direction, int mode, int channel,
                                           uint32_t cycle, uint32_t sequence, uint8_t * buffer, int capacity )
{
    const uint32_t length = rocketnet_live_length( config, mode, channel, sequence );
    if ( capacity < 0 || length > (uint32_t) capacity )
        return 0;
    uint8_t header[ROCKETNET_LIVE_HEADER_BYTES];
    header[0] = rocketnet_live_tag( direction, mode );
    header[1] = (uint8_t) channel;
    header[2] = (uint8_t) cycle;
    header[3] = (uint8_t) ( sequence );
    header[4] = (uint8_t) ( sequence >> 8 );
    header[5] = (uint8_t) ( sequence >> 16 );
    header[6] = (uint8_t) ( sequence >> 24 );
    for ( uint32_t i = 0; i < length && i < ROCKETNET_LIVE_HEADER_BYTES; i++ )
        buffer[i] = header[i];
    for ( uint32_t i = ROCKETNET_LIVE_HEADER_BYTES; i < length; i++ )
        buffer[i] = (uint8_t) ( ( i + sequence ) % 256u );
    return (int) length;
}

// The sequence a message of 7 bytes or more carries (section 3, bytes 3 to
// 6). 0 for a shorter one, which carries none.
static inline uint32_t rocketnet_live_sequence( const uint8_t * buffer, int bytes )
{
    if ( bytes < ROCKETNET_LIVE_HEADER_BYTES )
        return 0;
    return (uint32_t) buffer[3] | ( (uint32_t) buffer[4] << 8 ) | ( (uint32_t) buffer[5] << 16 ) | ( (uint32_t) buffer[6] << 24 );
}

// Section 3: check a received message against the stream the receiver
// expects (direction, mode, the sender's channel, the connection's cycle).
// A message of 7 bytes or more has its length recomputed from the sequence
// it carries and every byte checked. A shorter one proves the header bytes
// it carries; when the receiver knows the sequence it expects
// (expected_sequence >= 0, the ordered modes), its length and sequence bytes
// are checked against that sequence too. bad_offset, when not NULL, receives
// the offset of the first wrong byte.
static inline int rocketnet_live_check( const rocketnet_live_config_t * config, const uint8_t * buffer, int bytes,
                                        int direction, int mode, int channel, uint32_t cycle,
                                        int64_t expected_sequence, int * bad_offset )
{
    if ( bad_offset )
        *bad_offset = -1;
    if ( !buffer || bytes < 1 )
        return ROCKETNET_LIVE_SHORT;
    if ( buffer[0] != rocketnet_live_tag( direction, mode ) )
    {
        if ( bad_offset ) *bad_offset = 0;
        return ROCKETNET_LIVE_BAD_HEADER;
    }
    if ( bytes >= 2 && buffer[1] != (uint8_t) channel )
    {
        if ( bad_offset ) *bad_offset = 1;
        return ROCKETNET_LIVE_BAD_CHANNEL;
    }
    if ( bytes >= 3 && buffer[2] != (uint8_t) cycle )
    {
        if ( bad_offset ) *bad_offset = 2;
        return ROCKETNET_LIVE_BAD_CYCLE;
    }

    uint32_t sequence;
    if ( bytes >= ROCKETNET_LIVE_HEADER_BYTES )
    {
        sequence = rocketnet_live_sequence( buffer, bytes );
    }
    else
    {
        if ( expected_sequence < 0 )
            return ROCKETNET_LIVE_OK;           // the stated blind spot: no sequence to check against
        sequence = (uint32_t) expected_sequence;
    }

    if ( (uint32_t) bytes != rocketnet_live_length( config, mode, channel, sequence ) )
        return ROCKETNET_LIVE_BAD_LENGTH;

    for ( int i = 3; i < bytes && i < ROCKETNET_LIVE_HEADER_BYTES; i++ )
    {
        if ( buffer[i] != (uint8_t) ( sequence >> ( 8 * ( i - 3 ) ) ) )
        {
            if ( bad_offset ) *bad_offset = i;
            return ROCKETNET_LIVE_BAD_BYTE;
        }
    }
    for ( int i = ROCKETNET_LIVE_HEADER_BYTES; i < bytes; i++ )
    {
        if ( buffer[i] != (uint8_t) ( ( (uint32_t) i + sequence ) % 256u ) )
        {
            if ( bad_offset ) *bad_offset = i;
            return ROCKETNET_LIVE_BAD_BYTE;
        }
    }
    return ROCKETNET_LIVE_OK;
}

#endif // #ifndef ROCKETNET_LIVE_VERIFY_H
