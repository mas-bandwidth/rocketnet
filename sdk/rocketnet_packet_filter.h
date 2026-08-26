/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_PACKET_FILTER_H
#define ROCKETNET_PACKET_FILTER_H

#include "rocketnet.h"
#include "rocketnet_address.h"

void rocketnet_generate_pittle( uint8_t * output, const uint8_t * from_address, const uint8_t * to_address, uint16_t packet_length );

void rocketnet_generate_chonkle( uint8_t * output, const uint8_t * magic, const uint8_t * from_address, const uint8_t * to_address, uint16_t packet_length );

bool rocketnet_basic_packet_filter( const uint8_t * data, uint16_t packet_length );

void rocketnet_address_data( const rocketnet_address_t * address, uint8_t * address_data );


#endif // #ifndef ROCKETNET_PACKET_FILTER_H
