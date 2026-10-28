/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the message reader and the config body reader.

    The reader is sdk/rocketnet_message.cpp (rocketnet_message_reader_begin,
    rocketnet_message_reader_next, rocketnet_message_read_config_body),
    included below. Payloads are 24 symbolic bytes, which is enough for a
    block header and a holdable record. Limits are symbolic inside the
    ranges below. A refused payload yields no record. An accepted record's
    bytes lie inside the payload and stop before the trailing form byte,
    so nothing is read past the given length. A holdable record's span,
    and a block's span, lie inside receive_message_buffer_bytes. The config reader
    refuses a null buffer, an empty buffer, and a 4-byte buffer, and a
    28-byte buffer stays inside that object.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 8. A successful record is at least 4 bytes, so a 23-byte
    body holds at most 5 records; the following truncated byte is one
    more iteration.
*/

#include "rocketnet_message.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned nondet_uint( void ) { return 0; }
#else
int nondet_int( void );
unsigned nondet_uint( void );
#endif // #ifndef __CPROVER__

#define BOUND 24

static int round16( int bytes )
{
    return ( bytes + 15 ) & ~15;
}

static void fill_limits( struct rocketnet_message_limits_t * limits )
{
    limits->num_channels = nondet_int();
    __CPROVER_assume( limits->num_channels >= 1 && limits->num_channels <= 65536 );
    limits->enabled_codes = nondet_uint();
    limits->reliable_messages_in_flight = nondet_int();
    __CPROVER_assume( limits->reliable_messages_in_flight >= 1 && limits->reliable_messages_in_flight <= 4096 );
    limits->max_message_bytes = nondet_int();
    __CPROVER_assume( limits->max_message_bytes >= 1 && limits->max_message_bytes <= 65536 );
    limits->receive_message_buffer_bytes = nondet_int();
    __CPROVER_assume( limits->receive_message_buffer_bytes >= 0 && limits->receive_message_buffer_bytes <= 65536 );
}

static void harness_reader( void )
{
    uint8_t payload[BOUND];
    struct rocketnet_message_limits_t limits;
    fill_limits( &limits );

    struct rocketnet_message_reader_t reader;
    int violation = rocketnet_message_reader_begin( &reader, payload, BOUND, &limits );
    struct rocketnet_message_record_t record;

    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        __CPROVER_assert( rocketnet_message_reader_next( &reader, &record ) == 0,
                          "refused payload yields no record" );
        return;
    }

    const uint8_t * end = payload + BOUND - 1;
    int yielded = 0;

    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        __CPROVER_assert( record.data >= payload, "record data starts inside the payload" );
        __CPROVER_assert( record.bytes >= 1, "record length is at least one" );
        __CPROVER_assert( record.data + record.bytes <= end, "record does not read past the given length" );

        if ( record.block )
        {
            const int F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
            const int base = (int) record.ring - (int) record.fragment * ( F / 16 );
            const int last = record.fragment == record.fragments;
            const int span = (int) record.fragments * F + ( last ? round16( record.bytes ) : 16 );
            __CPROVER_assert( base >= 0, "block base is inside the ring" );
            __CPROVER_assert( (long long) base * 16 + span <= limits.receive_message_buffer_bytes,
                              "block span fits the ring" );
        }
        else if ( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED || record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        {
            __CPROVER_assert( (long long) record.ring * 16 + round16( record.bytes ) <= limits.receive_message_buffer_bytes,
                              "holdable span fits the ring" );
        }

        yielded++;
        __CPROVER_assert( yielded <= BOUND, "reader advances through the payload" );
    }
}

static void harness_short_and_config( void )
{
    struct rocketnet_message_limits_t limits;
    fill_limits( &limits );

    uint8_t payload[BOUND];
    struct rocketnet_message_reader_t reader;
    __CPROVER_assert( rocketnet_message_reader_begin( &reader, payload, 0, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_FORM,
                      "empty payload is a bad form" );
    __CPROVER_assert( rocketnet_message_reader_begin( &reader, payload, 1, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_FORM,
                      "one-byte payload is a bad form" );
    __CPROVER_assert( rocketnet_message_reader_begin( &reader, NULL, BOUND, &limits ) == ROCKETNET_MESSAGE_VIOLATION_BAD_FORM,
                      "null payload is a bad form" );

    struct rocketnet_message_config_body_t body;
    uint8_t config[ROCKETNET_MESSAGE_CONFIG_BYTES];
    uint8_t short_config[4];
    (void) rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES, &body );
    __CPROVER_assert( rocketnet_message_read_config_body( NULL, ROCKETNET_MESSAGE_CONFIG_BYTES, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL,
                      "null config is refused" );
    __CPROVER_assert( rocketnet_message_read_config_body( config, 0, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL,
                      "empty config is refused" );
    __CPROVER_assert( rocketnet_message_read_config_body( short_config, 4, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL,
                      "short config is refused" );
}

int main( void )
{
    harness_reader();
    harness_short_and_config();
#if defined( REACHABILITY_CONTROL )
    __CPROVER_assert( 0, "message-read reachability control" );
#endif // #if defined( REACHABILITY_CONTROL )
    return 0;
}
