/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "transport_dispatch_fixture.h"
#include <chrono>
#include <stdio.h>
#include <stdlib.h>

static double measure( dispatch_fixture * f, bool indirect, int count )
{
    rocketnet_address_t to = {};
    const auto start = std::chrono::steady_clock::now();
    if ( indirect )
    {
        for ( int i = 0; i < count; ++i )
        {
            uint64_t id;
            if ( !rocketnet_server_transport_io_has( f->transport, ROCKETNET_TRANSPORT_STAGED ) )
                abort();
            uint8_t * frame = f->transport.operations->start_packet( f->transport.instance, &to, &id );
            frame[0] = (uint8_t) i;
            if ( !rocketnet_server_transport_io_has( f->transport, ROCKETNET_TRANSPORT_STAGED ) )
                abort();
            f->transport.operations->finish_packet( f->transport.instance, id, frame, 1 );
        }
    }
    else
    {
        for ( int i = 0; i < count; ++i )
        {
            uint64_t id;
            if ( !f->transport.instance )
                abort();
            uint8_t * frame = dispatch_fixture_start( f->transport.instance, &to, &id );
            frame[0] = (uint8_t) i;
            if ( !f->transport.instance )
                abort();
            dispatch_fixture_finish( f->transport.instance, id, frame, 1 );
        }
    }
    const auto end = std::chrono::steady_clock::now();
    if ( f->starts != (uint64_t) count || f->finishes != (uint64_t) count || !f->sum )
        abort();
    return std::chrono::duration<double, std::nano>( end - start ).count() / count;
}
int main()
{
    for ( int sample = -1; sample < 9; ++sample )
    {
        double ns[2];
        for ( int j = 0; j < 2; ++j )
        {
            const int indirect = ( j + sample + 1 ) % 2;
            dispatch_fixture fixture;
            dispatch_fixture_init( &fixture );
            ns[indirect] = measure( &fixture, indirect != 0, 10000000 );
        }
        if ( sample >= 0 )
            printf( "sample=%d direct_ns=%.3f indirect_ns=%.3f delta_ns=%.3f\n", sample, ns[0], ns[1], ns[1] - ns[0] );
    }
}
