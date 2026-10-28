/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_PACKET_BATCH_H
#define ROCKETNET_SDK_ROCKETNET_PACKET_BATCH_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"

// Shared receive view. The historic type name is retained for native transport
// source compatibility; this layout is independent of the XDP implementation.
struct rocketnet_server_socket_process_packets_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
    int packet_bytes[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
    uint8_t * packet_data[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE];
};

#endif // ROCKETNET_SDK_ROCKETNET_PACKET_BATCH_H
