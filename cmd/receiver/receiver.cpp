/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet.h"

static volatile int quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

int main()
{
    signal( SIGINT, interrupt_handler ); signal( SIGTERM, interrupt_handler );

    if ( !rocketnet_init() )
    {
        rocketnet_error( "could not initialize rocketnet" );
        return 1;        
    }

    while ( !quit )
    {
        // ...

        rocketnet_platform_sleep( 1.0 / 100.0 );
    }

    rocketnet_term();

    return 0;
}
