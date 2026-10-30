/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_WINDOW_RECEIVE_H
#define ROCKETNET_SDK_ROCKETNET_WINDOW_RECEIVE_H

#include "rocketnet_allocator.h"
#include "rocketnet_message.h"

// standard/MESSAGES.md m5, m5.2: one pure receiver state over the connection's region.
struct rn_window_receive_t
{
    rn_region_t * region;
    uint64_t R;
    int violation;
};

enum
{
    RN_WINDOW_RECEIVE_ACCEPT,
    RN_WINDOW_RECEIVE_DUPLICATE,
    RN_WINDOW_RECEIVE_VIOLATION
};

void rn_window_receive_init( rn_window_receive_t * receive, rn_region_t * region, uint64_t first_number );
int rn_window_receive_accept( rn_window_receive_t * receive, const rocketnet_message_record_t * record, uint32_t * slot );
#if ROCKETNET_DEV
void rn_window_receive_release( rn_window_receive_t * receive, uint64_t number );
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_WINDOW_RECEIVE_H
