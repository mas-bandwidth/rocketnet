/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The published development backend public key (STANDARD.md), as data for
    the examples, which set it into the server config by hand after
    rocketnet_server_default_config, with the example's own protocol id. It is
    public: a release library refuses it at server start, so a release server
    never accepts the development tokens rocketnet_create_dev_connect_token
    makes. A real server brings its own backend's key, recommended through
    ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code.
*/

#ifndef ROCKETNET_EXAMPLES_DEV_KEYS_H
#define ROCKETNET_EXAMPLES_DEV_KEYS_H

#include <stdint.h>

/// The examples' protocol id: any nonzero id the client's tokens and the
/// server's config share.
#define EXAMPLE_PROTOCOL_ID 0x52434b544e455431ULL

static const uint8_t example_dev_backend_public_key[32] =
{
    0xb0, 0xab, 0x03, 0xb3, 0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99,
    0x29, 0xa5, 0xb4, 0xbf, 0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d,
    0x08, 0x4a, 0x08, 0xd8, 0x58, 0xbd, 0x24, 0x2c
};

#endif // #ifndef ROCKETNET_EXAMPLES_DEV_KEYS_H
