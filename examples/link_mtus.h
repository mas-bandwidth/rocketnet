/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The backend examples' --link-mtus <list> flag: an ordered list of link
    MTUs, largest first, comma separated, such as --link-mtus 1492,1200,576.
    It sets link_mtus in the client or server config. With no flag the config
    keeps its default list, the one entry 1500.

    The client and the server must be given the same list: the server answers
    only the packet sizes its own list gives, so a client probing a size the
    server's list does not hold never gets that size. See "Path MTU" in
    examples/backend/README.md.
*/

#pragma once

#ifndef ROCKETNET_EXAMPLES_LINK_MTUS_H
#define ROCKETNET_EXAMPLES_LINK_MTUS_H

#include "rocketnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Takes "--link-mtus <list>" out of argv, so the program's own arguments are
// parsed as before, and returns the arguments left. The entries go to
// link_mtus and their count to num_link_mtus, 0 when the flag is absent.
// Returns -1, having printed why, for a list that does not parse: create
// checks the values themselves and names a refused list by its status.

static inline int rn_cli_take_link_mtus( int argc, char ** argv, int * link_mtus, int * num_link_mtus )
{
    *num_link_mtus = 0;

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--link-mtus" ) != 0 )
        {
            continue;
        }

        if ( i + 1 >= argc )
        {
            printf( "FAIL: --link-mtus takes a list such as 1492,1200,576\n" );
            return -1;
        }

        const char * p = argv[i + 1];
        int count = 0;
        while ( 1 )
        {
            char * end = NULL;
            const long value = strtol( p, &end, 10 );
            if ( p[0] < '0' || p[0] > '9' || end == p || ( *end != ',' && *end != '\0' ) || value > 65535 || count == ROCKETNET_MAX_LINK_MTUS )
            {
                printf( "FAIL: --link-mtus takes 1 .. %d comma-separated MTUs, not \"%s\"\n", ROCKETNET_MAX_LINK_MTUS, argv[i + 1] );
                return -1;
            }

            link_mtus[count++] = (int) value;
            if ( *end == '\0' )
            {
                break;
            }
            p = end + 1;
        }

        *num_link_mtus = count;

        for ( int j = i; j + 2 < argc; j++ )
        {
            argv[j] = argv[j + 2];
        }
        argc -= 2;
        argv[argc] = NULL;
        i--;
    }

    return argc;
}

#endif // ROCKETNET_EXAMPLES_LINK_MTUS_H
