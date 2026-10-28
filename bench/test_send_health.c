/*
    rocketnet transport bench: send-health classification around the stop
    (bench_common.h, PROTOCOL.md "Sender counts are by construction").

    No sockets, no clock, no threads: it drives bench_send_observe and
    bench_send_validate directly with the stop flag set by hand.

    1. ECONNREFUSED before the stop is a refusal in the sample, and
       bench_send_validate refuses the sample (exit 2, in a child).
    2. ECONNREFUSED after the stop is late: not a refusal, no abort at any
       count, and bench_send_validate lets the sample stand.
    3. Back pressure is unchanged either side of the stop.

    Built and run by bench/selftest.sh.
*/

#include "bench_common.h"

#include <sys/wait.h>

static int failures;

static void check( int condition, const char * what )
{
    if ( !condition )
    {
        printf( "FAIL: %s\n", what );
        failures++;
    }
}

static int validate_exit_status( const struct bench_send_health_t * health )
{
    fflush( stdout );
    pid_t child = fork();
    if ( child < 0 )
    {
        perror( "fork" );
        exit( 1 );
    }
    if ( child == 0 )
    {
        freopen( "/dev/null", "w", stdout );
        freopen( "/dev/null", "w", stderr );
        bench_send_validate( health );
        _exit( 0 );
    }
    int status = 0;
    if ( waitpid( child, &status, 0 ) != child || !WIFEXITED( status ) )
        return -1;
    return WEXITSTATUS( status );
}

int main( void )
{
    struct bench_send_health_t health;

    /* 1. in the window */
    memset( &health, 0, sizeof( health ) );
    bench_stop_requested = 0;
    check( bench_send_ok( &health, "send" ) == 1, "a successful send counts" );
    check( bench_send_refused( &health, "send", ECONNREFUSED ) == 0, "an in-window refusal is not a sent packet" );
    check( health.refused == 1 && health.late_refused == 0, "an in-window ECONNREFUSED is a refusal in the sample" );
    check( validate_exit_status( &health ) == 2, "an in-window ECONNREFUSED fails the sample" );

    /* 2. after the stop, at a count far past the consecutive-refusal limit */
    memset( &health, 0, sizeof( health ) );
    bench_stop_requested = 0;
    check( bench_send_ok( &health, "send" ) == 1, "a successful send counts" );
    bench_stop_requested = 1;
    for ( int i = 0; i < 4 * BENCH_SEND_REFUSAL_LIMIT; i++ )
    {
        check( bench_send_refused( &health, "send", ECONNREFUSED ) == 0, "a late refusal is not a sent packet" );
    }
    check( health.refused == 0, "a late ECONNREFUSED is not a refusal in the sample" );
    check( health.late_refused == 4 * BENCH_SEND_REFUSAL_LIMIT, "every late ECONNREFUSED is counted as late" );
    check( validate_exit_status( &health ) == 0, "late ECONNREFUSED leaves the sample standing" );

    /* 3. back pressure either side of the stop */
    memset( &health, 0, sizeof( health ) );
    bench_stop_requested = 0;
    bench_send_refused( &health, "send", ENOBUFS );
    check( health.refused == 1 && health.back_pressure == 1, "in-window back pressure is counted as before" );
    check( validate_exit_status( &health ) == 0, "in-window back pressure leaves the sample standing" );

    if ( failures != 0 )
    {
        printf( "test_send_health: %d check(s) failed\n", failures );
        return 1;
    }
    printf( "test_send_health: ok\n" );
    return 0;
}
