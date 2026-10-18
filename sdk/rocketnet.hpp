/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// C++ transport interfaces: a thin header-only C++11 wrapper over the C
// transport surface of rocketnet.h. ClientTransport and ServerTransport are
// abstract classes whose pure virtual methods map one-to-one onto the
// rocketnet_client_transport_* and rocketnet_server_transport_* functions
// (same name without the prefix, the transport pointer dropped). Create and
// destroy live on the concrete classes: a static create returning NULL on
// failure (no throwing constructor) and a destructor.
//
// The interface uses no exceptions, no RTTI and no STL; the zero-copy views
// are passed through exactly as the C API gives them, with no ownership.

#ifndef ROCKETNET_HPP
#define ROCKETNET_HPP

#include "rocketnet.h"

namespace rocketnet
{

class ClientTransport
{
public:
    virtual ~ClientTransport() {}

    virtual void update( double time ) = 0;
    virtual void send_packet( const uint8_t * packet_data, int packet_bytes ) = 0;
    virtual int receive_packet( uint8_t * packet_data, int packet_capacity ) = 0;
    virtual int max_payload_bytes() = 0;
    virtual int num_payloads() = 0;
    virtual uint8_t * payload( int payload_index, int * payload_bytes ) = 0;
    virtual int payloads( uint8_t * const ** payload_data, const int ** payload_bytes ) = 0;
    virtual uint8_t * start_packet( int request_bytes, struct rocketnet_packet_context_t * context ) = 0;
    virtual uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity ) = 0;
    virtual int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes ) = 0;
    virtual void abort_packet( struct rocketnet_packet_context_t * context ) = 0;
    virtual void send_packets() = 0;
    virtual int state() = 0;
    virtual const char * driver() = 0;
    virtual int client_index() = 0;
    virtual void disconnect() = 0;
    virtual const struct rocketnet_counters_t * counters() = 0;
    virtual void stats( struct rocketnet_client_transport_stats_t * stats ) = 0;
    virtual int acked_packets( const uint64_t ** packet_ids ) = 0;
};

class ServerTransport
{
public:
    virtual ~ServerTransport() {}

    virtual void stop() = 0;
    virtual int state() = 0;
    virtual uint16_t port() = 0;
    virtual const char * driver() = 0;
    virtual void update( double time ) = 0;
    virtual void set_admission_callback( rocketnet_server_admission_callback_t callback, void * context ) = 0;
    virtual int num_payloads() = 0;
    virtual uint8_t * payload( int payload_index, int * client_index, int * payload_bytes ) = 0;
    virtual int payloads( const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes ) = 0;
    virtual uint8_t * start_packet( int client_index, int request_bytes, struct rocketnet_packet_context_t * context ) = 0;
    virtual uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity ) = 0;
    virtual int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes ) = 0;
    virtual void abort_packet( struct rocketnet_packet_context_t * context ) = 0;
    virtual void send_packets() = 0;
    virtual void send_packet( int client_index, const uint8_t * payload_data, int payload_bytes ) = 0;
    virtual void disconnect_client( int client_index ) = 0;
    virtual int num_connected_clients() = 0;
    virtual int is_client_connected( int client_index ) = 0;
    virtual uint64_t client_id( int client_index ) = 0;
    virtual const struct rocketnet_counters_t * counters() = 0;
    virtual int max_payload_bytes() = 0;
    virtual void stats( struct rocketnet_server_transport_stats_t * stats ) = 0;
    virtual void client_stats( int client_index, struct rocketnet_server_transport_client_stats_t * stats ) = 0;
    virtual int acked_packets( const int ** client_index, const uint64_t ** packet_ids ) = 0;
};

class PortableClientTransport : public ClientTransport
{
public:
    static PortableClientTransport * create( void * context, const uint8_t * connect_token,
                                            const struct rocketnet_client_transport_config_t * config, double time )
    {
        struct rocketnet_client_transport_t * c = rocketnet_client_transport_create( context, connect_token, config, time );
        if ( !c )
            return NULL;
        return new PortableClientTransport( c );
    }

    ~PortableClientTransport()
    {
        rocketnet_client_transport_destroy( transport_ );
    }

    void update( double time ) { rocketnet_client_transport_update( transport_, time ); }
    void send_packet( const uint8_t * packet_data, int packet_bytes )
    { rocketnet_client_transport_send_packet( transport_, packet_data, packet_bytes ); }
    int receive_packet( uint8_t * packet_data, int packet_capacity )
    { return rocketnet_client_transport_receive_packet( transport_, packet_data, packet_capacity ); }
    int max_payload_bytes() { return rocketnet_client_transport_max_payload_bytes( transport_ ); }
    int num_payloads() { return rocketnet_client_transport_num_payloads( transport_ ); }
    uint8_t * payload( int payload_index, int * payload_bytes )
    { return rocketnet_client_transport_payload( transport_, payload_index, payload_bytes ); }
    int payloads( uint8_t * const ** payload_data, const int ** payload_bytes )
    { return rocketnet_client_transport_payloads( transport_, payload_data, payload_bytes ); }
    uint8_t * start_packet( int request_bytes, struct rocketnet_packet_context_t * context )
    { return rocketnet_client_transport_start_packet( transport_, request_bytes, context ); }
    uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity )
    { return rocketnet_client_transport_packet_payload( transport_, context, capacity ); }
    int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes )
    { return rocketnet_client_transport_finish_packet( transport_, context, payload_bytes ); }
    void abort_packet( struct rocketnet_packet_context_t * context )
    { rocketnet_client_transport_abort_packet( transport_, context ); }
    void send_packets() { rocketnet_client_transport_send_packets( transport_ ); }
    int state() { return rocketnet_client_transport_state( transport_ ); }
    const char * driver() { return rocketnet_client_transport_driver( transport_ ); }
    int client_index() { return rocketnet_client_transport_client_index( transport_ ); }
    void disconnect() { rocketnet_client_transport_disconnect( transport_ ); }
    const struct rocketnet_counters_t * counters() { return rocketnet_client_transport_counters( transport_ ); }
    void stats( struct rocketnet_client_transport_stats_t * stats )
    { rocketnet_client_transport_stats( transport_, stats ); }
    int acked_packets( const uint64_t ** packet_ids )
    { return rocketnet_client_transport_acked_packets( transport_, packet_ids ); }

private:
    explicit PortableClientTransport( struct rocketnet_client_transport_t * c ) : transport_( c ) {}
    PortableClientTransport( const PortableClientTransport & );
    PortableClientTransport & operator=( const PortableClientTransport & );
    struct rocketnet_client_transport_t * transport_;
};

class PortableServerTransport : public ServerTransport
{
public:
    static PortableServerTransport * create( void * context, const char * public_address_string,
                                            const struct rocketnet_server_transport_config_t * config )
    {
        struct rocketnet_server_transport_t * s = rocketnet_server_transport_create( context, public_address_string, config );
        if ( !s )
            return NULL;
        return new PortableServerTransport( s );
    }

    ~PortableServerTransport()
    {
        rocketnet_server_transport_destroy( transport_ );
    }

    void stop() { rocketnet_server_transport_stop( transport_ ); }
    int state() { return rocketnet_server_transport_state( transport_ ); }
    uint16_t port() { return rocketnet_server_transport_port( transport_ ); }
    const char * driver() { return rocketnet_server_transport_driver( transport_ ); }
    void update( double time ) { rocketnet_server_transport_update( transport_, time ); }
    void set_admission_callback( rocketnet_server_admission_callback_t callback, void * context )
    { rocketnet_server_transport_set_admission_callback( transport_, callback, context ); }
    int num_payloads() { return rocketnet_server_transport_num_payloads( transport_ ); }
    uint8_t * payload( int payload_index, int * client_index, int * payload_bytes )
    { return rocketnet_server_transport_payload( transport_, payload_index, client_index, payload_bytes ); }
    int payloads( const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
    { return rocketnet_server_transport_payloads( transport_, client_index, payload_data, payload_bytes ); }
    uint8_t * start_packet( int client_index, int request_bytes, struct rocketnet_packet_context_t * context )
    { return rocketnet_server_transport_start_packet( transport_, client_index, request_bytes, context ); }
    uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity )
    { return rocketnet_server_transport_packet_payload( transport_, context, capacity ); }
    int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes )
    { return rocketnet_server_transport_finish_packet( transport_, context, payload_bytes ); }
    void abort_packet( struct rocketnet_packet_context_t * context )
    { rocketnet_server_transport_abort_packet( transport_, context ); }
    void send_packets() { rocketnet_server_transport_send_packets( transport_ ); }
    void send_packet( int client_index, const uint8_t * payload_data, int payload_bytes )
    { rocketnet_server_transport_send_packet( transport_, client_index, payload_data, payload_bytes ); }
    void disconnect_client( int client_index )
    { rocketnet_server_transport_disconnect_client( transport_, client_index ); }
    int num_connected_clients() { return rocketnet_server_transport_num_connected_clients( transport_ ); }
    int is_client_connected( int client_index )
    { return rocketnet_server_transport_is_client_connected( transport_, client_index ); }
    uint64_t client_id( int client_index )
    { return rocketnet_server_transport_client_id( transport_, client_index ); }
    const struct rocketnet_counters_t * counters() { return rocketnet_server_transport_counters( transport_ ); }
    int max_payload_bytes() { return rocketnet_server_transport_max_payload_bytes( transport_ ); }
    void stats( struct rocketnet_server_transport_stats_t * stats )
    { rocketnet_server_transport_stats( transport_, stats ); }
    void client_stats( int client_index, struct rocketnet_server_transport_client_stats_t * stats )
    { rocketnet_server_transport_client_stats( transport_, client_index, stats ); }
    int acked_packets( const int ** client_index, const uint64_t ** packet_ids )
    { return rocketnet_server_transport_acked_packets( transport_, client_index, packet_ids ); }

private:
    explicit PortableServerTransport( struct rocketnet_server_transport_t * s ) : transport_( s ) {}
    PortableServerTransport( const PortableServerTransport & );
    PortableServerTransport & operator=( const PortableServerTransport & );
    struct rocketnet_server_transport_t * transport_;
};

} // namespace rocketnet

// -----------------------------------------
//            the serialize read (C++)
// -----------------------------------------

// Include serialize.h before this header and the stream forms of the
// acquire/commit surface appear: the same calls, with a WriteStream bound
// at the frame's aligned payload root on start and measured on finish, and
// a ReadStream bound over each received payload. Plain C callers see none
// of this; it compiles to nothing but the calls above.

#if defined(__cplusplus) && defined(SERIALIZE_H)

namespace rocketnet
{
    inline bool server_start_packet( struct rocketnet_server_transport_t * server, int client_index, int request_bytes, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        uint8_t * frame = rocketnet_server_transport_start_packet( server, client_index, request_bytes, &context );
        if ( !frame )
            return false;
        stream.Initialize( frame + context.payload_index, context.payload_capacity );
        return true;
    }

    // serialize's write side checks bounds with asserts only, by design: a
    // Release overflow past the payload capacity is not caught here or
    // there. Size packets to the capacity the context reports.

    inline bool server_finish_packet( struct rocketnet_server_transport_t * server, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        stream.Flush();
        return rocketnet_server_transport_finish_packet( server, &context, (int) stream.GetBytesProcessed() ) != 0;
    }

    inline void server_abort_packet( struct rocketnet_server_transport_t * server, struct rocketnet_packet_context_t & context )
    {
        rocketnet_server_transport_abort_packet( server, &context );
    }

    inline bool server_payload( struct rocketnet_server_transport_t * server, int payload_index, serialize::ReadStream & stream, int & client_index )
    {
        int payload_bytes;
        uint8_t * payload = rocketnet_server_transport_payload( server, payload_index, &client_index, &payload_bytes );
        if ( !payload )
            return false;
        stream.Initialize( payload, payload_bytes );
        return true;
    }

    inline bool client_start_packet( struct rocketnet_client_transport_t * client, int request_bytes, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        uint8_t * frame = rocketnet_client_transport_start_packet( client, request_bytes, &context );
        if ( !frame )
            return false;
        stream.Initialize( frame + context.payload_index, context.payload_capacity );
        return true;
    }

    inline bool client_finish_packet( struct rocketnet_client_transport_t * client, serialize::WriteStream & stream, struct rocketnet_packet_context_t & context )
    {
        stream.Flush();
        return rocketnet_client_transport_finish_packet( client, &context, (int) stream.GetBytesProcessed() ) != 0;
    }

    inline void client_abort_packet( struct rocketnet_client_transport_t * client, struct rocketnet_packet_context_t & context )
    {
        rocketnet_client_transport_abort_packet( client, &context );
    }

    inline bool client_payload( struct rocketnet_client_transport_t * client, int payload_index, serialize::ReadStream & stream )
    {
        int payload_bytes;
        uint8_t * payload = rocketnet_client_transport_payload( client, payload_index, &payload_bytes );
        if ( !payload )
            return false;
        stream.Initialize( payload, payload_bytes );
        return true;
    }
}

#endif // #if defined(__cplusplus) && defined(SERIALIZE_H)

#endif // ROCKETNET_HPP
