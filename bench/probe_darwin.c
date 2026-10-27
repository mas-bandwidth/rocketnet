/*
    rocketnet transport probe: Darwin surface report (#100, the #81 doctrine)

    The macOS sibling of probe.c: every claim about Darwin's fast paths is
    a runtime attempt with the errno recorded, never an assumption. Run on
    the target machine; the report is the survey the transport choice cites.

    What it measures:

    1. sendmsg_x / recvmsg_x -- Darwin's private-but-stable batch UDP
       syscalls. Resolved by weak link AND dlsym (both recorded), then
       exercised for the shapes a server pump needs: unconnected socket,
       per-message msg_name on send, msg_name fill on receive, batch
       counts in and out, partial-batch semantics, and the msg_datalen
       receive-length contract.
    2. SO_REUSEPORT fan-out -- N sockets bound to one port, a spray of
       packets from distinct 5-tuples, the per-socket distribution
       reported. Linux hashes flows across the group; Darwin is measured
       here, not assumed.
    3. kqueue -- registration and wakeup on a UDP socket, and what the
       kevent data field reports.
    4. socket buffer reality -- what SO_RCVBUF/SO_SNDBUF grants against
       the request, plus the sysctls that cap them.

    Build and run on macOS only:  cc -O2 -o probe_darwin probe_darwin.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
    struct msghdr_x, from xnu bsd/sys/socket_private.h. Defined locally so
    this builds against any SDK; the layout is the syscall ABI. msg_datalen
    carries the received byte count back per message on recvmsg_x.
*/

struct msghdr_x
{
    void * msg_name;
    socklen_t msg_namelen;
    struct iovec * msg_iov;
    int msg_iovlen;
    void * msg_control;
    socklen_t msg_controllen;
    int msg_flags;
    size_t msg_datalen;
};

typedef ssize_t ( *sendmsg_x_fn )( int s, const struct msghdr_x * msgp, u_int cnt, int flags );
typedef ssize_t ( *recvmsg_x_fn )( int s, struct msghdr_x * msgp, u_int cnt, int flags );

/* the weak link half of the probe: if the export vanishes in a future
   macOS, these resolve to NULL instead of failing the load */

extern ssize_t sendmsg_x( int s, const struct msghdr_x * msgp, u_int cnt, int flags ) __attribute__( ( weak_import ) );
extern ssize_t recvmsg_x( int s, struct msghdr_x * msgp, u_int cnt, int flags ) __attribute__( ( weak_import ) );

#define PROBE_BATCH 8
#define PROBE_PACKET_BYTES 64

static int udp_socket_nonblocking( int port_out_fd_bound_port[1] )
{
    int fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( fd < 0 )
        return -1;
    struct sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    address.sin_port = 0;
    if ( bind( fd, (struct sockaddr *) &address, sizeof( address ) ) < 0 )
    {
        close( fd );
        return -1;
    }
    socklen_t len = sizeof( address );
    getsockname( fd, (struct sockaddr *) &address, &len );
    if ( port_out_fd_bound_port )
        port_out_fd_bound_port[0] = (int) ntohs( address.sin_port );
    fcntl( fd, F_SETFL, O_NONBLOCK );
    return fd;
}

static struct sockaddr_in loopback_address( int port )
{
    struct sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    address.sin_port = htons( (uint16_t) port );
    return address;
}

static void probe_msg_x( void )
{
    printf( "== sendmsg_x / recvmsg_x ==\n" );

    sendmsg_x_fn dlsym_send = (sendmsg_x_fn) dlsym( RTLD_DEFAULT, "sendmsg_x" );
    recvmsg_x_fn dlsym_recv = (recvmsg_x_fn) dlsym( RTLD_DEFAULT, "recvmsg_x" );

    printf( "weak link: sendmsg_x=%s recvmsg_x=%s\n", &sendmsg_x != NULL ? "present" : "NULL",
            &recvmsg_x != NULL ? "present" : "NULL" );
    printf( "dlsym:     sendmsg_x=%s recvmsg_x=%s\n", dlsym_send ? "present" : "NULL",
            dlsym_recv ? "present" : "NULL" );

    if ( !dlsym_send || !dlsym_recv )
    {
        printf( "sendmsg_x/recvmsg_x absent: nothing further to measure\n\n" );
        return;
    }

    int recv_port = 0;
    int recv_fd = udp_socket_nonblocking( &recv_port );
    int send_port = 0;
    int send_fd = udp_socket_nonblocking( &send_port );
    if ( recv_fd < 0 || send_fd < 0 )
    {
        printf( "probe socket setup failed: %s\n\n", strerror( errno ) );
        return;
    }

    struct sockaddr_in to = loopback_address( recv_port );

    /* shape 1, the server pump's send: UNCONNECTED socket, per-message
       msg_name. this is the shape that matters -- a server sends one batch
       across many clients */

    uint8_t send_payload[PROBE_BATCH][PROBE_PACKET_BYTES];
    struct iovec send_iov[PROBE_BATCH];
    struct msghdr_x send_msg[PROBE_BATCH];
    memset( send_msg, 0, sizeof( send_msg ) );
    for ( int i = 0; i < PROBE_BATCH; i++ )
    {
        memset( send_payload[i], (int) 'a' + i, PROBE_PACKET_BYTES );
        send_iov[i].iov_base = send_payload[i];
        send_iov[i].iov_len = PROBE_PACKET_BYTES;
        send_msg[i].msg_name = &to;
        send_msg[i].msg_namelen = sizeof( to );
        send_msg[i].msg_iov = &send_iov[i];
        send_msg[i].msg_iovlen = 1;
    }

    errno = 0;
    ssize_t sent = dlsym_send( send_fd, send_msg, PROBE_BATCH, 0 );
    printf( "sendmsg_x unconnected + per-message msg_name: sent=%zd errno=%d (%s)\n",
            sent, sent < 0 ? errno : 0, sent < 0 ? strerror( errno ) : "ok" );

    /* shape 2: CONNECTED socket, msg_name NULL -- the fallback shape if
       shape 1 is refused */

    int connected_fd = udp_socket_nonblocking( NULL );
    connect( connected_fd, (struct sockaddr *) &to, sizeof( to ) );
    struct msghdr_x connected_msg[PROBE_BATCH];
    memcpy( connected_msg, send_msg, sizeof( connected_msg ) );
    for ( int i = 0; i < PROBE_BATCH; i++ )
    {
        connected_msg[i].msg_name = NULL;
        connected_msg[i].msg_namelen = 0;
    }
    errno = 0;
    ssize_t sent_connected = dlsym_send( connected_fd, connected_msg, PROBE_BATCH, 0 );
    printf( "sendmsg_x connected + msg_name NULL:          sent=%zd errno=%d (%s)\n",
            sent_connected, sent_connected < 0 ? errno : 0, sent_connected < 0 ? strerror( errno ) : "ok" );

    usleep( 50000 );

    /* the receive shape: unconnected socket, msg_name buffers per slot,
       batch of PROBE_BATCH against however many actually arrived. record
       the count, each message's msg_datalen, whether msg_name filled, and
       whether payload bytes are intact */

    struct sockaddr_in from[PROBE_BATCH];
    uint8_t recv_payload[PROBE_BATCH][PROBE_PACKET_BYTES * 2];
    struct iovec recv_iov[PROBE_BATCH];
    struct msghdr_x recv_msg[PROBE_BATCH];
    memset( recv_msg, 0, sizeof( recv_msg ) );
    memset( from, 0, sizeof( from ) );
    for ( int i = 0; i < PROBE_BATCH; i++ )
    {
        recv_iov[i].iov_base = recv_payload[i];
        recv_iov[i].iov_len = sizeof( recv_payload[i] );
        recv_msg[i].msg_name = &from[i];
        recv_msg[i].msg_namelen = sizeof( from[i] );
        recv_msg[i].msg_iov = &recv_iov[i];
        recv_msg[i].msg_iovlen = 1;
    }

    errno = 0;
    ssize_t received = dlsym_recv( recv_fd, recv_msg, PROBE_BATCH, 0 );
    printf( "recvmsg_x unconnected batch:                  received=%zd errno=%d (%s)\n",
            received, received < 0 ? errno : 0, received < 0 ? strerror( errno ) : "ok" );

    int name_filled = 0;
    int datalen_ok = 0;
    int payload_ok = 0;
    for ( ssize_t i = 0; i < ( received > 0 ? received : 0 ); i++ )
    {
        if ( from[i].sin_port != 0 )
            name_filled++;
        if ( recv_msg[i].msg_datalen == PROBE_PACKET_BYTES )
            datalen_ok++;
        int intact = 1;
        for ( int j = 0; j < PROBE_PACKET_BYTES; j++ )
            if ( recv_payload[i][j] != (uint8_t) ( 'a' + i ) )
            {
                intact = 0;
                break;
            }
        if ( intact )
            payload_ok++;
    }
    if ( received > 0 )
        printf( "recvmsg_x contract: msg_name filled %d/%zd, msg_datalen==%d %d/%zd, payload intact %d/%zd\n",
                name_filled, received, PROBE_PACKET_BYTES, datalen_ok, received, payload_ok, received );

    /* empty-socket semantics: what a drained nonblocking recvmsg_x returns.
       the connected-socket shape above queued its own 8 packets onto the
       receive socket, so drain everything first -- the first run of this
       probe read those leftovers back and taught the lesson */

    uint8_t sink[256];
    while ( recv( recv_fd, sink, sizeof( sink ), 0 ) > 0 )
    {
    }

    errno = 0;
    ssize_t empty = dlsym_recv( recv_fd, recv_msg, PROBE_BATCH, 0 );
    printf( "recvmsg_x on drained socket:                  received=%zd errno=%d (%s)\n",
            empty, empty < 0 ? errno : 0, empty < 0 ? strerror( errno ) : "ok" );

    /* partial batch: send 3, ask for PROBE_BATCH -- does the syscall return
       the 3 without blocking? */

    for ( int i = 0; i < 3; i++ )
        sendto( send_fd, send_payload[i], PROBE_PACKET_BYTES, 0, (struct sockaddr *) &to, sizeof( to ) );
    usleep( 50000 );
    for ( int i = 0; i < PROBE_BATCH; i++ )
        recv_msg[i].msg_namelen = sizeof( from[i] );
    errno = 0;
    ssize_t partial = dlsym_recv( recv_fd, recv_msg, PROBE_BATCH, 0 );
    printf( "recvmsg_x partial (3 queued, %d asked):        received=%zd errno=%d (%s)\n",
            PROBE_BATCH, partial, partial < 0 ? errno : 0, partial < 0 ? strerror( errno ) : "ok" );

    close( recv_fd );
    close( send_fd );
    close( connected_fd );
    printf( "\n" );
}

/*
    SO_REUSEPORT fan-out: NUM_WORKERS sockets bound to one port, REUSEPORT
    set before bind on every one, then a spray from many distinct source
    ports (distinct 5-tuples -- what a hash would fan out on). The
    distribution IS the finding.
*/

#define REUSEPORT_WORKERS 4
#define REUSEPORT_SENDERS 32
#define REUSEPORT_PACKETS_PER_SENDER 250

static void probe_reuseport( void )
{
    printf( "== SO_REUSEPORT fan-out (%d workers, %d source tuples, %d packets) ==\n",
            REUSEPORT_WORKERS, REUSEPORT_SENDERS, REUSEPORT_SENDERS * REUSEPORT_PACKETS_PER_SENDER );

    int workers[REUSEPORT_WORKERS];
    int port = 0;

    for ( int i = 0; i < REUSEPORT_WORKERS; i++ )
    {
        workers[i] = socket( AF_INET, SOCK_DGRAM, 0 );
        int yes = 1;
        if ( setsockopt( workers[i], SOL_SOCKET, SO_REUSEPORT, &yes, sizeof( yes ) ) != 0 )
        {
            printf( "SO_REUSEPORT refused on socket %d: errno=%d (%s)\n", i, errno, strerror( errno ) );
            for ( int j = 0; j <= i; j++ )
                close( workers[j] );
            return;
        }
        struct sockaddr_in address = loopback_address( port );
        if ( bind( workers[i], (struct sockaddr *) &address, sizeof( address ) ) < 0 )
        {
            printf( "bind refused on reuseport socket %d: errno=%d (%s)\n", i, errno, strerror( errno ) );
            for ( int j = 0; j <= i; j++ )
                close( workers[j] );
            return;
        }
        if ( port == 0 )
        {
            socklen_t len = sizeof( address );
            getsockname( workers[i], (struct sockaddr *) &address, &len );
            port = (int) ntohs( address.sin_port );
        }
        fcntl( workers[i], F_SETFL, O_NONBLOCK );
    }

    struct sockaddr_in to = loopback_address( port );
    uint8_t packet[32];
    memset( packet, 0xab, sizeof( packet ) );

    for ( int s = 0; s < REUSEPORT_SENDERS; s++ )
    {
        int fd = udp_socket_nonblocking( NULL );
        for ( int p = 0; p < REUSEPORT_PACKETS_PER_SENDER; p++ )
        {
            if ( sendto( fd, packet, sizeof( packet ), 0, (struct sockaddr *) &to, sizeof( to ) ) < 0 )
                usleep( 1000 );
        }
        close( fd );
    }

    usleep( 200000 );

    uint64_t counts[REUSEPORT_WORKERS];
    uint64_t total = 0;
    for ( int i = 0; i < REUSEPORT_WORKERS; i++ )
    {
        counts[i] = 0;
        uint8_t sink[64];
        while ( recv( workers[i], sink, sizeof( sink ), 0 ) > 0 )
            counts[i]++;
        total += counts[i];
        close( workers[i] );
    }

    printf( "received %llu of %d\n", (unsigned long long) total, REUSEPORT_SENDERS * REUSEPORT_PACKETS_PER_SENDER );
    for ( int i = 0; i < REUSEPORT_WORKERS; i++ )
        printf( "worker %d (bind order %d): %llu packets (%.1f%%)\n", i, i, (unsigned long long) counts[i],
                total ? 100.0 * (double) counts[i] / (double) total : 0.0 );
    printf( "\n" );
}

static void probe_kqueue( void )
{
    printf( "== kqueue on UDP ==\n" );

    int kq = kqueue();
    if ( kq < 0 )
    {
        printf( "kqueue() failed: errno=%d (%s)\n\n", errno, strerror( errno ) );
        return;
    }

    int port = 0;
    int fd = udp_socket_nonblocking( &port );
    int sender = udp_socket_nonblocking( NULL );

    struct kevent change;
    EV_SET( &change, fd, EVFILT_READ, EV_ADD, 0, 0, NULL );
    if ( kevent( kq, &change, 1, NULL, 0, NULL ) < 0 )
    {
        printf( "kevent EV_ADD failed: errno=%d (%s)\n\n", errno, strerror( errno ) );
        close( sender );
        close( fd );
        close( kq );
        return;
    }

    struct sockaddr_in to = loopback_address( port );
    uint8_t packet[100];
    memset( packet, 0xcd, sizeof( packet ) );
    for ( int i = 0; i < 3; i++ )
        sendto( sender, packet, sizeof( packet ), 0, (struct sockaddr *) &to, sizeof( to ) );
    usleep( 50000 );

    struct kevent event;
    struct timespec timeout = { 1, 0 };
    int n = kevent( kq, NULL, 0, &event, 1, &timeout );
    printf( "kevent after 3x100b queued: n=%d filter=%s data=%lld (socket buffer bytes pending)\n",
            n, n > 0 && event.filter == EVFILT_READ ? "EVFILT_READ" : "?", n > 0 ? (long long) event.data : 0 );

    close( sender );
    close( fd );
    close( kq );
    printf( "\n" );
}

static void probe_buffers( void )
{
    printf( "== socket buffer reality ==\n" );

    int64_t maxsockbuf = 0;
    size_t len = sizeof( maxsockbuf );
    sysctlbyname( "kern.ipc.maxsockbuf", &maxsockbuf, &len, NULL, 0 );
    int32_t recvspace = 0;
    len = sizeof( recvspace );
    sysctlbyname( "net.inet.udp.recvspace", &recvspace, &len, NULL, 0 );
    printf( "kern.ipc.maxsockbuf=%lld net.inet.udp.recvspace=%d\n", (long long) maxsockbuf, recvspace );

    int fd = socket( AF_INET, SOCK_DGRAM, 0 );
    static const int requests[] = { 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024 };
    for ( int i = 0; i < 3; i++ )
    {
        int request = requests[i];
        int rcv_result = setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &request, sizeof( request ) );
        int granted = 0;
        socklen_t glen = sizeof( granted );
        getsockopt( fd, SOL_SOCKET, SO_RCVBUF, &granted, &glen );
        printf( "SO_RCVBUF request %d: setsockopt=%s granted=%d\n", request, rcv_result == 0 ? "ok" : strerror( errno ), granted );
    }
    close( fd );
    printf( "\n" );
}

int main( void )
{
    printf( "rocketnet darwin probe (#100)\n" );

    char osversion[64] = "?";
    size_t len = sizeof( osversion );
    sysctlbyname( "kern.osversion", osversion, &len, NULL, 0 );
    char machine[64] = "?";
    len = sizeof( machine );
    sysctlbyname( "hw.machine", machine, &len, NULL, 0 );
    char model[64] = "?";
    len = sizeof( model );
    sysctlbyname( "hw.model", model, &len, NULL, 0 );
    int ncpu = 0;
    len = sizeof( ncpu );
    sysctlbyname( "hw.ncpu", &ncpu, &len, NULL, 0 );
    printf( "host: %s %s ncpu=%d osversion=%s\n\n", model, machine, ncpu, osversion );

    probe_msg_x();
    probe_reuseport();
    probe_kqueue();
    probe_buffers();

    return 0;
}
