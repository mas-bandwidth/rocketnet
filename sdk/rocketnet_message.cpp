/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_message.h"

// Not built yet: the red step of the layer 2 card. Every call refuses.

const char * rocketnet_message_violation_name( int ) { return NULL; }
int rocketnet_message_record_header_bytes( const struct rocketnet_message_record_t * ) { return 0; }
int rocketnet_message_record_valid( const struct rocketnet_message_record_t * ) { return 0; }
void rocketnet_message_writer_begin( struct rocketnet_message_writer_t * writer, uint8_t * payload, int capacity ) { writer->payload = payload; writer->capacity = capacity; writer->bytes = 0; writer->records = 0; }
int rocketnet_message_writer_space( const struct rocketnet_message_writer_t * ) { return 0; }
int rocketnet_message_writer_add( struct rocketnet_message_writer_t *, const struct rocketnet_message_record_t * ) { return 0; }
int rocketnet_message_writer_finish( struct rocketnet_message_writer_t * ) { return 0; }
int rocketnet_message_single_capacity( int ) { return 0; }
int rocketnet_message_single_finish( uint8_t *, int, int ) { return 0; }
uint32_t rocketnet_message_enabled_codes( uint32_t ) { return 0; }
int rocketnet_message_reader_begin( struct rocketnet_message_reader_t * reader, const uint8_t *, int, const struct rocketnet_message_limits_t * ) { reader->violation = ROCKETNET_MESSAGE_VIOLATION_BAD_FORM; return reader->violation; }
int rocketnet_message_reader_next( struct rocketnet_message_reader_t *, struct rocketnet_message_record_t * ) { return 0; }
int rocketnet_message_check_payload( const uint8_t *, int, const struct rocketnet_message_limits_t * ) { return ROCKETNET_MESSAGE_VIOLATION_BAD_FORM; }
void rocketnet_message_write_config_body( uint8_t *, const struct rocketnet_message_config_body_t * ) {}
int rocketnet_message_read_config_body( const uint8_t *, int, struct rocketnet_message_config_body_t * ) { return ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL; }
