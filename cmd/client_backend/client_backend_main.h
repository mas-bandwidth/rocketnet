/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#ifndef CLIENT_BACKEND_MAIN_H
#define CLIENT_BACKEND_MAIN_H

#include "client_backend.h"

struct main_t
{
    // void * curl;

    uint64_t start_time;
    uint32_t public_address;
    uint16_t port;
    bool initialized;
    bool shutting_down;
#ifdef __linux__
    int state_fd;
    int config_fd;
    int buyer_fd;
#endif // #ifdef __linux__
};

struct config_t;
struct bpf_t;

bool main_init( struct main_t * main, struct config_t * config, struct bpf_t * bpf );

int main_run( struct main_t * main );

void main_shutdown( struct main_t * main );

#endif // #ifndef CLIENT_BACKEND_MAIN_H
