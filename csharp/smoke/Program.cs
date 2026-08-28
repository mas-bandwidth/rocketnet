/*
    The C# binding smoke test: a loopback client/server over the portable
    backend, driven end to end through the binding. Connects, exchanges
    payloads both directions through the acquire/commit path and the
    convenience sends, verifies byte-exact delivery, observes a deliberate
    server-side disconnect, and runs the negative controls: a corrupted
    connect token must refuse, an oversize finish must drop, and a token
    with a corrupted sealed block must be refused by the server's signature
    check. Exit code 0 on success, 1 with a failure line otherwise.

    Time is the caller's: a virtual clock advanced in fixed 10ms steps.
    The library reads wall clock only for token expiry, by design.

    Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0 (see LICENSE).
*/

using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;

static class Smoke
{
    const double TimeStep = 0.01;

    static double time = 100.0;

    static void Fail( string message )
    {
        Console.WriteLine( $"FAIL: {message}" );
        Environment.Exit( 1 );
    }

    static void Ok( string message )
    {
        Console.WriteLine( $"ok: {message}" );
    }

    static byte Pattern( int seed, int i ) => (byte) ( seed * 151 + i * 7 + 3 );

    static void FillPattern( Span<byte> span, int seed )
    {
        for ( int i = 0; i < span.Length; i++ )
            span[i] = Pattern( seed, i );
    }

    static bool MatchesPattern( ReadOnlySpan<byte> span, int seed )
    {
        for ( int i = 0; i < span.Length; i++ )
            if ( span[i] != Pattern( seed, i ) )
                return false;
        return true;
    }

    static int FreePort()
    {
        using Socket socket = new Socket( AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp );
        socket.Bind( new IPEndPoint( IPAddress.Loopback, 0 ) );
        return ( (IPEndPoint) socket.LocalEndPoint! ).Port;
    }

    // one pump iteration: advance the clock, update both endpoints, flush the server
    static void Pump( IntPtr client, IntPtr server )
    {
        time += TimeStep;
        if ( client != IntPtr.Zero )
            RocketNet.ClientTransportUpdate( client, time );
        RocketNet.ServerTransportUpdate( server, time );
        RocketNet.ServerTransportSendPackets( server );
        Thread.Sleep( 1 );
    }

    static int Main()
    {
        if ( !RocketNet.Init() )
            Fail( "rocketnet_init" );
        RocketNet.LogLevel( RocketNet.LogLevelError );
        Ok( $"init (protocol version {RocketNet.ProtocolVersion():x}, native time {RocketNet.Time():F2}, user id {RocketNet.UserIdString( 0x1234567812345678UL )})" );

        int port = FreePort();
        string address = $"127.0.0.1:{port}";

        RocketNet.ServerTransportDevConfig( out RocketNet.ServerTransportConfig serverConfig );
        serverConfig.MaxClientSlots = 4;
        IntPtr server = RocketNet.ServerTransportCreate( IntPtr.Zero, address, ref serverConfig, time );
        if ( server == IntPtr.Zero )
            Fail( $"server create on {address}" );
        Ok( $"server up on {address}" );

        // connect with an explicitly defaulted config: the config struct layout
        // crosses the interop boundary here — a wrong filter value would leave
        // every packet prefiltered on the floor

        ulong clientId = RocketNet.RandomUint64();
        byte[] token = new byte[RocketNet.ConnectTokenBytes];
        ulong unixNow = (ulong) DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        if ( RocketNet.WriteDevConnectToken( address, clientId, unixNow, token ) == 0 )
            Fail( "write dev connect token" );

        RocketNet.ClientTransportDefaultConfig( out RocketNet.ClientTransportConfig clientConfig );
        IntPtr client = RocketNet.ClientTransportCreate( IntPtr.Zero, token, ref clientConfig, time );
        if ( client == IntPtr.Zero )
            Fail( "client create" );

        int iterations = 0;
        while ( RocketNet.ClientTransportState( client ) != RocketNet.ClientTransportStateConnected && iterations < 2000 )
        {
            Pump( client, server );
            iterations++;
        }
        if ( RocketNet.ClientTransportState( client ) != RocketNet.ClientTransportStateConnected )
            Fail( $"client did not connect: state '{RocketNet.ClientTransportStateName( RocketNet.ClientTransportState( client ) )}'" );
        if ( RocketNet.ClientTransportStateName( RocketNet.ClientTransportState( client ) ) != "connected" )
            Fail( "client state name" );
        if ( RocketNet.ServerTransportNumConnectedClients( server ) != 1 )
            Fail( "server connected count" );
        int clientIndex = RocketNet.ClientTransportClientIndex( client );
        if ( RocketNet.ServerTransportIsClientConnected( server, clientIndex ) == 0 )
            Fail( "server is_client_connected" );
        if ( RocketNet.ServerTransportClientId( server, clientIndex ) != clientId )
            Fail( "server client id" );
        Ok( $"connected in {iterations} iterations (client index {clientIndex})" );

        // client -> server: one payload through acquire/commit, one through the
        // convenience send

        RocketNet.PacketContext context = default;
        if ( RocketNet.ClientTransportStartPacket( client, ref context ) == IntPtr.Zero )
            Fail( "client start packet" );
        Span<byte> payload = RocketNet.PayloadSpan( in context );
        if ( payload.Length < 256 || payload.Length % 8 != 0 )
            Fail( $"payload capacity {payload.Length}" );
        FillPattern( payload.Slice( 0, 256 ), 1 );
        if ( RocketNet.ClientTransportFinishPacket( client, ref context, 256 ) == 0 )
            Fail( "client finish packet" );
        RocketNet.ClientTransportSendPackets( client );

        byte[] convenience = new byte[64];
        FillPattern( convenience, 2 );
        RocketNet.ClientTransportSendPacket( client, convenience );

        // the oversize finish is dropped with its sequence burned, never sent

        RocketNet.PacketContext oversize = default;
        if ( RocketNet.ClientTransportStartPacket( client, ref oversize ) == IntPtr.Zero )
            Fail( "client start packet (oversize)" );
        if ( RocketNet.ClientTransportFinishPacket( client, ref oversize, oversize.PayloadCapacity + 1 ) != 0 )
            Fail( "oversize finish was not dropped" );
        Ok( "oversize finish_packet dropped (returned 0)" );

        bool got256 = false, got64 = false;
        for ( int i = 0; i < 1000 && !( got256 && got64 ); i++ )
        {
            time += TimeStep;
            RocketNet.ClientTransportUpdate( client, time );
            RocketNet.ServerTransportUpdate( server, time );
            RocketNet.ServerTransportPayloadBatch batch = RocketNet.ServerTransportPayloads( server );
            for ( int j = 0; j < batch.Count; j++ )
            {
                if ( batch.ClientIndex( j ) != clientIndex )
                    Fail( "payload from wrong client index" );
                ReadOnlySpan<byte> received = batch[j];
                if ( received.Length == 256 && MatchesPattern( received, 1 ) )
                    got256 = true;
                else if ( received.Length == 64 && MatchesPattern( received, 2 ) )
                    got64 = true;
                else
                    Fail( $"unexpected payload at server: {received.Length} bytes" );
            }
            RocketNet.ServerTransportSendPackets( server );
            Thread.Sleep( 1 );
        }
        if ( !got256 || !got64 )
            Fail( $"server did not receive both payloads (acquire/commit {got256}, convenience {got64})" );
        Ok( "client -> server: acquire/commit and convenience payloads byte-exact" );

        // server -> client: one payload through the server's acquire/commit path
        // (the 256-byte pattern sent back) and one through the convenience send.
        // Both are staged only now, after the drain above: a payload the client
        // receives while another loop is pumping updates is gone unobserved, so
        // everything in flight toward the client must be drained by the loop
        // that expects it. Drained with both receive forms — the indexed single
        // accessor and the batch view.

        RocketNet.PacketContext down = default;
        if ( RocketNet.ServerTransportStartPacket( server, clientIndex, ref down ) == IntPtr.Zero )
            Fail( "server start packet" );
        FillPattern( RocketNet.PayloadSpan( in down ).Slice( 0, 256 ), 1 );
        if ( RocketNet.ServerTransportFinishPacket( server, ref down, 256 ) == 0 )
            Fail( "server finish packet" );
        RocketNet.ServerTransportSendPacket( server, clientIndex, new ReadOnlySpan<byte>( ConvenienceDown() ) );

        bool gotEcho = false, gotDown = false;
        for ( int i = 0; i < 1000 && !( gotEcho && gotDown ); i++ )
        {
            Pump( client, server );
            int num = RocketNet.ClientTransportNumPayloads( client );
            RocketNet.ClientTransportPayloadBatch batch = RocketNet.ClientTransportPayloads( client );
            if ( batch.Count != num )
                Fail( "client payload count mismatch between accessors" );
            for ( int j = 0; j < batch.Count; j++ )
            {
                ReadOnlySpan<byte> received = batch[j];
                if ( !received.SequenceEqual( RocketNet.ClientTransportPayloadSpan( client, j ) ) )
                    Fail( "client payload mismatch between accessors" );
                if ( received.Length == 256 && MatchesPattern( received, 1 ) )
                    gotEcho = true;
                else if ( received.Length == 32 && MatchesPattern( received, 3 ) )
                    gotDown = true;
                else
                    Fail( $"unexpected payload at client: {received.Length} bytes" );
            }
        }
        if ( !gotEcho || !gotDown )
            Fail( $"client did not receive both payloads (acquire/commit {gotEcho}, convenience {gotDown})" );
        Ok( "server -> client: acquire/commit and convenience payloads byte-exact" );

        // quality stats: the structs cross the interop boundary here, and the
        // numbers must be plausible after live traffic — rtt sampled from the
        // handshake, rates and bandwidth nonzero over the trailing second of
        // the virtual clock, zero loss on a clean loopback

        RocketNet.ClientTransportGetStats( client, out RocketNet.ClientTransportStats clientStats );
        if ( clientStats.Rtt <= 0.0f || clientStats.Rtt >= 1000.0f )
            Fail( $"client rtt implausible ({clientStats.Rtt} ms)" );
        if ( clientStats.Jitter < 0.0f )
            Fail( $"client jitter implausible ({clientStats.Jitter} ms)" );
        if ( clientStats.PacketLoss != 0.0f )
            Fail( $"client packet loss on a clean loopback ({clientStats.PacketLoss}%)" );
        if ( clientStats.PacketsSentPerSecond <= 0 || clientStats.PacketsReceivedPerSecond <= 0 )
            Fail( $"client packet rates implausible ({clientStats.PacketsSentPerSecond} sent, {clientStats.PacketsReceivedPerSecond} received)" );
        if ( clientStats.BandwidthSentKbps <= 0 || clientStats.BandwidthReceivedKbps <= 0 )
            Fail( $"client bandwidth implausible ({clientStats.BandwidthSentKbps} up, {clientStats.BandwidthReceivedKbps} down)" );

        RocketNet.ServerTransportGetStats( server, out RocketNet.ServerTransportStats serverStats );
        if ( serverStats.PacketsSentPerSecond == 0 || serverStats.PacketsReceivedPerSecond == 0 )
            Fail( $"server packet rates implausible ({serverStats.PacketsSentPerSecond} sent, {serverStats.PacketsReceivedPerSecond} received)" );
        if ( serverStats.BandwidthSentKbps <= 0.0f || serverStats.BandwidthReceivedKbps <= 0.0f )
            Fail( $"server bandwidth implausible ({serverStats.BandwidthSentKbps} up, {serverStats.BandwidthReceivedKbps} down)" );
        Ok( $"quality stats: client rtt {clientStats.Rtt:F1} ms, jitter {clientStats.Jitter:F2} ms, loss {clientStats.PacketLoss:F1}%, " +
            $"client {clientStats.PacketsSentPerSecond}/{clientStats.PacketsReceivedPerSecond} pps, server {serverStats.PacketsSentPerSecond}/{serverStats.PacketsReceivedPerSecond} pps" );

        // the delivery notification primitive: one payload each way through
        // acquire/commit, and the packet id start_packet handed out comes back
        // acknowledged on each end -- the join key, through the binding

        RocketNet.PacketContext ackedUp = default;
        if ( RocketNet.ClientTransportStartPacket( client, ref ackedUp ) == IntPtr.Zero )
            Fail( "client start packet (acked)" );
        FillPattern( RocketNet.PayloadSpan( in ackedUp ).Slice( 0, 16 ), 4 );
        if ( RocketNet.ClientTransportFinishPacket( client, ref ackedUp, 16 ) == 0 )
            Fail( "client finish packet (acked)" );
        RocketNet.ClientTransportSendPackets( client );

        RocketNet.PacketContext ackedDown = default;
        if ( RocketNet.ServerTransportStartPacket( server, clientIndex, ref ackedDown ) == IntPtr.Zero )
            Fail( "server start packet (acked)" );
        FillPattern( RocketNet.PayloadSpan( in ackedDown ).Slice( 0, 16 ), 5 );
        if ( RocketNet.ServerTransportFinishPacket( server, ref ackedDown, 16 ) == 0 )
            Fail( "server finish packet (acked)" );

        bool clientSawAck = false, serverSawAck = false;
        for ( int i = 0; i < 2000 && !( clientSawAck && serverSawAck ); i++ )
        {
            Pump( client, server );

            ReadOnlySpan<ulong> clientAcked = RocketNet.ClientTransportAckedPacketsSpan( client );
            for ( int j = 0; j < clientAcked.Length; j++ )
                if ( clientAcked[j] == ackedUp.PacketId )
                    clientSawAck = true;

            RocketNet.ServerTransportAckedBatch serverAcked = RocketNet.ServerTransportAckedPackets( server );
            for ( int j = 0; j < serverAcked.Count; j++ )
            {
                if ( serverAcked.PacketId( j ) == ackedDown.PacketId )
                {
                    if ( serverAcked.ClientIndex( j ) != clientIndex )
                        Fail( "acked packet attributed to the wrong client index" );
                    serverSawAck = true;
                }
            }
        }
        if ( !clientSawAck || !serverSawAck )
            Fail( $"delivery notifications did not surface (client {clientSawAck}, server {serverSawAck})" );
        Ok( $"acked packet ids surfaced both ways (client id {ackedUp.PacketId}, server id {ackedDown.PacketId})" );

        // the per-client stats: fed by the acks, recomputed round-robin over
        // the slots -- after live traffic and enough updates the slot's
        // numbers have moved

        RocketNet.ServerTransportGetClientStats( server, clientIndex, out RocketNet.ServerTransportClientStats slotStats );
        if ( slotStats.BandwidthReceivedKbps <= 0.0f )
            Fail( $"per-client received bandwidth did not move ({slotStats.BandwidthReceivedKbps} kbps)" );
        if ( slotStats.Rtt < 0.0f || slotStats.Rtt >= 1000.0f )
            Fail( $"per-client rtt implausible ({slotStats.Rtt} ms)" );
        if ( slotStats.PacketLoss < 0.0f || slotStats.PacketLoss > 100.0f )
            Fail( $"per-client packet loss implausible ({slotStats.PacketLoss}%)" );
        RocketNet.ServerTransportGetClientStats( server, 12345, out RocketNet.ServerTransportClientStats bogusStats );
        if ( bogusStats.Rtt != 0.0f || bogusStats.BandwidthReceivedKbps != 0.0f )
            Fail( "out-of-range slot stats not zero" );
        Ok( $"per-client stats: rtt {slotStats.Rtt:F1} ms, loss {slotStats.PacketLoss:F1}%, received {slotStats.BandwidthReceivedKbps:F1} kbps, acked {slotStats.BandwidthAckedKbps:F1} kbps" );

        // counters: delivery counted, and the C# counter indices line up with the
        // C names at both ends of the enum

        ReadOnlySpan<ulong> serverCounters = RocketNet.CountersSpan( RocketNet.ServerTransportCounters( server ) );
        if ( serverCounters[(int) RocketNet.Counter.PayloadDelivered] < 2 )
            Fail( "server payload delivered counter" );
        if ( RocketNet.CounterName( RocketNet.Counter.ConnectionRequestSignatureDidNotVerify ) != "CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY" )
            Fail( "counter name mapping (signature)" );
        if ( RocketNet.CounterName( RocketNet.Counter.AckUnknownSequence ) != "ACK_UNKNOWN_SEQUENCE" )
            Fail( "counter name mapping (ack side counter)" );
        if ( RocketNet.CounterName( RocketNet.Counter.ClientAckWrongOpenedSize ) != "CLIENT_ACK_WRONG_OPENED_SIZE" )
            Fail( "counter name mapping (last counter)" );
        Ok( $"counters: {serverCounters[(int) RocketNet.Counter.PayloadDelivered]} payloads delivered at the server" );

        // the deliberate server-side disconnect, observed by the client

        RocketNet.ServerTransportDisconnectClient( server, clientIndex );
        for ( int i = 0; i < 1000 && RocketNet.ClientTransportState( client ) > RocketNet.ClientTransportStateDisconnected; i++ )
            Pump( client, server );
        if ( RocketNet.ClientTransportState( client ) > RocketNet.ClientTransportStateDisconnected )
            Fail( "client did not observe server disconnect" );
        if ( RocketNet.ServerTransportNumConnectedClients( server ) != 0 )
            Fail( "server still counts the disconnected client" );
        Ok( $"server disconnect observed by client (state '{RocketNet.ClientTransportStateName( RocketNet.ClientTransportState( client ) )}')" );
        RocketNet.ClientTransportDestroy( client );

        // negative control 1: one corrupted byte in the clear signed fields
        // (byte 0 is the version) and the client refuses the token at create

        byte[] badToken = new byte[RocketNet.ConnectTokenBytes];
        if ( RocketNet.WriteDevConnectToken( address, RocketNet.RandomUint64(), unixNow, badToken ) == 0 )
            Fail( "write dev connect token (negative 1)" );
        badToken[0] ^= 0xFF;
        RocketNet.Quiet( true );        // the refusal logs an error by design; keep the receipt clean
        IntPtr badClient = RocketNet.ClientTransportCreate( IntPtr.Zero, badToken, IntPtr.Zero, time );
        RocketNet.Quiet( false );
        if ( badClient != IntPtr.Zero )
            Fail( "corrupted token (version byte) was accepted at client create" );
        Ok( "negative control: corrupted token byte 0 refused at create (invalid connect token)" );

        // negative control 2: one corrupted byte inside the sealed block and the
        // server's signature check refuses the connection request; the client
        // must never reach connected

        byte[] sealedBad = new byte[RocketNet.ConnectTokenBytes];
        if ( RocketNet.WriteDevConnectToken( address, RocketNet.RandomUint64(), unixNow, sealedBad ) == 0 )
            Fail( "write dev connect token (negative 2)" );
        sealedBad[100] ^= 0xFF;         // inside the sealed private block, under the signature
        IntPtr sealedClient = RocketNet.ClientTransportCreate( IntPtr.Zero, sealedBad, IntPtr.Zero, time );
        if ( sealedClient == IntPtr.Zero )
            Fail( "sealed-block corruption should pass the client's local checks" );
        ulong refusals = 0;
        for ( int i = 0; i < 1000 && refusals == 0; i++ )
        {
            Pump( sealedClient, server );
            if ( RocketNet.ClientTransportState( sealedClient ) == RocketNet.ClientTransportStateConnected )
                Fail( "client connected with a corrupted sealed block" );
            refusals = RocketNet.CountersSpan( RocketNet.ServerTransportCounters( server ) )[(int) RocketNet.Counter.ConnectionRequestSignatureDidNotVerify];
        }
        if ( refusals == 0 )
            Fail( "server never refused the corrupted signature" );
        Ok( $"negative control: sealed-block corruption refused by server signature check ({refusals} refusal(s)), client state '{RocketNet.ClientTransportStateName( RocketNet.ClientTransportState( sealedClient ) )}'" );
        RocketNet.ClientTransportDisconnect( sealedClient );
        RocketNet.ClientTransportDestroy( sealedClient );

        // frame sync: three real-time waits at 60fps and sane stats

        IntPtr frameSync = RocketNet.FrameSyncCreate( IntPtr.Zero, 60.0 );
        if ( frameSync == IntPtr.Zero )
            Fail( "frame sync create" );
        for ( int i = 0; i < 3; i++ )
            RocketNet.FrameSyncWait( frameSync );
        RocketNet.FrameSyncGetStats( frameSync, out RocketNet.FrameSyncStats stats );
        if ( stats.Frame < 1 || stats.GuardSeconds < 0.0 || stats.GuardSeconds > 1.0 )
            Fail( $"frame sync stats implausible (frame {stats.Frame}, guard {stats.GuardSeconds})" );
        RocketNet.FrameSyncDestroy( frameSync );
        Ok( $"frame sync: {stats.Frame} frames, {stats.DroppedFrames} dropped, priority {stats.PriorityHeld}, spin {stats.Spinning}" );

        // stop the server and shut down

        RocketNet.ServerTransportStop( server );
        for ( int i = 0; i < 1000 && RocketNet.ServerTransportState( server ) != RocketNet.ServerTransportStopped; i++ )
            Pump( IntPtr.Zero, server );
        if ( RocketNet.ServerTransportState( server ) != RocketNet.ServerTransportStopped )
            Fail( "server did not stop" );
        RocketNet.ServerTransportDestroy( server );
        RocketNet.Term();
        Ok( "smoke passed" );
        return 0;
    }

    static byte[] ConvenienceDown()
    {
        byte[] data = new byte[32];
        FillPattern( data, 3 );
        return data;
    }
}
