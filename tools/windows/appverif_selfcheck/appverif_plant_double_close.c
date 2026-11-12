/*
    appverif_plant_double_close: a planted handle fault for appverif_run.py --self-check.

    Closes one event handle twice. The second CloseHandle passes a handle the
    process no longer owns, which the Handles layer reports as a stop (an
    invalid handle, 0x300). The stop is continuable: with the run's stop
    settings it is logged with its stack trace and the program goes on, so the
    program exits 0 and only the log can show the fault. A self-check that
    sees no Handles logEntry for this program proves the stop settings do not
    log, whatever the exit status says.
*/

#include <windows.h>
#include <stdio.h>

int main( void )
{
    HANDLE event = CreateEventW( NULL, TRUE, FALSE, NULL );
    if ( event == NULL )
    {
        printf( "appverif_plant_double_close: CreateEventW failed (%lu)\n", GetLastError() );
        return 2;
    }
    const BOOL first = CloseHandle( event );
    const BOOL second = CloseHandle( event );
    printf( "appverif_plant_double_close: first close %d, second close %d\n", (int) first, (int) second );
    return 0;
}
