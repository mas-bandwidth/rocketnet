/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The examples' protocol id. The development backend public key the examples
    set into the server config comes from rocketnet.h itself,
    ROCKETNET_DEV_BACKEND_PUBLIC_KEY (development builds): it is public, and a
    release library refuses it at server start, so a release server never
    accepts the development tokens rocketnet_create_dev_connect_token makes. A
    real server brings its own backend's key, recommended through
    ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code.
*/

#ifndef ROCKETNET_EXAMPLES_DEV_KEYS_H
#define ROCKETNET_EXAMPLES_DEV_KEYS_H

/// The examples' protocol id: any nonzero id the client's tokens and the
/// server's config share.
#define EXAMPLE_PROTOCOL_ID 0x52434b544e455431ULL

// The backend examples' non-secret title filter, distinct from the SDK's
// development filter. It must match at both endpoints in either build mode.
#define EXAMPLE_BACKEND_PACKET_FILTER { 0x52, 0x4e, 0x42, 0x45 }

#endif // #ifndef ROCKETNET_EXAMPLES_DEV_KEYS_H
