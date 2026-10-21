/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_message.h"
#include "rocketnet_platform.h"
#include "rocketnet_read_write.h"

#include <string.h>

// Layer 2, message records: STANDARD.md _Messages_ and docs/CHANNELS.md
// sections 5 and 13. No TLA+ module covers this layer: it holds no state
// between calls, so it is checked by known-answer vectors, the pack/parse
// property test and the fuzz target (CHANNELS.md 3, layer 2 row).

const char * rocketnet_message_violation_name( int violation )
{
    switch ( violation )
    {
        case ROCKETNET_MESSAGE_VIOLATION_NONE:              return "none";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_FORM:          return "MESSAGE_BAD_FORM";
        case ROCKETNET_MESSAGE_VIOLATION_TRUNCATED:         return "MESSAGE_TRUNCATED";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS:         return "MESSAGE_BAD_FLAGS";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH:        return "MESSAGE_BAD_LENGTH";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL:       return "MESSAGE_BAD_CHANNEL";
        case ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED:  return "MESSAGE_TYPE_NOT_ENABLED";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK:         return "MESSAGE_BAD_BLOCK";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_RING:          return "MESSAGE_BAD_RING";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE:          return "MESSAGE_BAD_SIZE";
        case ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL:       return "MESSAGE_BAD_CONTROL";
        case ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW: return "MESSAGE_NUMBER_OUTSIDE_WINDOW";
        case ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION:        return "MESSAGE_SLOT_COLLISION";
        default:                                            return "unknown";
    }
}

// -----------------------------------------
//                     the fields of a code
// -----------------------------------------

// Which fields a record carries, from its flags byte alone (STANDARD.md
// _Messages_, the record table). Defined for every flags byte, valid or not,
// so the receiver can run check 2 (the header fits) before check 3 (the
// flags are allowed), in the standard's order.

static inline bool rocketnet_message_code_has_number( int code )        { return code == 2 || code == 3 || code == 4; }
static inline bool rocketnet_message_code_has_sequencing( int code )    { return code == 1 || code == 4; }
static inline bool rocketnet_message_code_has_channel( int code )       { return code == 1 || code == 3 || code == 4; }
static inline bool rocketnet_message_code_is_holdable( int code )       { return code == 3 || code == 4; }

static int rocketnet_message_flags_header_bytes( uint8_t flags )
{
    const int code = flags & ROCKETNET_MESSAGE_FLAG_CODE_MASK;
    const bool block = ( flags & ROCKETNET_MESSAGE_FLAG_BLOCK ) != 0;
    const bool wide = ( flags & ROCKETNET_MESSAGE_FLAG_WIDE ) != 0;
    int bytes = ROCKETNET_MESSAGE_RECORD_FIXED_BYTES;
    if ( rocketnet_message_code_has_number( code ) )
        bytes += 3;
    if ( rocketnet_message_code_is_holdable( code ) || block )
        bytes += 2;
    if ( rocketnet_message_code_has_sequencing( code ) )
        bytes += 3;
    if ( rocketnet_message_code_has_channel( code ) )
        bytes += 3 + ( wide ? 2 : 1 );     // ordering, then channel
    if ( block )
        bytes += 4;
    return bytes;
}

// Check 3 (STANDARD.md _Messages_): the flag combinations a correct sender writes.

static bool rocketnet_message_flags_allowed( uint8_t flags )
{
    const int code = flags & ROCKETNET_MESSAGE_FLAG_CODE_MASK;
    const bool block = ( flags & ROCKETNET_MESSAGE_FLAG_BLOCK ) != 0;
    const bool wide = ( flags & ROCKETNET_MESSAGE_FLAG_WIDE ) != 0;
    const bool control = ( flags & ROCKETNET_MESSAGE_FLAG_CONTROL ) != 0;
    if ( code >= ROCKETNET_MESSAGE_NUM_CODES )
        return false;                                               // L2.1: 5 to 7 never reach the wire
    if ( flags & ROCKETNET_MESSAGE_FLAG_RESERVED )
        return false;
    if ( block && ( code == ROCKETNET_MESSAGE_CODE_UNRELIABLE || code == ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED ) )
        return false;                                               // only reliable messages are blocks (CHANNELS.md 8)
    if ( wide && !rocketnet_message_code_has_channel( code ) )
        return false;
    if ( control && ( code != ROCKETNET_MESSAGE_CODE_RELIABLE || block ) )
        return false;                                               // CONTROL is code 2, never a block (CHANNELS.md 10.3)
    return true;
}

static uint8_t rocketnet_message_record_flags( const struct rocketnet_message_record_t * record )
{
    uint8_t flags = (uint8_t) ( record->code & ROCKETNET_MESSAGE_FLAG_CODE_MASK );
    if ( record->block )
        flags |= ROCKETNET_MESSAGE_FLAG_BLOCK;
    if ( rocketnet_message_code_has_channel( record->code ) && record->channel >= 256 )
        flags |= ROCKETNET_MESSAGE_FLAG_WIDE;                       // exactly when channel >= 256
    if ( record->control )
        flags |= ROCKETNET_MESSAGE_FLAG_CONTROL;
    return flags;
}

int rocketnet_message_record_header_bytes( const struct rocketnet_message_record_t * record )
{
    rocketnet_assert( record );
    return rocketnet_message_flags_header_bytes( rocketnet_message_record_flags( record ) );
}

int rocketnet_message_record_valid( const struct rocketnet_message_record_t * record )
{
    if ( !record )
        return 0;
    if ( record->code < 0 || record->code >= ROCKETNET_MESSAGE_NUM_CODES )
        return 0;
    if ( !rocketnet_message_flags_allowed( rocketnet_message_record_flags( record ) ) )
        return 0;
    if ( record->bytes < 1 || record->bytes > ROCKETNET_MESSAGE_MAX_RECORD_BYTES || !record->data )
        return 0;

    const int code = record->code;
    const bool block = record->block != 0;

    // absent fields are zero, present ones inside their wire widths

    if ( record->number > ( rocketnet_message_code_has_number( code ) ? ROCKETNET_MESSAGE_U24_MASK : 0u ) )
        return 0;
    if ( record->ring > ( rocketnet_message_code_is_holdable( code ) || block ? 0xFFFFu : 0u ) )
        return 0;
    if ( record->sequencing > ( rocketnet_message_code_has_sequencing( code ) ? ROCKETNET_MESSAGE_U24_MASK : 0u ) )
        return 0;
    if ( record->ordering > ( rocketnet_message_code_has_channel( code ) ? ROCKETNET_MESSAGE_U24_MASK : 0u ) )
        return 0;
    if ( record->channel > ( rocketnet_message_code_has_channel( code ) ? 0xFFFFu : 0u ) )
        return 0;
    if ( record->fragments > ( block ? 0xFFFFu : 0u ) || record->fragment > record->fragments )
        return 0;
    if ( block && record->fragments == 0 )
        return 0;                                                   // a block has at least two fragments

    return 1;
}

// -----------------------------------------
//                                  writing
// -----------------------------------------

static inline void rocketnet_message_write_uint24( uint8_t ** p, uint32_t value )
{
    (*p)[0] = (uint8_t) ( value & 0xFF );
    (*p)[1] = (uint8_t) ( ( value >> 8 ) & 0xFF );
    (*p)[2] = (uint8_t) ( ( value >> 16 ) & 0xFF );
    *p += 3;
}

static inline uint32_t rocketnet_message_read_uint24( const uint8_t ** p )
{
    uint32_t value = (uint32_t) (*p)[0];
    value |= (uint32_t) (*p)[1] << 8;
    value |= (uint32_t) (*p)[2] << 16;
    *p += 3;
    return value;
}

void rocketnet_message_writer_begin( struct rocketnet_message_writer_t * writer, uint8_t * payload, int capacity )
{
    rocketnet_assert( writer );
    rocketnet_assert( payload );
    rocketnet_assert( capacity >= 0 );
    writer->payload = payload;
    writer->capacity = capacity;
    writer->bytes = 0;
    writer->records = 0;
}

int rocketnet_message_writer_space( const struct rocketnet_message_writer_t * writer )
{
    rocketnet_assert( writer );
    const int space = writer->capacity - 1 - writer->bytes;        // the form byte is always reserved
    return space > 0 ? space : 0;
}

// The record layout, field by field in the standard's order (STANDARD.md
// _Messages_; CHANNELS.md 13.2): flags, length, then number, ring,
// sequencing, ordering, channel, fragment, fragments, each only when its
// code or flags carry it, then the data.

#if defined(ROCKETNET_MESSAGE_COPY_TEST_HOOKS)
static volatile uint64_t rn_message_payload_copies = 0;

uint64_t rn_message_payload_copy_count()
{
    return rocketnet_platform_atomic_add_u64( &rn_message_payload_copies, 0 );
}
#endif

static int rocketnet_message_writer_put( struct rocketnet_message_writer_t * writer, const struct rocketnet_message_record_t * record, int copy )
{
    rocketnet_assert( writer );
    rocketnet_assert( rocketnet_message_record_valid( record ) );
    if ( !rocketnet_message_record_valid( record ) )
        return 0;

    const uint8_t flags = rocketnet_message_record_flags( record );
    const int header = rocketnet_message_flags_header_bytes( flags );
    if ( header + record->bytes > rocketnet_message_writer_space( writer ) )
        return 0;                                                   // never split: the record ends this packet (CHANNELS.md 9.3)
    if ( !copy && record->data != writer->payload + writer->bytes + header )
        return 0;                                                   // in place: the data starts where its header ends

    const int code = record->code;
    uint8_t * p = writer->payload + writer->bytes;
    rocketnet_write_uint8( &p, flags );
    rocketnet_write_uint16( &p, (uint16_t) record->bytes );
    if ( rocketnet_message_code_has_number( code ) )
        rocketnet_message_write_uint24( &p, record->number );
    if ( rocketnet_message_code_is_holdable( code ) || record->block )
        rocketnet_write_uint16( &p, (uint16_t) record->ring );
    if ( rocketnet_message_code_has_sequencing( code ) )
        rocketnet_message_write_uint24( &p, record->sequencing );
    if ( rocketnet_message_code_has_channel( code ) )
    {
        rocketnet_message_write_uint24( &p, record->ordering );
        if ( flags & ROCKETNET_MESSAGE_FLAG_WIDE )
            rocketnet_write_uint16( &p, (uint16_t) record->channel );
        else
            rocketnet_write_uint8( &p, (uint8_t) record->channel );
    }
    if ( record->block )
    {
        rocketnet_write_uint16( &p, (uint16_t) record->fragment );
        rocketnet_write_uint16( &p, (uint16_t) record->fragments );
    }
    rocketnet_assert( p - ( writer->payload + writer->bytes ) == header );
    if ( copy )
    {
#if defined(ROCKETNET_MESSAGE_COPY_TEST_HOOKS)
        rocketnet_platform_atomic_add_u64( &rn_message_payload_copies, 1 );
#endif
        rocketnet_write_bytes( &p, record->data, record->bytes );   // the one copy, store to frame (CHANNELS.md R5)
    }

    writer->bytes += header + record->bytes;
    writer->records++;
    return 1;
}

int rocketnet_message_writer_add( struct rocketnet_message_writer_t * writer, const struct rocketnet_message_record_t * record )
{
    return rocketnet_message_writer_put( writer, record, 1 );
}

int rocketnet_message_writer_add_in_place( struct rocketnet_message_writer_t * writer, const struct rocketnet_message_record_t * record )
{
    return rocketnet_message_writer_put( writer, record, 0 );
}

int rocketnet_message_writer_finish( struct rocketnet_message_writer_t * writer )
{
    rocketnet_assert( writer );
    if ( writer->records == 0 )
        return 0;                                                   // an empty plan writes nothing (CHANNELS.md 9.3)
    rocketnet_assert( writer->bytes + 1 <= writer->capacity );
    writer->payload[writer->bytes] = ROCKETNET_MESSAGE_FORM_RECORDS;
    return writer->bytes + 1;
}

// SINGLE (CHANNELS.md 5, 11.2): the form byte goes last, so the message is
// at payload offset 0, the transport's 16-aligned root, with nothing before it.

int rocketnet_message_single_capacity( int payload_capacity )
{
    return payload_capacity > 1 ? payload_capacity - 1 : 0;
}

int rocketnet_message_single_finish( uint8_t * payload, int payload_capacity, int message_bytes )
{
    rocketnet_assert( payload );
    if ( message_bytes < 1 || message_bytes > rocketnet_message_single_capacity( payload_capacity ) )
        return 0;
    payload[message_bytes] = ROCKETNET_MESSAGE_FORM_SINGLE;
    return message_bytes + 1;
}

// -----------------------------------------
//                                  reading
// -----------------------------------------

uint32_t rocketnet_message_enabled_codes( uint32_t enabled_reliabilities )
{
    // L2.1: the receipt variants travel as their base codes
    uint32_t codes = enabled_reliabilities & 0x1F;
    if ( enabled_reliabilities & ( 1u << 5 ) )
        codes |= 1u << ROCKETNET_MESSAGE_CODE_UNRELIABLE;
    if ( enabled_reliabilities & ( 1u << 6 ) )
        codes |= 1u << ROCKETNET_MESSAGE_CODE_RELIABLE;
    if ( enabled_reliabilities & ( 1u << 7 ) )
        codes |= 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    return codes;
}

static inline int rocketnet_message_round16( int bytes )
{
    return ( bytes + 15 ) & ~15;
}

// One record at body[offset], with remaining = body bytes from offset on.
// Runs checks 2 to 9 of STANDARD.md _Messages_ in order and stops at the
// first that fails. Reads only [body + offset, body + offset + remaining).

static int rocketnet_message_decode_record( const uint8_t * record_start, int remaining, int payload_bytes,
                                            const struct rocketnet_message_limits_t * limits,
                                            struct rocketnet_message_record_t * record, int * record_bytes )
{
    // check 2: the fixed part, then every field the flags name, inside the body

    if ( remaining < ROCKETNET_MESSAGE_RECORD_FIXED_BYTES )
        return ROCKETNET_MESSAGE_VIOLATION_TRUNCATED;
    const uint8_t flags = record_start[0];
    const int header = rocketnet_message_flags_header_bytes( flags );
    if ( header > remaining )
        return ROCKETNET_MESSAGE_VIOLATION_TRUNCATED;

    // check 3: the flags a correct sender writes

    if ( !rocketnet_message_flags_allowed( flags ) )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS;

    const int code = flags & ROCKETNET_MESSAGE_FLAG_CODE_MASK;
    const bool block = ( flags & ROCKETNET_MESSAGE_FLAG_BLOCK ) != 0;
    const bool wide = ( flags & ROCKETNET_MESSAGE_FLAG_WIDE ) != 0;
    const bool control = ( flags & ROCKETNET_MESSAGE_FLAG_CONTROL ) != 0;

    memset( record, 0, sizeof( *record ) );
    record->code = code;
    record->block = block ? 1 : 0;
    record->control = control ? 1 : 0;

    const uint8_t * p = record_start + 1;
    const int length = rocketnet_read_uint16( &p );
    if ( rocketnet_message_code_has_number( code ) )
        record->number = rocketnet_message_read_uint24( &p );
    if ( rocketnet_message_code_is_holdable( code ) || block )
        record->ring = rocketnet_read_uint16( &p );
    if ( rocketnet_message_code_has_sequencing( code ) )
        record->sequencing = rocketnet_message_read_uint24( &p );
    if ( rocketnet_message_code_has_channel( code ) )
    {
        record->ordering = rocketnet_message_read_uint24( &p );
        record->channel = wide ? rocketnet_read_uint16( &p ) : rocketnet_read_uint8( &p );
    }
    if ( block )
    {
        record->fragment = rocketnet_read_uint16( &p );
        record->fragments = rocketnet_read_uint16( &p );
    }

    // check 4: the data is at least one byte and inside the body

    if ( length == 0 || length > remaining - header )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH;

    // check 5: the channel is under this end's runtime count (I15), and WIDE
    // only where it is needed, so every record has one encoding

    if ( rocketnet_message_code_has_channel( code ) )
    {
        if ( (int) record->channel >= limits->num_channels )
            return ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL;
        if ( wide && record->channel < 256 )
            return ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL;
    }

    // check 6: the type is enabled (CHANNELS.md 7.5); CONTROL is exempt

    if ( !control && ( limits->enabled_codes & ( 1u << code ) ) == 0 )
        return ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED;

    // check 7, the part a record shows alone: a count of at least two, the
    // index inside it, k <= W, k * F <= max_message_bytes + F - 1, and every
    // fragment but the last exactly F bytes (CHANNELS.md 8). The agreement
    // with a live block descriptor is layer 5's.

    const int F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    if ( block )
    {
        const int k = (int) record->fragments + 1;
        const bool last = record->fragment == record->fragments;
        if ( record->fragments == 0 || record->fragment > record->fragments )
            return ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK;
        if ( k > limits->reliable_messages_in_flight || k * F > limits->max_message_bytes + F - 1 )
            return ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK;
        if ( last ? length > F : length != F )
            return ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK;
    }

    // check 8: a holdable record's span lies inside the ring. A block's span
    // is the whole block's, from its base (ring - fragment * F/16) to the end
    // of its last fragment; a fragment that is not the last knows that end
    // only to within the last fragment's length, at least 1 byte, so it
    // checks the shortest block it could belong to, and the last fragment
    // checks the exact end.

    if ( block )
    {
        const int base_units = (int) record->ring - (int) record->fragment * ( F / 16 );
        const bool last = record->fragment == record->fragments;
        const int span = (int) record->fragments * F + ( last ? rocketnet_message_round16( length ) : 16 );
        if ( base_units < 0 || base_units * 16 + span > limits->receive_message_buffer_bytes )
            return ROCKETNET_MESSAGE_VIOLATION_BAD_RING;
    }
    else if ( rocketnet_message_code_is_holdable( code ) )
    {
        if ( (int) record->ring * 16 + rocketnet_message_round16( length ) > limits->receive_message_buffer_bytes )
            return ROCKETNET_MESSAGE_VIOLATION_BAD_RING;
    }

    // check 9: reliable and CONTROL records ride only unfragmented payloads (L2.2, L2.4)

    if ( ( rocketnet_message_code_has_number( code ) || control ) && payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE;

    record->data = record_start + header;
    record->bytes = length;
    *record_bytes = header + length;
    return ROCKETNET_MESSAGE_VIOLATION_NONE;
}

// Check 1, then each record of the body in turn; the first failure refuses
// the whole payload (CHANNELS.md R6).

int rocketnet_message_check_payload( const uint8_t * payload, int bytes, const struct rocketnet_message_limits_t * limits )
{
    rocketnet_assert( limits );

    if ( bytes < 2 || !payload )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_FORM;
    const uint8_t form = payload[bytes - 1];
    if ( form != ROCKETNET_MESSAGE_FORM_SINGLE && form != ROCKETNET_MESSAGE_FORM_RECORDS )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_FORM;

    if ( form == ROCKETNET_MESSAGE_FORM_SINGLE )
    {
        // one UNRELIABLE message, any size the transport carried (L2.3)
        if ( ( limits->enabled_codes & ( 1u << ROCKETNET_MESSAGE_CODE_UNRELIABLE ) ) == 0 )
            return ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED;
        return ROCKETNET_MESSAGE_VIOLATION_NONE;
    }

    const int body_bytes = bytes - 1;
    int offset = 0;
    while ( offset < body_bytes )
    {
        struct rocketnet_message_record_t record;
        int record_bytes = 0;
        const int violation = rocketnet_message_decode_record( payload + offset, body_bytes - offset, bytes, limits, &record, &record_bytes );
        if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
            return violation;
        offset += record_bytes;
    }
    rocketnet_assert( offset == body_bytes );
    return ROCKETNET_MESSAGE_VIOLATION_NONE;
}

int rocketnet_message_reader_begin( struct rocketnet_message_reader_t * reader, const uint8_t * payload, int bytes, const struct rocketnet_message_limits_t * limits )
{
    rocketnet_assert( reader );
    rocketnet_assert( limits );
    reader->payload = payload;
    reader->payload_bytes = bytes;
    reader->offset = 0;
    reader->limits = limits;
    reader->violation = rocketnet_message_check_payload( payload, bytes, limits );
    reader->form = reader->violation == ROCKETNET_MESSAGE_VIOLATION_NONE ? payload[bytes - 1] : -1;
    return reader->violation;
}

int rocketnet_message_reader_next( struct rocketnet_message_reader_t * reader, struct rocketnet_message_record_t * record )
{
    rocketnet_assert( reader );
    rocketnet_assert( record );

    // a refused payload hands out nothing: a record is whole or absent
    if ( reader->violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return 0;

    const int body_bytes = reader->payload_bytes - 1;
    if ( reader->offset >= body_bytes )
        return 0;

    if ( reader->form == ROCKETNET_MESSAGE_FORM_SINGLE )
    {
        // zero copy: the message is the payload, from its root
        memset( record, 0, sizeof( *record ) );
        record->code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        record->data = reader->payload;
        record->bytes = body_bytes;
        reader->offset = body_bytes;
        return 1;
    }

    int record_bytes = 0;
    const int violation = rocketnet_message_decode_record( reader->payload + reader->offset, body_bytes - reader->offset,
                                                           reader->payload_bytes, reader->limits, record, &record_bytes );
    rocketnet_assert( violation == ROCKETNET_MESSAGE_VIOLATION_NONE );     // begin already checked every record
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        reader->violation = violation;
        return 0;
    }
    reader->offset += record_bytes;
    return 1;
}

// -----------------------------------------
//                           CONFIG control
// -----------------------------------------

// The CONFIG body (STANDARD.md _Messages_; CHANNELS.md 13.4):
// kind, version, enabled, zero, then five u32 and four zero bytes.

void rocketnet_message_write_config_body( uint8_t * output, const struct rocketnet_message_config_body_t * body )
{
    rocketnet_assert( output );
    rocketnet_assert( body );
    uint8_t * p = output;
    rocketnet_write_uint8( &p, ROCKETNET_MESSAGE_CONTROL_CONFIG );
    rocketnet_write_uint8( &p, (uint8_t) body->version );
    rocketnet_write_uint8( &p, (uint8_t) body->enabled );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint32( &p, body->num_channels );
    rocketnet_write_uint32( &p, body->reliable_messages_in_flight );
    rocketnet_write_uint32( &p, body->receive_message_buffer_bytes );
    rocketnet_write_uint32( &p, body->max_message_bytes );
    rocketnet_write_uint32( &p, body->fragment_bytes );
    rocketnet_write_uint32( &p, 0 );
    rocketnet_assert( p - output == ROCKETNET_MESSAGE_CONFIG_BYTES );
}

int rocketnet_message_read_config_body( const uint8_t * data, int bytes, struct rocketnet_message_config_body_t * body )
{
    rocketnet_assert( body );
    if ( !data || bytes < 1 || data[0] != ROCKETNET_MESSAGE_CONTROL_CONFIG )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL;            // kinds other than 1 are reserved
    if ( bytes != ROCKETNET_MESSAGE_CONFIG_BYTES )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL;

    const uint8_t * p = data + 1;
    body->version = rocketnet_read_uint8( &p );
    body->enabled = rocketnet_read_uint8( &p );
    const uint8_t zero = rocketnet_read_uint8( &p );
    body->num_channels = rocketnet_read_uint32( &p );
    body->reliable_messages_in_flight = rocketnet_read_uint32( &p );
    body->receive_message_buffer_bytes = rocketnet_read_uint32( &p );
    body->max_message_bytes = rocketnet_read_uint32( &p );
    body->fragment_bytes = rocketnet_read_uint32( &p );
    const uint32_t zero_tail = rocketnet_read_uint32( &p );

    // version 1's reserved bytes are zero; a version this end does not know
    // reads, and the comparison above it is a CONFIG mismatch
    if ( body->version == ROCKETNET_MESSAGE_CONFIG_VERSION && ( zero != 0 || zero_tail != 0 ) )
        return ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL;
    return ROCKETNET_MESSAGE_VIOLATION_NONE;
}
