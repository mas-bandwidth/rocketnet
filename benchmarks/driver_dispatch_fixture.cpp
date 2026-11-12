/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Separate translation unit, no LTO: the measured caller cannot devirtualize.
#include "driver_dispatch_fixture.h"
#include <string.h>
uint8_t * dispatch_fixture_start( void * instance, const rocketnet_address_t *, uint64_t * id )
{
    auto * f = (dispatch_fixture *) instance;
    *id = ++f->starts;
    return f->frame;
}
void dispatch_fixture_finish( void * instance, uint64_t id, uint8_t * frame, int bytes )
{
    auto * f = (dispatch_fixture *) instance;
    ++f->finishes;
    f->sum += id + frame[0] + bytes;
}
void dispatch_fixture_init( dispatch_fixture * f )
{
    // Named members on a zeroed table, so an operation added to the driver later stays NULL here instead of breaking this build.
    static const rocketnet_server_transport_io_operations_t ops = []
    {
        rocketnet_server_transport_io_operations_t o = {};
        o.flags = ROCKETNET_TRANSPORT_STAGED;
        o.start_packet = dispatch_fixture_start;
        o.finish_packet = dispatch_fixture_finish;
        return o;
    }();
    memset( f, 0, sizeof( *f ) );
    f->transport.operations = &ops;
    f->transport.instance = f;
}
