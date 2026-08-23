/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Internal platform layer. Moved from flow's flow_platform.h, taking only
    what stage 1 needs: the monotonic clock behind rocketnet_time, sleep,
    getenv, and the byte-order and address-presentation helpers the address
    code calls. Sockets, threads and mutexes stay in flow until the stage
    that moves them.

    Not public API. These carry C++ linkage and are internal to the library.
*/

#ifndef ROCKETNET_PLATFORM_H
#define ROCKETNET_PLATFORM_H

#include "rocketnet.h"

bool rocketnet_platform_init();

void rocketnet_platform_term();

// Seconds since rocketnet_platform_init, from the platform's monotonic clock.
double rocketnet_platform_time();

void rocketnet_platform_sleep( double time );

const char * rocketnet_platform_getenv( const char * var );

uint16_t rocketnet_platform_ntohs( uint16_t in );

uint16_t rocketnet_platform_htons( uint16_t in );

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out );

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out );

// True on success. NOTE: flow's Linux implementation returned inet_ntop(...) == NULL,
// inverting the result on exactly one platform (mac and windows both had != NULL).
// Latent there because every caller ignored the return value. Fixed on arrival;
// recorded in the stage 1 report.
bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size );

#endif // #ifndef ROCKETNET_PLATFORM_H
