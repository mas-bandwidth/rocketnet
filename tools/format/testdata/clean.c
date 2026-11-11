#include <stdint.h>
#include <stdio.h>

static int round_up( int value, int multiple )
{
    int remainder = value % multiple;
    if ( remainder != 0 )
    {
        return value + multiple - remainder;
    }
    return value;
}

static void describe_port( uint16_t port, char * buffer, int length )
{
    snprintf( buffer, length, "%d", (int) port );
}

int main( void )
{
    char buffer[32];
    int n = 1;
    n = round_up( n, 4 );
    describe_port( (uint16_t) n, buffer, 32 );
    printf( "%s\n", buffer );
    return 0;
}
