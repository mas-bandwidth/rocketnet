/*
    appverif_plant_clean: the negative control for appverif_run.py --self-check.

    The same calls as the two planted programs, each used correctly: one event
    handle closed once, one 17-byte block written inside its bounds and freed.
    Under the run's stop settings it must exit 0 with a log that holds a
    session and no logEntry; a stop here means the self-check's stop reading is
    wrong, not that the plants are right.
*/

#include <windows.h>
#include <stdio.h>

int main( void )
{
    HANDLE event = CreateEventW( NULL, TRUE, FALSE, NULL );
    if ( event == NULL )
    {
        printf( "appverif_plant_clean: CreateEventW failed (%lu)\n", GetLastError() );
        return 2;
    }
    if ( !CloseHandle( event ) )
    {
        printf( "appverif_plant_clean: CloseHandle failed (%lu)\n", GetLastError() );
        return 2;
    }
    void * allocation = HeapAlloc( GetProcessHeap(), 0, 17 );
    if ( allocation == NULL )
    {
        printf( "appverif_plant_clean: HeapAlloc failed\n" );
        return 2;
    }
    volatile unsigned char * block = (volatile unsigned char *) allocation;
    block[0] = 0x5a;
    block[16] = 0xa5;
    if ( !HeapFree( GetProcessHeap(), 0, allocation ) )
    {
        printf( "appverif_plant_clean: HeapFree failed (%lu)\n", GetLastError() );
        return 2;
    }
    printf( "appverif_plant_clean: one handle closed once, one block written in bounds and freed\n" );
    return 0;
}
