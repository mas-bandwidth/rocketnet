/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once
#include "rocketnet_driver.h"
struct dispatch_fixture
{
    rocketnet_server_transport_io_t transport;
    uint64_t starts, finishes, sum;
    uint8_t frame[16];
};
void dispatch_fixture_init( dispatch_fixture * );
uint8_t * dispatch_fixture_start( void *, const rocketnet_address_t *, uint64_t * );
void dispatch_fixture_finish( void *, uint64_t, uint8_t *, int );
