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
            RocketNet.ClientUpdate( client, time );
        RocketNet.ServerUpdate( server, time );
        RocketNet.ServerSendPackets( server );
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

        RocketNet.ServerDevConfig( out RocketNet.ServerConfig serverConfig );
        serverConfig.MaxClientSlots = 4;
        IntPtr server = RocketNet.ServerCreate( IntPtr.Zero, address, ref serverConfig, time );
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

        RocketNet.ClientDefaultConfig( out RocketNet.ClientConfig clientConfig );
        IntPtr client = RocketNet.ClientCreate( IntPtr.Zero, token, ref clientConfig, time );
        if ( client == IntPtr.Zero )
            Fail( "client create" );

        int iterations = 0;
        while ( RocketNet.ClientState( client ) != RocketNet.ClientStateConnected && iterations < 2000 )
        {
            Pump( client, server );
            iterations++;
        }
        if ( RocketNet.ClientState( client ) != RocketNet.ClientStateConnected )
            Fail( $"client did not connect: state '{RocketNet.ClientStateName( RocketNet.ClientState( client ) )}'" );
        if ( RocketNet.ClientStateName( RocketNet.ClientState( client ) ) != "connected" )
            Fail( "client state name" );
        if ( RocketNet.ServerNumConnectedClients( server ) != 1 )
            Fail( "server connected count" );
        int clientIndex = RocketNet.ClientClientIndex( client );
        if ( RocketNet.ServerIsClientConnected( server, clientIndex ) == 0 )
            Fail( "server is_client_connected" );
        if ( RocketNet.ServerClientId( server, clientIndex ) != clientId )
            Fail( "server client id" );
        Ok( $"connected in {iterations} iterations (client index {clientIndex})" );

        // client -> server: one payload through acquire/commit, one through the
        // convenience send

        RocketNet.PacketContext context = default;
        if ( RocketNet.ClientStartPacket( client, ref context ) == IntPtr.Zero )
            Fail( "client start packet" );
        Span<byte> payload = RocketNet.PayloadSpan( in context );
        if ( payload.Length < 256 || payload.Length % 8 != 0 )
            Fail( $"payload capacity {payload.Length}" );
        FillPattern( payload.Slice( 0, 256 ), 1 );
        if ( RocketNet.ClientFinishPacket( client, ref context, 256 ) == 0 )
            Fail( "client finish packet" );
        RocketNet.ClientSendPackets( client );

        byte[] convenience = new byte[64];
        FillPattern( convenience, 2 );
        RocketNet.ClientSendPacket( client, convenience );

        // the oversize finish is dropped with its sequence burned, never sent

        RocketNet.PacketContext oversize = default;
        if ( RocketNet.ClientStartPacket( client, ref oversize ) == IntPtr.Zero )
            Fail( "client start packet (oversize)" );
        if ( RocketNet.ClientFinishPacket( client, ref oversize, oversize.PayloadCapacity + 1 ) != 0 )
            Fail( "oversize finish was not dropped" );
        Ok( "oversize finish_packet dropped (returned 0)" );

        bool got256 = false, got64 = false;
        for ( int i = 0; i < 1000 && !( got256 && got64 ); i++ )
        {
            time += TimeStep;
            RocketNet.ClientUpdate( client, time );
            RocketNet.ServerUpdate( server, time );
            RocketNet.ServerPayloadBatch batch = RocketNet.ServerPayloads( server );
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
            RocketNet.ServerSendPackets( server );
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
        if ( RocketNet.ServerStartPacket( server, clientIndex, ref down ) == IntPtr.Zero )
            Fail( "server start packet" );
        FillPattern( RocketNet.PayloadSpan( in down ).Slice( 0, 256 ), 1 );
        if ( RocketNet.ServerFinishPacket( server, ref down, 256 ) == 0 )
            Fail( "server finish packet" );
        RocketNet.ServerSendPacket( server, clientIndex, new ReadOnlySpan<byte>( ConvenienceDown() ) );

        bool gotEcho = false, gotDown = false;
        for ( int i = 0; i < 1000 && !( gotEcho && gotDown ); i++ )
        {
            Pump( client, server );
            int num = RocketNet.ClientNumPayloads( client );
            RocketNet.ClientPayloadBatch batch = RocketNet.ClientPayloads( client );
            if ( batch.Count != num )
                Fail( "client payload count mismatch between accessors" );
            for ( int j = 0; j < batch.Count; j++ )
            {
                ReadOnlySpan<byte> received = batch[j];
                if ( !received.SequenceEqual( RocketNet.ClientPayloadSpan( client, j ) ) )
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

        // counters: delivery counted, and the C# counter indices line up with the
        // C names at both ends of the enum

        ReadOnlySpan<ulong> serverCounters = RocketNet.CountersSpan( RocketNet.ServerCounters( server ) );
        if ( serverCounters[(int) RocketNet.Counter.PayloadDelivered] < 2 )
            Fail( "server payload delivered counter" );
        if ( RocketNet.CounterName( RocketNet.Counter.ConnectionRequestSignatureDidNotVerify ) != "CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY" )
            Fail( "counter name mapping (signature)" );
        if ( RocketNet.CounterName( RocketNet.Counter.ClientDisconnectAccepted ) != "CLIENT_DISCONNECT_ACCEPTED" )
            Fail( "counter name mapping (last counter)" );
        Ok( $"counters: {serverCounters[(int) RocketNet.Counter.PayloadDelivered]} payloads delivered at the server" );

        // the deliberate server-side disconnect, observed by the client

        RocketNet.ServerDisconnectClient( server, clientIndex );
        for ( int i = 0; i < 1000 && RocketNet.ClientState( client ) > RocketNet.ClientStateDisconnected; i++ )
            Pump( client, server );
        if ( RocketNet.ClientState( client ) > RocketNet.ClientStateDisconnected )
            Fail( "client did not observe server disconnect" );
        if ( RocketNet.ServerNumConnectedClients( server ) != 0 )
            Fail( "server still counts the disconnected client" );
        Ok( $"server disconnect observed by client (state '{RocketNet.ClientStateName( RocketNet.ClientState( client ) )}')" );
        RocketNet.ClientDestroy( client );

        // negative control 1: one corrupted byte in the clear signed fields
        // (byte 0 is the version) and the client refuses the token at create

        byte[] badToken = new byte[RocketNet.ConnectTokenBytes];
        if ( RocketNet.WriteDevConnectToken( address, RocketNet.RandomUint64(), unixNow, badToken ) == 0 )
            Fail( "write dev connect token (negative 1)" );
        badToken[0] ^= 0xFF;
        RocketNet.Quiet( true );        // the refusal logs an error by design; keep the receipt clean
        IntPtr badClient = RocketNet.ClientCreate( IntPtr.Zero, badToken, IntPtr.Zero, time );
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
        IntPtr sealedClient = RocketNet.ClientCreate( IntPtr.Zero, sealedBad, IntPtr.Zero, time );
        if ( sealedClient == IntPtr.Zero )
            Fail( "sealed-block corruption should pass the client's local checks" );
        ulong refusals = 0;
        for ( int i = 0; i < 1000 && refusals == 0; i++ )
        {
            Pump( sealedClient, server );
            if ( RocketNet.ClientState( sealedClient ) == RocketNet.ClientStateConnected )
                Fail( "client connected with a corrupted sealed block" );
            refusals = RocketNet.CountersSpan( RocketNet.ServerCounters( server ) )[(int) RocketNet.Counter.ConnectionRequestSignatureDidNotVerify];
        }
        if ( refusals == 0 )
            Fail( "server never refused the corrupted signature" );
        Ok( $"negative control: sealed-block corruption refused by server signature check ({refusals} refusal(s)), client state '{RocketNet.ClientStateName( RocketNet.ClientState( sealedClient ) )}'" );
        RocketNet.ClientDisconnect( sealedClient );
        RocketNet.ClientDestroy( sealedClient );

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

        RocketNet.ServerStop( server );
        for ( int i = 0; i < 1000 && RocketNet.ServerState( server ) != RocketNet.ServerStopped; i++ )
            Pump( IntPtr.Zero, server );
        if ( RocketNet.ServerState( server ) != RocketNet.ServerStopped )
            Fail( "server did not stop" );
        RocketNet.ServerDestroy( server );
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
