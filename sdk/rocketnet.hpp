/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

/** @brief Header-only C++ interfaces for RocketNet client and server transports.
 *
 * Portable wrappers own the C transport returned by their factory. Failed C
 * creation returns NULL, but allocating the C++ wrapper uses new and may throw
 * std::bad_alloc. Views returned by methods remain library-owned.
 */

#ifndef ROCKETNET_HPP
#define ROCKETNET_HPP

#include "rocketnet.h"

namespace rocketnet
{

/** @brief Abstract client transport contract; methods map to the client C API. */
class ClientTransport
{
public:
    virtual ~ClientTransport() {}

/**
 * @brief Advance transport state using the supplied monotonic game-clock time.
 * Time is in seconds from the caller's monotonic game clock. Call on the main thread outside an open packet-fill phase.
 * @param[in] time Current time in seconds from the caller's monotonic game clock.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void update( double time ) = 0;
/**
 * @brief Send a payload through the client convenience path.
 * The C convenience call flushes inline. Call on the main thread and never during an open packet fill.
 * @param[in] packet_data Input payload bytes; NULL is a counted no-op.
 * @param[in] packet_bytes Payload length in bytes, from 1 through max_payload_bytes().
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void send_packet( const uint8_t * packet_data, int packet_bytes ) = 0;
/**
 * @brief Copy the next delivered payload into a caller-owned buffer.
 * Returns a payload length, or zero when the batch is drained or the call is refused. Oversized payloads are refused whole, never truncated.
 * @param[out] packet_data Writable caller-owned buffer; NULL is a counted no-op.
 * @param[in] packet_capacity Byte count or capacity for this operation; it must fit the configured limit or caller buffer.
 * @return Returns the copied payload length, or zero when drained or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int receive_packet( uint8_t * packet_data, int packet_capacity ) = 0;
/**
 * @brief Return the configured maximum payload size in bytes.
 * Use the configured maximum to size buffers that must accept reassembled payloads.
 * @return Returns the corresponding C API result; zero represents no data or a refused call where applicable.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int max_payload_bytes() = 0;
/**
 * @brief Return the number of payloads delivered by the last update.
 * The payload batch is valid until the next update.
 * @return Returns the available item count, or zero when empty or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int num_payloads() = 0;
/**
 * @brief Return one payload view from the last update batch.
 * The returned library-owned view and output length are valid until the next update; an invalid index returns NULL.
 * @param[in] payload_index Zero-based index in the current batch, from zero to num_payloads() minus one.
 * @param[out] payload_bytes Output payload length in bytes; NULL is a counted no-op.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual uint8_t * payload( int payload_index, int * payload_bytes ) = 0;
/**
 * @brief Return the last update batch as parallel arrays.
 * Returned library-owned arrays remain valid until the next update.
 * @param[out] payload_data Input payload or output payload-array pointer as indicated by this operation; NULL is a counted no-op.
 * @param[out] payload_bytes Output payload length in bytes; NULL is a counted no-op.
 * @return Returns the available item count, or zero when empty or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int payloads( uint8_t * const ** payload_data, const int ** payload_bytes ) = 0;
/**
 * @brief Acquire a writable packet lease.
 * request_bytes is from 1 through the configured maximum. Fill between update and send_packets, then synchronize all writers before flushing.
 * @param[in] request_bytes Byte count or capacity for this operation; it must fit the configured limit or caller buffer.
 * @param[out] context Output lease context; NULL is a counted no-op.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 * @code
 * rocketnet_packet_context_t lease;
 * if (transport.start_packet(64, &lease)) {
 *     int capacity = 0;
 *     uint8_t * payload = transport.packet_payload(&lease, &capacity);
 *     if (payload) { // write at most capacity bytes
 *         transport.finish_packet(&lease, bytes_written);
 *         transport.send_packets();
 *     } else {
 *         transport.abort_packet(&lease);
 *     }
 * }
 * @endcode
 */
    virtual uint8_t * start_packet( int request_bytes, struct rocketnet_packet_context_t * context ) = 0;
/**
 * @brief Validate a packet lease and return its writable payload view.
 * The context must be active, unmodified, and owned by this transport. The returned view expires at finish, abort, update, flush, or destruction.
 * @param[in] context Input active lease context owned by this transport; NULL is a counted no-op.
 * @param[out] capacity Byte count or capacity for this operation; it must fit the configured limit or caller buffer.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity ) = 0;
/**
 * @brief Seal and stage the payload written through a packet lease.
 * Returns the sealed wire size, or zero if the packet is dropped or the call is refused.
 * @param[in] context Input active lease context owned by this transport; NULL is a counted no-op.
 * @param[in] payload_bytes Number of payload bytes written, from zero through the lease capacity.
 * @return Returns the sealed wire size, or zero when the packet is dropped or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes ) = 0;
/**
 * @brief Discard an active packet lease.
 * The context must be an active lease owned by this transport.
 * @param[in] context Input active lease context owned by this transport; NULL is a counted no-op.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void abort_packet( struct rocketnet_packet_context_t * context ) = 0;
/**
 * @brief Flush packets staged before this call.
 * Call on the main thread after joining the fill phase; packets staged later wait for a later flush.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void send_packets() = 0;
/**
 * @brief Return the transport connection or lifecycle state.
 * Client states are -7 token expired, -6 invalid token, -5 connection timeout, -4 response timeout, -3 request timeout, -2 cookie timeout, -1 denied, 0 disconnected, 1 requesting cookie, 2 sending request, 3 sending response, and 4 connected.
 * @return Returns the documented client or server state constant.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int state() = 0;
/**
 * @brief Return the driver name granted at creation.
 * This reports the selected driver, not the requested candidate list.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual const char * driver() = 0;
/**
 * @brief Return the client slot assigned to this connection.
 * Zero can be a valid slot; do not use it as an invalid sentinel.
 * @return Returns the corresponding C API result; zero represents no data or a refused call where applicable.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int client_index() = 0;
/**
 * @brief Begin a deliberate client disconnect.
 * Call on the main thread and never while a packet fill is open.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void disconnect() = 0;
/**
 * @brief Return a read-only view of transport counters.
 * The pointer is library-owned and must not be freed.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual const struct rocketnet_counters_t * counters() = 0;
/**
 * @brief Write a statistics snapshot to the caller-owned structure.
 * The output structure is caller-owned; a NULL output pointer is a counted no-op.
 * @param[out] stats Output value or array pointer; NULL is a counted no-op.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void stats( struct rocketnet_client_transport_stats_t * stats ) = 0;
/**
 * @brief Return packet identifiers acknowledged by the last update.
 * The library-owned array is valid until the next update; overflow is deferred to a later update.
 * @param[out] packet_ids Output value or array pointer; NULL is a counted no-op.
 * @return Returns the available item count, or zero when empty or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int acked_packets( const uint64_t ** packet_ids ) = 0;
};

/** @brief Abstract server transport contract; methods map to the server C API. */
class ServerTransport
{
public:
    virtual ~ServerTransport() {}

/**
 * @brief Begin stopping the server transport.
 * Call on the main thread outside a fill phase; wait for the stopped state before destruction.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void stop() = 0;
/**
 * @brief Return the transport connection or lifecycle state.
 * Client values are ROCKETNET_CLIENT_TRANSPORT_STATE_*; server values are ROCKETNET_SERVER_TRANSPORT_STATE_*.
 * @return Returns the documented client or server state constant.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int state() = 0;
/**
 * @brief Return the UDP port bound by the server transport.
 * Port zero is supported only when the granted driver discovers its bound port.
 * @return Returns the corresponding C API result; zero represents no data or a refused call where applicable.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual uint16_t port() = 0;
/**
 * @brief Return the driver name granted at creation.
 * This reports the selected driver, not the requested candidate list.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual const char * driver() = 0;
/**
 * @brief Advance transport state using the supplied monotonic game-clock time.
 * Time is in seconds from the caller's monotonic game clock. Call on the main thread outside an open packet-fill phase.
 * @param[in] time Current time in seconds from the caller's monotonic game clock.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void update( double time ) = 0;
/**
 * @brief Set or clear the server source-admission callback.
 * A NULL callback restores the default, which admits every source. Change it between updates.
 * @param[in] callback Admission callback, or NULL to restore the default admission behavior.
 * @param[in] context Opaque caller-owned value passed to callback; NULL is allowed and is not freed by the transport.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void set_admission_callback( rocketnet_server_admission_callback_t callback, void * context ) = 0;
/**
 * @brief Return the number of payloads delivered by the last update.
 * The payload batch is valid until the next update.
 * @return Returns the available item count, or zero when empty or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int num_payloads() = 0;
/**
 * @brief Return one payload view from the last update batch.
 * The returned library-owned view and output length are valid until the next update; an invalid index returns NULL.
 * @param[in] payload_index Zero-based index in the current batch, from zero to num_payloads() minus one.
 * @param[out] client_index Output zero-based client slot or parallel slot array; NULL is a counted no-op.
 * @param[out] payload_bytes Output payload length in bytes; NULL is a counted no-op.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual uint8_t * payload( int payload_index, int * client_index, int * payload_bytes ) = 0;
/**
 * @brief Return the last update batch as parallel arrays.
 * Returned library-owned arrays remain valid until the next update.
 * @param[out] client_index Output zero-based client slot or parallel slot array; NULL is a counted no-op.
 * @param[out] payload_data Input payload or output payload-array pointer as indicated by this operation; NULL is a counted no-op.
 * @param[out] payload_bytes Output payload length in bytes; NULL is a counted no-op.
 * @return Returns the available item count, or zero when empty or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int payloads( const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes ) = 0;
/**
 * @brief Acquire a writable packet lease.
 * request_bytes is from 1 through the configured maximum. Fill between update and send_packets, then synchronize all writers before flushing.
 * @param[in] client_index Zero-based client slot; invalid slots are refused or reported as disconnected.
 * @param[in] request_bytes Byte count or capacity for this operation; it must fit the configured limit or caller buffer.
 * @param[out] context Output lease context; NULL is a counted no-op.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 * @code
 * rocketnet_packet_context_t lease;
 * if (transport.start_packet(64, &lease)) {
 *     int capacity = 0;
 *     uint8_t * payload = transport.packet_payload(&lease, &capacity);
 *     if (payload) { // write at most capacity bytes
 *         transport.finish_packet(&lease, bytes_written);
 *         transport.send_packets();
 *     } else {
 *         transport.abort_packet(&lease);
 *     }
 * }
 * @endcode
 */
    virtual uint8_t * start_packet( int client_index, int request_bytes, struct rocketnet_packet_context_t * context ) = 0;
/**
 * @brief Validate a packet lease and return its writable payload view.
 * The context must be active, unmodified, and owned by this transport. The returned view expires at finish, abort, update, flush, or destruction.
 * @param[in] context Input active lease context owned by this transport; NULL is a counted no-op.
 * @param[out] capacity Byte count or capacity for this operation; it must fit the configured limit or caller buffer.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity ) = 0;
/**
 * @brief Seal and stage the payload written through a packet lease.
 * Returns the sealed wire size, or zero if the packet is dropped or the call is refused.
 * @param[in] context Input active lease context owned by this transport; NULL is a counted no-op.
 * @param[in] payload_bytes Number of payload bytes written, from zero through the lease capacity.
 * @return Returns the sealed wire size, or zero when the packet is dropped or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes ) = 0;
/**
 * @brief Discard an active packet lease.
 * The context must be an active lease owned by this transport.
 * @param[in] context Input active lease context owned by this transport; NULL is a counted no-op.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void abort_packet( struct rocketnet_packet_context_t * context ) = 0;
/**
 * @brief Flush packets staged before this call.
 * Call on the main thread after joining the fill phase; packets staged later wait for a later flush.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void send_packets() = 0;
/**
 * @brief Stage one payload for a client slot.
 * The helper acquires and commits one lease but does not flush it; call send_packets after the fill phase. It may be used in the same synchronized fill window as start_packet and finish_packet.
 * @param[in] client_index Zero-based client slot; an invalid or disconnected slot is refused.
 * @param[in] payload_data Input payload bytes; NULL is a counted no-op.
 * @param[in] payload_bytes Payload length in bytes, from 1 through max_payload_bytes().
 * @pre Create the transport before calling; do not call after destruction.
 * @note This forwards to rocketnet_server_transport_send_packet, which stages without calling send_packets.
 */
    virtual void send_packet( int client_index, const uint8_t * payload_data, int payload_bytes ) = 0;
/**
 * @brief Disconnect one client slot deliberately.
 * Call on the main thread outside a fill phase. The slot frees immediately; buffered drivers may flush part of the disconnect burst inline.
 * @param[in] client_index Zero-based client slot; invalid slots are refused or reported as disconnected.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void disconnect_client( int client_index ) = 0;
/**
 * @brief Return the number of connected client slots.
 * Returns zero when there are no clients or the C API refuses the call.
 * @return Returns the available item count, or zero when empty or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int num_connected_clients() = 0;
/**
 * @brief Check whether a client slot is connected.
 * Returns nonzero for a connected slot, and zero for a free, invalid, or refused query.
 * @param[in] client_index Zero-based client slot; invalid slots are refused or reported as disconnected.
 * @return Returns nonzero when connected, or zero when disconnected, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int is_client_connected( int client_index ) = 0;
/**
 * @brief Return the client ID associated with a connected slot.
 * The client ID identifies the client, not the connection instance. Returns zero when the slot is free or out of range.
 * @param[in] client_index Zero-based client slot; invalid slots return zero.
 * @return Returns the connected slot's client ID, or zero when free, out of range, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual uint64_t client_id( int client_index ) = 0;
/**
 * @brief Return a read-only view of transport counters.
 * The pointer is library-owned and must not be freed.
 * @return Returns a library-owned view or NULL when unavailable, invalid, or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual const struct rocketnet_counters_t * counters() = 0;
/**
 * @brief Return the configured maximum payload size in bytes.
 * Use the configured maximum to size buffers that must accept reassembled payloads.
 * @return Returns the corresponding C API result; zero represents no data or a refused call where applicable.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int max_payload_bytes() = 0;
/**
 * @brief Write a statistics snapshot to the caller-owned structure.
 * The output structure is caller-owned; a NULL output pointer is a counted no-op.
 * @param[out] stats Output value or array pointer; NULL is a counted no-op.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void stats( struct rocketnet_server_transport_stats_t * stats ) = 0;
/**
 * @brief Write the statistics snapshot for one client slot.
 * The output structure is caller-owned; values describe the slot's current connection.
 * @param[in] client_index Zero-based client slot; invalid slots are refused or reported as disconnected.
 * @param[out] stats Output value or array pointer; NULL is a counted no-op.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual void client_stats( int client_index, struct rocketnet_server_transport_client_stats_t * stats ) = 0;
/**
 * @brief Return packet identifiers acknowledged by the last update.
 * The library-owned array is valid until the next update; overflow is deferred to a later update.
 * @param[out] client_index Output zero-based client slot or parallel slot array; NULL is a counted no-op.
 * @param[out] packet_ids Output value or array pointer; NULL is a counted no-op.
 * @return Returns the available item count, or zero when empty or refused.
 * @pre Create the transport before calling; do not call after destruction.
 * @note This interface maps to the corresponding same-named C transport function with the transport pointer omitted.
 */
    virtual int acked_packets( const int ** client_index, const uint64_t ** packet_ids ) = 0;
};

/** @brief Portable wrapper that owns and forwards to a client C transport. */
class PortableClientTransport : public ClientTransport
{
public:
    /** @brief Create a portable client transport.
     * @param[in] context Optional opaque caller-owned context passed to the C API; NULL is allowed and the value is not freed by the transport.
     * @param[in] connect_token Encoded connection token; NULL is refused by the C API.
     * @param[in] config Client configuration, or NULL for defaults.
     * @param[in] time Current monotonic game-clock time in seconds.
     * @return New wrapper, or NULL if C creation fails; allocating the wrapper may throw std::bad_alloc.
     * @pre Call rocketnet_init before creating the C transport.
     * @note The wrapper owns the C transport. Disconnect and pump a few updates before deleting it.
     * @code
     * PortableClientTransport * client = PortableClientTransport::create(context, token, config, time);
     * if (client) {
     *     client->disconnect();
     *     for (int i = 0; i < 3; ++i) client->update(time + 1.0 + i / 60.0);
     *     delete client;
     * }
     * @endcode
     */
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

    /** @copydoc rocketnet::ClientTransport::update */
    void update( double time ) { rocketnet_client_transport_update( transport_, time ); }
    /** @copydoc rocketnet::ClientTransport::send_packet */
    void send_packet( const uint8_t * packet_data, int packet_bytes )
    { rocketnet_client_transport_send_packet( transport_, packet_data, packet_bytes ); }
    /** @copydoc rocketnet::ClientTransport::receive_packet */
    int receive_packet( uint8_t * packet_data, int packet_capacity )
    { return rocketnet_client_transport_receive_packet( transport_, packet_data, packet_capacity ); }
    /** @copydoc rocketnet::ClientTransport::max_payload_bytes */
    int max_payload_bytes() { return rocketnet_client_transport_max_payload_bytes( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::num_payloads */
    int num_payloads() { return rocketnet_client_transport_num_payloads( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::payload */
    uint8_t * payload( int payload_index, int * payload_bytes )
    { return rocketnet_client_transport_payload( transport_, payload_index, payload_bytes ); }
    /** @copydoc rocketnet::ClientTransport::payloads */
    int payloads( uint8_t * const ** payload_data, const int ** payload_bytes )
    { return rocketnet_client_transport_payloads( transport_, payload_data, payload_bytes ); }
    /** @copydoc rocketnet::ClientTransport::start_packet */
    uint8_t * start_packet( int request_bytes, struct rocketnet_packet_context_t * context )
    { return rocketnet_client_transport_start_packet( transport_, request_bytes, context ); }
    /** @copydoc rocketnet::ClientTransport::packet_payload */
    uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity )
    { return rocketnet_client_transport_packet_payload( transport_, context, capacity ); }
    /** @copydoc rocketnet::ClientTransport::finish_packet */
    int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes )
    { return rocketnet_client_transport_finish_packet( transport_, context, payload_bytes ); }
    /** @copydoc rocketnet::ClientTransport::abort_packet */
    void abort_packet( struct rocketnet_packet_context_t * context )
    { rocketnet_client_transport_abort_packet( transport_, context ); }
    /** @copydoc rocketnet::ClientTransport::send_packets */
    void send_packets() { rocketnet_client_transport_send_packets( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::state */
    int state() { return rocketnet_client_transport_state( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::driver */
    const char * driver() { return rocketnet_client_transport_driver( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::client_index */
    int client_index() { return rocketnet_client_transport_client_index( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::disconnect */
    void disconnect() { rocketnet_client_transport_disconnect( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::counters */
    const struct rocketnet_counters_t * counters() { return rocketnet_client_transport_counters( transport_ ); }
    /** @copydoc rocketnet::ClientTransport::stats */
    void stats( struct rocketnet_client_transport_stats_t * stats )
    { rocketnet_client_transport_stats( transport_, stats ); }
    /** @copydoc rocketnet::ClientTransport::acked_packets */
    int acked_packets( const uint64_t ** packet_ids )
    { return rocketnet_client_transport_acked_packets( transport_, packet_ids ); }

private:
    explicit PortableClientTransport( struct rocketnet_client_transport_t * c ) : transport_( c ) {}
    PortableClientTransport( const PortableClientTransport & );
    PortableClientTransport & operator=( const PortableClientTransport & );
    struct rocketnet_client_transport_t * transport_;
};

/** @brief Portable wrapper that owns and forwards to a server C transport. */
class PortableServerTransport : public ServerTransport
{
public:
    /** @brief Create a portable server transport.
     * @param[in] context Optional opaque caller-owned context passed to the C API; NULL is allowed and the value is not freed by the transport.
     * @param[in] public_address_string Public address and UDP port; NULL is refused by the C API.
     * @param[in] config Server configuration, or NULL for defaults.
     * @return New wrapper, or NULL if C creation fails; allocating the wrapper may throw std::bad_alloc.
     * @pre Call rocketnet_init before creating the C transport.
     * @note Stop the server and wait for its stopped state before destroying the wrapper.
     */
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

    /** @copydoc rocketnet::ServerTransport::stop */
    void stop() { rocketnet_server_transport_stop( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::state */
    int state() { return rocketnet_server_transport_state( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::port */
    uint16_t port() { return rocketnet_server_transport_port( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::driver */
    const char * driver() { return rocketnet_server_transport_driver( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::update */
    void update( double time ) { rocketnet_server_transport_update( transport_, time ); }
    /** @copydoc rocketnet::ServerTransport::set_admission_callback */
    void set_admission_callback( rocketnet_server_admission_callback_t callback, void * context )
    { rocketnet_server_transport_set_admission_callback( transport_, callback, context ); }
    /** @copydoc rocketnet::ServerTransport::num_payloads */
    int num_payloads() { return rocketnet_server_transport_num_payloads( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::payload */
    uint8_t * payload( int payload_index, int * client_index, int * payload_bytes )
    { return rocketnet_server_transport_payload( transport_, payload_index, client_index, payload_bytes ); }
    /** @copydoc rocketnet::ServerTransport::payloads */
    int payloads( const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
    { return rocketnet_server_transport_payloads( transport_, client_index, payload_data, payload_bytes ); }
    /** @copydoc rocketnet::ServerTransport::start_packet */
    uint8_t * start_packet( int client_index, int request_bytes, struct rocketnet_packet_context_t * context )
    { return rocketnet_server_transport_start_packet( transport_, client_index, request_bytes, context ); }
    /** @copydoc rocketnet::ServerTransport::packet_payload */
    uint8_t * packet_payload( const struct rocketnet_packet_context_t * context, int * capacity )
    { return rocketnet_server_transport_packet_payload( transport_, context, capacity ); }
    /** @copydoc rocketnet::ServerTransport::finish_packet */
    int finish_packet( struct rocketnet_packet_context_t * context, int payload_bytes )
    { return rocketnet_server_transport_finish_packet( transport_, context, payload_bytes ); }
    /** @copydoc rocketnet::ServerTransport::abort_packet */
    void abort_packet( struct rocketnet_packet_context_t * context )
    { rocketnet_server_transport_abort_packet( transport_, context ); }
    /** @copydoc rocketnet::ServerTransport::send_packets */
    void send_packets() { rocketnet_server_transport_send_packets( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::send_packet */
    void send_packet( int client_index, const uint8_t * payload_data, int payload_bytes )
    { rocketnet_server_transport_send_packet( transport_, client_index, payload_data, payload_bytes ); }
    /** @copydoc rocketnet::ServerTransport::disconnect_client */
    void disconnect_client( int client_index )
    { rocketnet_server_transport_disconnect_client( transport_, client_index ); }
    /** @copydoc rocketnet::ServerTransport::num_connected_clients */
    int num_connected_clients() { return rocketnet_server_transport_num_connected_clients( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::is_client_connected */
    int is_client_connected( int client_index )
    { return rocketnet_server_transport_is_client_connected( transport_, client_index ); }
    /** @copydoc rocketnet::ServerTransport::client_id */
    uint64_t client_id( int client_index )
    { return rocketnet_server_transport_client_id( transport_, client_index ); }
    /** @copydoc rocketnet::ServerTransport::counters */
    const struct rocketnet_counters_t * counters() { return rocketnet_server_transport_counters( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::max_payload_bytes */
    int max_payload_bytes() { return rocketnet_server_transport_max_payload_bytes( transport_ ); }
    /** @copydoc rocketnet::ServerTransport::stats */
    void stats( struct rocketnet_server_transport_stats_t * stats )
    { rocketnet_server_transport_stats( transport_, stats ); }
    /** @copydoc rocketnet::ServerTransport::client_stats */
    void client_stats( int client_index, struct rocketnet_server_transport_client_stats_t * stats )
    { rocketnet_server_transport_client_stats( transport_, client_index, stats ); }
    /** @copydoc rocketnet::ServerTransport::acked_packets */
    int acked_packets( const int ** client_index, const uint64_t ** packet_ids )
    { return rocketnet_server_transport_acked_packets( transport_, client_index, packet_ids ); }

private:
    explicit PortableServerTransport( struct rocketnet_server_transport_t * s ) : transport_( s ) {}
    PortableServerTransport( const PortableServerTransport & );
    PortableServerTransport & operator=( const PortableServerTransport & );
    struct rocketnet_server_transport_t * transport_;
};

} // namespace rocketnet

#endif // ROCKETNET_HPP
