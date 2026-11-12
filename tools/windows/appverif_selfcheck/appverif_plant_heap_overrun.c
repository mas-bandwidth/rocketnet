/*
    appverif_plant_heap_overrun: a planted heap fault for appverif_run.py --self-check.

    Allocates 17 bytes from the process heap and writes one byte at index 17,
    one past the block, then frees it. Under the Heaps layer (full page heap)
    the byte lands in the block's suffix fill pattern, and the free reports the
    corrupted pattern as a stop. Heap corruption stops are not continuable:
    the stop is logged with its stack trace and the process then ends with
    STATUS_VERIFIER_STOP (0xC0000421). A self-check that sees that exit status
    and no Heaps logEntry proves the stop settings do not log.
*/

#include <windows.h>
#include <stdio.h>

int main( void )
{
    void * allocation = HeapAlloc( GetProcessHeap(), 0, 17 );
    if ( allocation == NULL )
    {
        printf( "appverif_plant_heap_overrun: HeapAlloc failed\n" );
        return 2;
    }
    volatile unsigned char * block = (volatile unsigned char *) allocation;
    block[16] = 0x5a;
    block[17] = 0xa5;
    const BOOL freed = HeapFree( GetProcessHeap(), 0, allocation );
    printf( "appverif_plant_heap_overrun: wrote one byte past a 17-byte block, free %d\n", (int) freed );
    return 0;
}
