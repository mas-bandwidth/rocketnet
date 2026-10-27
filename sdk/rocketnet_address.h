/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_ADDRESS_H
#define ROCKETNET_SDK_ROCKETNET_ADDRESS_H

#include "rocketnet_internal.h"

#include <stdint.h>

// struct rocketnet_address_t is the SDK's own (rocketnet_layers.h); the
// public API takes and gives addresses only as strings.

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Load an IPv4 endpoint from network-order integers.
 * @param address [out] Non-NULL destination; previous contents are replaced.
 * @param big_endian_ipv4 [in] Four IPv4 octets in network byte order.
 * @param big_endian_port [in] Port in network byte order, 0 through 65535.
 * @return Nothing.
 */
void rocketnet_address_load_ipv4( struct rocketnet_address_t * address, uint32_t big_endian_ipv4, uint16_t big_endian_port );

/** @brief Parse a numeric IPv4 or IPv6 address and optional port.
 * @param address [out] Non-NULL destination; it is cleared before parsing.
 * @param address_string [in] Non-NULL, NUL-terminated numeric address shorter than ROCKETNET_MAX_ADDRESS_STRING_LENGTH. Accepts dotted IPv4 with optional :port, bare IPv6 without a port, or bracketed IPv6 with optional :port; decimal ports range from 0 through 65535.
 * @return True when parsing succeeds; false for empty, overlong, malformed, or unsupported text, which leaves address cleared. A NULL argument asserts; with assertions disabled, a NULL address returns false and writes nothing, and a NULL address_string clears address and returns false.
 */
bool rocketnet_address_parse( struct rocketnet_address_t * address, const char * address_string );

// both of the following write into the caller's buffer bounded by
// ROCKETNET_MAX_ADDRESS_STRING_LENGTH: the buffer must be at least that many
// bytes, terminator included. the constant arrives with rocketnet.h, above.

/** @brief Format an address, including a nonzero port, into a caller buffer.
 * @param address [in] Non-NULL address to format.
 * @param buffer [out] Non-NULL buffer of at least ROCKETNET_MAX_ADDRESS_STRING_LENGTH bytes, including the terminator.
 * @return The same buffer, containing the formatted address; IPv6 with a port is bracketed. Unsupported address types produce NONE. On legacy Windows builds without IPv6 support, IPv6 formats as an empty string. The buffer remains caller-owned.
 */
const char * rocketnet_address_to_string( const struct rocketnet_address_t * address, char * buffer );

/** @brief Format only the numeric host address into a caller buffer.
 * @param address [in] Non-NULL address to format.
 * @param buffer [out] Non-NULL buffer of at least ROCKETNET_MAX_ADDRESS_STRING_LENGTH bytes, including the terminator.
 * @return The same buffer, containing the host without a port; unsupported address types produce NONE. On legacy Windows builds without IPv6 support, IPv6 formats as an empty string. The buffer remains caller-owned.
 */
const char * rocketnet_address_to_string_without_port( const struct rocketnet_address_t * address, char * buffer );

/** @brief Compare two addresses including their ports.
 * @param a [in] Non-NULL first address.
 * @param b [in] Non-NULL second address.
 * @return True when both have the same supported family, address bytes, and port; false otherwise.
 */
bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b );

/** @brief Clear the port and host bits used to identify an endpoint.
 * @param address [in,out] Non-NULL address to anonymize. IPv4 keeps its first three octets; mapped IPv6 keeps its first three IPv4 octets; all other types clear the final four 16-bit groups.
 * @return Nothing.
 */
void rocketnet_address_anonymize( struct rocketnet_address_t * address );

/** @brief Check whether an address is IPv4-mapped IPv6.
 * @param address [in] Non-NULL address to inspect.
 * @return True for ::ffff:a.b.c.d; false for other families or prefixes.
 */
bool rocketnet_address_is_ipv4_in_ipv6( struct rocketnet_address_t * address );

/** @brief Convert an IPv4-mapped IPv6 address to native IPv4 form.
 * @param address [in,out] Non-NULL mapped IPv6 address; the precondition is checked by an assertion. The port is preserved.
 * @return Nothing.
 */
void rocketnet_address_convert_ipv6_to_ipv4( struct rocketnet_address_t * address );

/** @brief Convert an IPv4 address to its IPv4-mapped IPv6 form.
 * @param address [in,out] Non-NULL IPv4 address; its octets and port are preserved.
 * @return Nothing.
 */
void rocketnet_address_convert_ipv4_to_ipv6( struct rocketnet_address_t * address );

/** @brief Return the four stored IPv4 octets as an integer.
 * @param address [in] Non-NULL address. The implementation reads the IPv4 byte array without checking type.
 * @return The four address bytes copied into a uint32_t, preserving the byte representation.
 */
uint32_t rocketnet_address_ipv4( struct rocketnet_address_t * address );

#ifdef __cplusplus
}
#endif

// the one rule for an ipv4-mapped source (::ffff:a.b.c.d) on receive.
//
// on a dual stack socket (IPV6_V6ONLY off) the kernel delivers every ipv4
// client as a mapped address: it is accepted and canonicalised to native
// ipv4 here, at the edge, so connection keys, rate limits, cookies and token
// binding all see one address per client.
//
// anywhere the real ipv6 header is seen -- a raw or xdp ipv6 path, or an
// ipv6-only socket -- a mapped source never occurs legitimately (RFC 4291
// 2.5.5.2): it is spoofed, and dropped.
//
// returns false when the source must be dropped. any other source passes
// through unchanged.

/** @brief Accept a source and canonicalize mapped IPv4 on dual-stack input.
 * @param from [in,out] Non-NULL source address. A mapped source becomes native IPv4 when dual_stack is true.
 * @param dual_stack [in] True when the receiving socket accepts IPv4-mapped IPv6 sources.
 * @return False only for a mapped address received when dual_stack is false; other addresses pass unchanged.
 */
static inline bool rocketnet_address_accept_received_source( struct rocketnet_address_t * from, bool dual_stack )
{
    if ( !rocketnet_address_is_ipv4_in_ipv6( from ) )
        return true;
    if ( !dual_stack )
        return false;
    rocketnet_address_convert_ipv6_to_ipv4( from );
    return true;
}

#endif // ROCKETNET_SDK_ROCKETNET_ADDRESS_H
