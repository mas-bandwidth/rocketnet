/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_PROTOCOL_H
#define ROCKETNET_PROTOCOL_H

#include <stdint.h>

// The connection protocol wire layer, per STANDARD.md. This module grows with
// the standard's own build order: the prefilter header, then packet layouts,
// sealing, and the reading order.

// packet types

#define ROCKETNET_CONNECTION_REQUEST_PACKET     0
#define ROCKETNET_CONNECTION_DENIED_PACKET      1
#define ROCKETNET_CONNECTION_CHALLENGE_PACKET   2
#define ROCKETNET_CONNECTION_RESPONSE_PACKET    3
#define ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET  4
#define ROCKETNET_CONNECTION_PAYLOAD_PACKET     5
#define ROCKETNET_CONNECTION_DISCONNECT_PACKET  6
#define ROCKETNET_CONNECTION_COOKIE_PACKET      7

// types 8 through 255 are reserved and ignored by a conforming receiver

// the prefilter header: 4 bytes on the front of every packet. the first byte
// is the packet type and carries the type and nothing else; the other three
// are the per-title tag, an arbitrary constant agreed by value.

#define ROCKETNET_PREFILTER_BYTES               4
#define ROCKETNET_TITLE_TAG_BYTES               3

// the published test title tag, embedded so development works out of the box.
// production titles get their own tag from the per-title generator.

#define ROCKETNET_TEST_TITLE_TAG                { 0xb7, 0x59, 0x01 }

// the receive ceiling: fixed for every receiver, whatever its send budget

#define ROCKETNET_RECEIVE_CEILING               1400

void rocketnet_write_prefilter_header( uint8_t ** p, uint8_t packet_type, const uint8_t * title_tag );

// The prefilter structural check from the standard's reading order: the
// packet is at least a prefilter header, no larger than the receive ceiling,
// and carries this title's tag. Returns the packet type on success, -1 on
// refusal. Deliberately knows nothing about types or per-type sizes; those
// are later steps of the reading order.

int rocketnet_prefilter_check( const uint8_t * packet_data, int packet_bytes, const uint8_t * title_tag );

#endif // #ifndef ROCKETNET_PROTOCOL_H
