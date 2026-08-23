/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Internal definition of rocketnet_address_t, moved from flow_address.h.

    The struct is OPAQUE at the public seam: rocketnet.h forward-declares it
    and exports rocketnet_address_bytes() so a caller in another language can
    allocate one without knowing the layout. Only this library sees the fields.

    Layout note: the layout is flow's portable union (bytes, not a sockaddr),
    which is the same on every platform. The public header's comment says the
    struct holds a sockaddr; the moved reality is this portable form. Recorded
    in the stage 1 report rather than silently editing the header.
*/

#ifndef ROCKETNET_ADDRESS_H
#define ROCKETNET_ADDRESS_H

#include "rocketnet.h"

#define ROCKETNET_ADDRESS_NONE                                    0
#define ROCKETNET_ADDRESS_IPV4                                    1
#define ROCKETNET_ADDRESS_IPV6                                    2

struct rocketnet_address_t
{
    union { uint32_t ip; uint8_t ipv4[4]; uint16_t ipv6[8]; } data;
    uint16_t port;
    uint8_t type;
};

#endif // #ifndef ROCKETNET_ADDRESS_H
