/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "client_backend.h"
#include "client_backend_main.h"
#include "client_backend_bpf.h"
#include "client_backend_config.h"

#include "platform/platform.h"

#include <memory.h>
#include <stdio.h>
#include <signal.h>

static struct config_t config;
static struct bpf_t bpf;
static struct main_t main_data;

volatile bool quit;
volatile bool clean_shutdown = false;

void interrupt_handler( int signal )
{
    (void) signal; quit = true;
}

void clean_shutdown_handler( int signal )
{
    (void) signal;
    clean_shutdown = true;
    quit = true;
}

static void cleanup()
{
    main_shutdown( &main_data );
    bpf_shutdown( &bpf );
    fflush( stdout );
}

int main( int argc, char *argv[] )
{
    platform_init();

    printf( "RocketNet Client Backend\n" );

    fflush( stdout );

    signal( SIGINT,  interrupt_handler );
    signal( SIGTERM, clean_shutdown_handler );
    signal( SIGHUP,  clean_shutdown_handler );

    printf( "Reading config\n" );

    fflush( stdout );

    if ( !read_config( &config ) )
    {
        cleanup();
        return 1;
    }

    fflush( stdout );

    printf( "Initializing BPF\n" );

    fflush( stdout );

    if ( !bpf_init( &bpf, config.public_address ) )
    {
        cleanup();
        return 1;
    }

    fflush( stdout );

    printf( "Starting main thread\n" );

    fflush( stdout );

    if ( !main_init( &main_data, &config, &bpf ) )
    {
        cleanup();
        return 1;
    }

    int result = main_run( &main_data );

    cleanup();

    return result;
}
