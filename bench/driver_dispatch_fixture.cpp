/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Separate translation unit, no LTO: the measured caller cannot devirtualize.
#include "driver_dispatch_fixture.h"
#include <string.h>
uint8_t * dispatch_fixture_start(void * instance, const rocketnet_address_t *, uint64_t * id)
{
    auto * f = (dispatch_fixture *)instance;
    *id = ++f->starts;
    return f->frame;
}
void dispatch_fixture_finish(void * instance, uint64_t id, uint8_t * frame, int bytes)
{
    auto * f = (dispatch_fixture *)instance;
    ++f->finishes;
    f->sum += id + frame[0] + bytes;
}
void dispatch_fixture_init(dispatch_fixture * f)
{
    static rocketnet_server_transport_io_operations_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.flags = ROCKETNET_TRANSPORT_STAGED;
    ops.start_packet = dispatch_fixture_start;
    ops.finish_packet = dispatch_fixture_finish;
    memset(f, 0, sizeof(*f));
    f->transport.operations = &ops;
    f->transport.instance = f;
}
