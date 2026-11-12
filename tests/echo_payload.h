/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    What the test server's raw payload echo (tests/client_server/server.c)
    may send back. The echo returns a client's packet payload unchanged, as
    if the server had sent it. That is sound for a payload the server could
    have sent byte for byte: raw bytes that are not message records, the
    unreliable records (codes 0 and 1, one record each, no block) and the
    layer's control records. It is not sound for a numbered application
    message: an unreliable message over 1,411 bytes is promoted to its
    reliable twin, and an echo of it hands the client its own send numbers as
    the server's, which the client refuses as a protocol violation once they
    run past its receive window (docs/XDP-PACKET-BLASTER.md). Such a payload
    is refused by name instead of echoed; the echo measures the transport
    echo of unreliable records, nothing more.

    No allocation, no syscall, no global state. C and C++.
*/

#pragma once

#ifndef ROCKETNET_TESTS_ECHO_PAYLOAD_H
#define ROCKETNET_TESTS_ECHO_PAYLOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// NULL when the payload may be echoed; otherwise what makes it a payload
// only its sender's peer could answer, for the server to print
// (tests/echo_payload.cpp).

const char * rocketnet_echo_payload_refusal( const uint8_t * payload, int bytes );

#ifdef __cplusplus
}
#endif

#endif // ROCKETNET_TESTS_ECHO_PAYLOAD_H
