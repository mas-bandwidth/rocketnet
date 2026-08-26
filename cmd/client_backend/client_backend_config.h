/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#ifndef CLIENT_BACKEND_CONFIG_H
#define CLIENT_BACKEND_CONFIG_H

#include "client_backend.h"
#include "client_backend_shared.h"

struct config_t
{
    uint32_t public_address;
    uint16_t port;
    uint8_t client_backend_private_key[SIGN_PRIVATE_KEY_BYTES];
};

bool read_config( struct config_t * config );

#endif // #ifndef CLIENT_BACKEND_CONFIG_H
