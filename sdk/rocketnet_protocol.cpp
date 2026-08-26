/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"

#include <string.h>

void rocketnet_write_prefilter_header( uint8_t ** p, uint8_t packet_type, const uint8_t * title_tag )
{
    rocketnet_write_uint8( p, packet_type );
    rocketnet_write_bytes( p, title_tag, ROCKETNET_TITLE_TAG_BYTES );
}

int rocketnet_prefilter_check( const uint8_t * packet_data, int packet_bytes, const uint8_t * title_tag )
{
    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES )
        return -1;

    if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return -1;

    if ( packet_data[1] != title_tag[0] || packet_data[2] != title_tag[1] || packet_data[3] != title_tag[2] )
        return -1;

    return packet_data[0];
}
