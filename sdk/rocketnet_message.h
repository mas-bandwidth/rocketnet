/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_MESSAGE_H
#define ROCKETNET_SDK_ROCKETNET_MESSAGE_H

#include <stdint.h>
#include <stddef.h>
#include "rocketnet_internal.h"

// layer 2 of the message layer (standard/MESSAGES.md m3): message records
// inside one transport payload, and the zero-copy single-message form. The
// wire is standard/MESSAGES.md m3, to the byte; this file is its codec and the
// receiver's checks 1 to 9 and 12, in the standard's order. It is pure: no
// state survives a call, nothing is allocated, and nothing reads a clock.
// Reliability, ordering and the window are the layers above (standard/MESSAGES.md m5, m7, m9); this layer hands them each record whole, with its fields as the wire
// carries them (24-bit numbers unreconstructed).
//
// The form byte is the payload's LAST byte, so a SINGLE message starts at
// payload offset 0 -- the transport's 16-aligned frame + payload_index -- and
// is written and read in place with no copy (standard/MESSAGES.md m3, m2).

#define ROCKETNET_MESSAGE_FORM_SINGLE                       0x00
#define ROCKETNET_MESSAGE_FORM_RECORDS                      0x01

// the record's wire code: the reliability as it travels (standard/MESSAGES.md m1.3).
// the receipt variants 5, 6 and 7 are sent as 0, 2 and 3; 5 to 7 on the wire
// are violations.

#define ROCKETNET_MESSAGE_CODE_UNRELIABLE                   0
#define ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED         1
#define ROCKETNET_MESSAGE_CODE_RELIABLE                     2
#define ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED             3
#define ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED           4
#define ROCKETNET_MESSAGE_NUM_CODES                         5

#define ROCKETNET_MESSAGE_FLAG_CODE_MASK                    0x07
#define ROCKETNET_MESSAGE_FLAG_BLOCK                        0x08
#define ROCKETNET_MESSAGE_FLAG_WIDE                         0x10
#define ROCKETNET_MESSAGE_FLAG_CONTROL                      0x20
#define ROCKETNET_MESSAGE_FLAG_RESERVED                     0xC0

// flags, then the u16 length: the fixed part of every record

#define ROCKETNET_MESSAGE_RECORD_FIXED_BYTES                3

// code 4 with BLOCK and WIDE: 3 + number 3 + ring 2 + sequencing 3 + ordering 3
// + channel 2 + fragment 2 + fragments 2

#define ROCKETNET_MESSAGE_RECORD_MAX_HEADER_BYTES           20

#define ROCKETNET_MESSAGE_MAX_RECORD_BYTES                  65535

// the 24-bit wire counters: number, sequencing and ordering (standard/MESSAGES.md m5).
// A difference of two counters at or above the half range is negative. Each
// direction's reliable numbers start at the first number.

#define ROCKETNET_MESSAGE_NUMBER_BITS                       24
#define ROCKETNET_MESSAGE_NUMBER_BYTES                      3
#define ROCKETNET_MESSAGE_NUMBER_RANGE                      ( 1u << ROCKETNET_MESSAGE_NUMBER_BITS )             // 16777216
#define ROCKETNET_MESSAGE_HALF_RANGE                        ( 1u << ( ROCKETNET_MESSAGE_NUMBER_BITS - 1 ) )     // 8388608
#define ROCKETNET_MESSAGE_U24_MASK                          ( ROCKETNET_MESSAGE_NUMBER_RANGE - 1 )              // 0xFFFFFF
#define ROCKETNET_MESSAGE_FIRST_NUMBER                      0

// the unit of a record's ring offset and of a ring span's length

#define ROCKETNET_MESSAGE_RING_UNIT                         16

// the lowest channel a record carries in the two-byte WIDE form

#define ROCKETNET_MESSAGE_WIDE_CHANNEL                      256

// a 24-bit wire value's signed half-range difference from a reference's low
// 24 bits, in -2^23 .. 2^23 - 1: an exact half lies behind the reference.
// The full value is reference + delta (proofs/cbmc/proof_reconstruct_sequence.c).

static inline int32_t rocketnet_message_u24_delta( uint32_t wire, uint64_t reference )
{
    const uint32_t unsigned_delta = ( ( wire & ROCKETNET_MESSAGE_U24_MASK ) + ROCKETNET_MESSAGE_NUMBER_RANGE - ( (uint32_t) reference & ROCKETNET_MESSAGE_U24_MASK ) ) & ROCKETNET_MESSAGE_U24_MASK;
    return unsigned_delta >= ROCKETNET_MESSAGE_HALF_RANGE ? (int32_t) unsigned_delta - (int32_t) ROCKETNET_MESSAGE_NUMBER_RANGE : (int32_t) unsigned_delta;
}

// the CONFIG control record (standard/MESSAGES.md m11)

#define ROCKETNET_MESSAGE_CONTROL_CONFIG                    1
#define ROCKETNET_MESSAGE_CONFIG_VERSION                    1
#define ROCKETNET_MESSAGE_CONFIG_BYTES                      28

// the receiver's violations, one per check of standard/MESSAGES.md m4, in the
// standard's order. A parse names exactly one, or none. The window
// (MESSAGE_NUMBER_OUTSIDE_WINDOW, MESSAGE_SLOT_COLLISION) and ordering
// (MESSAGE_ORDERING_BEHIND) checks belong to layers 3 and 4, and the
// block-descriptor agreement of check 7 to layer 5: they need state this
// layer does not own.

#define ROCKETNET_MESSAGE_VIOLATION_NONE                    0
#define ROCKETNET_MESSAGE_VIOLATION_BAD_FORM                1   // check 1
#define ROCKETNET_MESSAGE_VIOLATION_TRUNCATED               2   // check 2
#define ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS               3   // check 3
#define ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH              4   // check 4
#define ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL             5   // check 5
#define ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED        6   // check 6
#define ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK               7   // check 7, the stateless part
#define ROCKETNET_MESSAGE_VIOLATION_BAD_RING                8   // check 8
#define ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE                9   // check 9
#define ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL             10  // check 12
#define ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW   11  // check 10, layer 3
#define ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION          12  // check 10, layer 3
#define ROCKETNET_MESSAGE_NUM_VIOLATIONS                    13

#if ROCKETNET_DEV
const char * rocketnet_message_violation_name( int violation );
#endif // #if ROCKETNET_DEV

// one record as the wire carries it. Fields the record's code does not carry
// are zero, so a parsed record packs back to the same bytes and a record
// packs only one way. WIDE is not a field: the writer sets it exactly when
// channel >= 256, and the reader refuses it otherwise.

struct rocketnet_message_record_t
{
    int code;                       // ROCKETNET_MESSAGE_CODE_*
    int block;                      // nonzero: one fragment of a block
    int control;                    // nonzero: a layer control record (code 2 only)
    uint32_t number;                // u24, codes 2, 3, 4
    uint32_t ring;                  // u16, in 16-byte units: codes 3, 4, or a block
    uint32_t sequencing;            // u24, codes 1, 4
    uint32_t ordering;              // u24, codes 1, 3, 4
    uint32_t channel;               // codes 1, 3, 4: 0 to 65535
    uint32_t fragment;              // u16, a block: this fragment's index
    uint32_t fragments;             // u16, a block: fragment count - 1
    const uint8_t * data;           // into the payload on parse; copied from on write
    int bytes;                      // 1 to 65535
};

// the header bytes the record's fields take on the wire (3 to 20)

int rocketnet_message_record_header_bytes( const struct rocketnet_message_record_t * record );

// nonzero when the record is one a correct sender may write, before any
// receiver's limits: a code 0 to 4, flags the standard allows, absent fields
// zero, each present field inside its wire width, 1 to 65535 data bytes, and
// a block's index inside its count.

int rocketnet_message_record_valid( const struct rocketnet_message_record_t * record );

// -----------------------------------------
//                                  writing
// -----------------------------------------

// a RECORDS payload, built in place in the caller's buffer (a transport frame
// at payload_index). capacity counts every byte the payload may take,
// including the trailing form byte. add copies the record's data after its
// header: the one copy from the send store into the frame.

struct rocketnet_message_writer_t
{
    uint8_t * payload;
    int capacity;
    int bytes;                      // body bytes written so far
    int records;
};

void rocketnet_message_writer_begin( struct rocketnet_message_writer_t * writer, uint8_t * payload, int capacity );

// the space left for one more record's header and data

int rocketnet_message_writer_space( const struct rocketnet_message_writer_t * writer );

// appends one valid record. Returns 1, or 0 when it does not fit, leaving the
// payload unchanged: a record is never split (standard/MESSAGES.md m2).

int rocketnet_message_writer_add( struct rocketnet_message_writer_t * writer, const struct rocketnet_message_record_t * record );

// appends one valid record whose data is already in place, right after the
// header's room (record->data == payload + bytes so far + its header bytes):
// only the header is written, and nothing is copied. The in-place unreliable
// send (docs/OUTGOING-BUILD.md) writes its one record this way. Returns 1, or
// 0 when it does not fit or the data is not where its header ends.

int rocketnet_message_writer_add_in_place( struct rocketnet_message_writer_t * writer, const struct rocketnet_message_record_t * record );

#if ROCKETNET_DEV
// test builds only (the unit suite's library, never the release library): the
// payload copies writer_add has made, from a send store into a frame -- the
// message-to-packet copy a fitting unreliable send never makes.
uint64_t rn_message_payload_copy_count();
#endif // #if ROCKETNET_DEV

// writes the RECORDS form byte. Returns the payload's length L, or 0 when no
// record was added (an empty plan writes nothing).

int rocketnet_message_writer_finish( struct rocketnet_message_writer_t * writer );

// the SINGLE form, zero copy. The caller writes the message at payload offset
// 0 -- frame + payload_index, 16-aligned -- in place, then finish writes the
// one form byte after it. single_capacity is the message room in a payload of
// payload_capacity bytes. finish returns the payload's length L, or 0 when the
// message is empty or does not leave room for the form byte.

int rocketnet_message_single_capacity( int payload_capacity );

int rocketnet_message_single_finish( uint8_t * payload, int payload_capacity, int message_bytes );

// -----------------------------------------
//                                  reading
// -----------------------------------------

// the receiver's own bounds, which checks 5 to 8 compare against: its
// runtime channel count (never the compile-time maximum), the wire codes its
// enabled mask admits, and its W, max_message_bytes and B.

struct rocketnet_message_limits_t
{
    int num_channels;               // 1 to 65536
    uint32_t enabled_codes;         // bit c set: wire code c is accepted on an application record
    int reliable_messages_in_flight;            // W
    int max_message_bytes;
    int receive_message_buffer_bytes;         // B, a multiple of 16
};

// the wire codes a mask of enabled reliabilities (bit r = reliability r, the
// CONFIG enabled byte) admits: 5 travels as 0, 6 as 2, 7 as 3.

uint32_t rocketnet_message_enabled_codes( uint32_t enabled_reliabilities );

// a reader over one payload. begin runs every check of this layer over the
// whole payload, in order, and stops at the first failure: the payload is
// then refused whole, and no record of it is ever handed out. On success,
// next hands the records out one at a time, in wire order, each whole, with
// data pointing into the payload (for SINGLE, data is the payload itself).
// Nothing is read outside [payload, payload + bytes).

struct rocketnet_message_reader_t
{
    const uint8_t * payload;
    int payload_bytes;              // L
    int form;
    int offset;                     // the next record's offset in the body
    int violation;
    const struct rocketnet_message_limits_t * limits;
};

// returns ROCKETNET_MESSAGE_VIOLATION_NONE, or the one violation the payload
// hit (also left in reader->violation).

int rocketnet_message_reader_begin( struct rocketnet_message_reader_t * reader, const uint8_t * payload, int bytes, const struct rocketnet_message_limits_t * limits );

// returns 1 with the next record, or 0 when the payload is done or was refused

int rocketnet_message_reader_next( struct rocketnet_message_reader_t * reader, struct rocketnet_message_record_t * record );

// the whole-payload check alone: the violation begin would return

int rocketnet_message_check_payload( const uint8_t * payload, int bytes, const struct rocketnet_message_limits_t * limits );

// -----------------------------------------
//                           CONFIG control
// -----------------------------------------

// the CONFIG body (standard/MESSAGES.md m11). The record carrying it is code 2
// with CONTROL set and its first reliable number; the layers above number and
// compare it. read refuses an unknown kind, a wrong length, or a version 1
// body with nonzero reserved bytes (MESSAGE_BAD_CONTROL); a version it does
// not know reads, and the comparison above makes it a CONFIG mismatch.

struct rocketnet_message_config_body_t
{
    uint32_t version;
    uint32_t enabled;               // bit r set: reliability r may be sent by the application
    uint32_t num_channels;
    uint32_t reliable_messages_in_flight;
    uint32_t receive_message_buffer_bytes;
    uint32_t max_message_bytes;
    uint32_t fragment_bytes;
};

void rocketnet_message_write_config_body( uint8_t * output, const struct rocketnet_message_config_body_t * body );

int rocketnet_message_read_config_body( const uint8_t * data, int bytes, struct rocketnet_message_config_body_t * body );

#endif // ROCKETNET_SDK_ROCKETNET_MESSAGE_H
