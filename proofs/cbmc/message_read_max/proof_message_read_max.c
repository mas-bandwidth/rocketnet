/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Bounded model checking harnesses for the message reader and the config
    body reader on the real SDK implementation (sdk/rocketnet_message.cpp,
    included directly without copying).

    Harnesses:
    - harness_message_reader: rocketnet_message_reader_begin and
      rocketnet_message_reader_next on payloads of symbolic length in
      [0, PAYLOAD_BOUND] (default 24, the largest arbitrary multi-record
      bound that finishes within the nightly budget; override with
      -DPAYLOAD_BOUND=1439 for the full ROCKETNET_RECEIVE_PAYLOAD_BYTES
      bound). The payload pointer is either NULL or tail-aligned to the end
      of a stack buffer of size PAYLOAD_BOUND so any read 1 byte past the
      symbolic length bytes is an out-of-bounds object violation.
    - harness_message_reader_single_max: rocketnet_message_reader_begin and
      rocketnet_message_reader_next on payloads of symbolic length in
      [0, ROCKETNET_RECEIVE_PAYLOAD_BYTES] (1439) for non-RECORDS
      payloads (single-message form, invalid form bytes, short and null
      inputs) at the full 1439-byte bound.
    - harness_message_read_config_body: rocketnet_message_read_config_body
      on buffers of symbolic length in [0, ROCKETNET_RECEIVE_PAYLOAD_BYTES]
      (1439), tail-aligned to the end of the stack object so any read past
      bytes fails bounds checking.
*/

#if defined( ROCKETNET_ESBMC ) || defined( __ESBMC__ )
#include "../esbmc_shim.h"
#endif // #if defined( ROCKETNET_ESBMC ) || defined( __ESBMC__ )

#include <stdlib.h>
#include <string.h>
#include "rocketnet_message.h"
#include "rocketnet_read_write.h"

static inline void * proof_memset_record( struct rocketnet_message_record_t * r, int v, size_t n )
{
    (void) v;
    (void) n;

    struct rocketnet_message_record_t zero = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, 0 };
    *r = zero;
    return r;
}
#define memset proof_memset_record
#include "rocketnet_message.cpp"
#undef memset

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned int nondet_uint( void ) { return 0; }
#elif !defined( ROCKETNET_ESBMC ) && !defined( __ESBMC__ )
int nondet_int( void );
unsigned int nondet_uint( void );
#endif // #ifndef __CPROVER__

#ifndef PAYLOAD_BOUND
#define PAYLOAD_BOUND 24
#endif // #ifndef PAYLOAD_BOUND

#ifndef CONFIG_BOUND
#define CONFIG_BOUND ROCKETNET_RECEIVE_PAYLOAD_BYTES
#endif // #ifndef CONFIG_BOUND

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

void harness_message_reader( void )
{
    struct rocketnet_message_limits_t limits;
    fill_limits( &limits );

    int bytes = nondet_int();
    __CPROVER_assume( bytes >= 0 && bytes <= PAYLOAD_BOUND );

    uint8_t buf[PAYLOAD_BOUND];
    const uint8_t * payload = nondet_int() ? ( buf + ( PAYLOAD_BOUND - bytes ) ) : NULL;

    struct rocketnet_message_reader_t reader;
    int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &limits );
    struct rocketnet_message_record_t record;

    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        __CPROVER_assert( rocketnet_message_reader_next( &reader, &record ) == 0,
                          "refused payload yields no record" );
        return;
    }

    __CPROVER_assert( payload != NULL && bytes >= 2,
                      "accepted payload has at least two bytes" );

    const uint8_t * end = payload + bytes - 1;
    int yielded = 0;

    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        __CPROVER_assert( record.data >= payload, "record data starts inside the payload" );
        __CPROVER_assert( record.bytes >= 1, "record length is at least one" );
        __CPROVER_assert( record.data + record.bytes <= end, "record does not read past the given length" );
        __CPROVER_assert( __CPROVER_r_ok( record.data, (size_t) record.bytes ),
                          "record slice is readable within the payload object" );

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
        __CPROVER_assert( yielded <= bytes, "reader advances through the payload" );
    }
}

void harness_message_reader_single_max( void )
{
    struct rocketnet_message_limits_t limits;
    fill_limits( &limits );

    int bytes = nondet_int();
    __CPROVER_assume( bytes >= 0 && bytes <= CONFIG_BOUND );

    uint8_t buf[CONFIG_BOUND];
    const uint8_t * payload = nondet_int() ? ( buf + ( CONFIG_BOUND - bytes ) ) : NULL;
    if ( payload != NULL && bytes >= 2 )
    {
        __CPROVER_assume( payload[bytes - 1] != ROCKETNET_MESSAGE_FORM_RECORDS );
    }

    struct rocketnet_message_reader_t reader;
    int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &limits );
    struct rocketnet_message_record_t record;

    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        __CPROVER_assert( rocketnet_message_reader_next( &reader, &record ) == 0,
                          "refused payload yields no record" );
        return;
    }

    __CPROVER_assert( payload != NULL && bytes >= 2,
                      "accepted payload has at least two bytes" );
    __CPROVER_assert( rocketnet_message_reader_next( &reader, &record ) == 1,
                      "single-message payload yields one record" );
    __CPROVER_assert( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE,
                      "single-message payload code is unreliable" );
    __CPROVER_assert( record.data == payload && record.bytes == bytes - 1,
                      "single-message record spans payload body" );
    __CPROVER_assert( __CPROVER_r_ok( record.data, (size_t) record.bytes ),
                      "single-message record slice is readable within the payload object" );
    __CPROVER_assert( rocketnet_message_reader_next( &reader, &record ) == 0,
                      "single-message payload yields at most one record" );
}

void harness_message_read_config_body( void )
{
    int bytes = nondet_int();
    __CPROVER_assume( bytes >= 0 && bytes <= CONFIG_BOUND );

    uint8_t buf[CONFIG_BOUND];
    const uint8_t * data = nondet_int() ? ( buf + ( CONFIG_BOUND - bytes ) ) : NULL;

    struct rocketnet_message_config_body_t body;
    int violation = rocketnet_message_read_config_body( data, bytes, &body );

    if ( data == NULL || bytes != ROCKETNET_MESSAGE_CONFIG_BYTES || data[0] != ROCKETNET_MESSAGE_CONTROL_CONFIG )
    {
        __CPROVER_assert( violation == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL,
                          "invalid config length, null buffer, or unknown control kind is refused" );
        return;
    }

    if ( violation == ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        __CPROVER_assert( body.version == data[1], "config version matches wire byte 1" );
        __CPROVER_assert( body.enabled == data[2], "config enabled mask matches wire byte 2" );
        if ( body.version == ROCKETNET_MESSAGE_CONFIG_VERSION )
        {
            __CPROVER_assert( data[3] == 0, "version 1 reserved byte 3 is zero" );
            __CPROVER_assert( data[24] == 0 && data[25] == 0 && data[26] == 0 && data[27] == 0,
                              "version 1 reserved tail is zero" );
        }
    }
}

int main( void )
{
    harness_message_reader();
    harness_message_reader_single_max();
    harness_message_read_config_body();
    return 0;
}
