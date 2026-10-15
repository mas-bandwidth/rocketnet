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


    static void Pump( ClientTransportHandle? client, ServerTransportHandle server )
    {
        time += TimeStep;
        if ( client is not null )
            RocketNet.ClientTransportUpdate( client, time );
        RocketNet.ServerTransportUpdate( server, time );
        RocketNet.ServerTransportSendPackets( server );
        Thread.Sleep( 1 );
    }

    static void RejectToken( Action action, bool isNull = false )
    {
        try
        {
            action();
        }
        catch ( ArgumentException e ) when ( e.ParamName == "connectToken" && ( e is ArgumentNullException ) == isNull )
        {
            return;
        }
        Fail( "invalid token buffer reached the native boundary" );
    }

    static void CheckTokenBoundaries()
    {
        RocketNet.ClientTransportConfig config = default;
#if ROCKETNET_DEVELOPMENT
        RejectToken( () => RocketNet.WriteDevConnectToken( "127.0.0.1:40000", 1, 1, (byte[]) null! ), true );
#endif
        RejectToken( () => RocketNet.ClientTransportCreate( IntPtr.Zero, (byte[]) null!, IntPtr.Zero, time ), true );
        RejectToken( () => RocketNet.ClientTransportCreate( IntPtr.Zero, (byte[]) null!, ref config, time ), true );
        foreach ( int length in new[] { 0, 1, RocketNet.ConnectTokenBytes - 1, RocketNet.ConnectTokenBytes + 1 } )
        {
            byte[] token = new byte[length];
#if ROCKETNET_DEVELOPMENT
            RejectToken( () => RocketNet.WriteDevConnectToken( "127.0.0.1:40000", 1, 1, token ) );
            RejectToken( () => RocketNet.WriteDevConnectToken( "127.0.0.1:40000", 1, 1, token.AsSpan() ) );
#endif
            RejectToken( () => RocketNet.ClientTransportCreate( IntPtr.Zero, token, IntPtr.Zero, time ) );
            RejectToken( () => RocketNet.ClientTransportCreate( IntPtr.Zero, token, ref config, time ) );
            RejectToken( () => RocketNet.ClientTransportCreate( IntPtr.Zero, (ReadOnlySpan<byte>) token, IntPtr.Zero, time ) );
            RejectToken( () => RocketNet.ClientTransportCreate( IntPtr.Zero, (ReadOnlySpan<byte>) token, ref config, time ) );
        }
        Ok( "token boundary: null, empty, short and long arrays/spans refused before native calls" );
    }

    static void CheckNonNullConfig()
    {
        try
        {
            RocketNet.ClientTransportCreate( IntPtr.Zero, new byte[RocketNet.ConnectTokenBytes], (IntPtr)1, time );
            Fail( "non-zero nullConfig did not throw" );
        }
        catch ( ArgumentNullException ) { }
        try
        {
            RocketNet.ClientTransportCreate( IntPtr.Zero, new byte[RocketNet.ConnectTokenBytes].AsSpan(), (IntPtr)1, time );
            Fail( "non-zero nullConfig (span) did not throw" );
        }
        catch ( ArgumentNullException ) { }
        Ok( "smoke_csharp_nonzero_config_throws" );
    }

    static int Main( string[] args )
    {
        CheckTokenBoundaries();
        CheckNonNullConfig();
        if ( args.Length == 1 && args[0] == "--token-boundaries" )
            return 0;

        if ( args.Length == 1 && args[0] == "--load-refusal" )
        {
            try { RocketNet.ClientTransportDefaultConfig( out var unused ); }
            catch ( System.DllNotFoundException ) { Ok( "explicit missing library refused" ); return 0; }
            Fail( "explicit missing library silently fell back" );
        }

        if ( args.Length == 2 && args[0] == "--abi-refusal" )
        {
            try
            {
                // Deliberately before Init, through an actual struct-writing import.
                RocketNet.ClientTransportDefaultConfig( out var unused );
                Fail( "incompatible ABI reached a struct-writing entry point" );
            }
            catch ( BadImageFormatException e ) when ( e.Message.Contains("RocketNet ABI:") && e.Message.Contains(args[1]) )
            {
                Ok( e.Message );
                return 0;
            }
            return 1;
        }

        // The first matching import also writes a struct before Init.
        RocketNet.ClientTransportDefaultConfig( out var abiDefault );
        if ( abiDefault.GetDriver() != "auto" ) Fail( "ABI-checked default config" );
        RocketNet.ValidateAbi();
        Ok( "native ABI revision, seven struct sizes and every field offset/width match" );
        if ( args.Length == 1 && args[0] == "--abi-only" ) return 0;

        if ( !RocketNet.Init() )
            Fail( "rocketnet_init" );
        RocketNet.LogLevel( RocketNet.LogLevelError );

        // the commit barrier, structural rather than per-line discipline: this
        // smoke must never commit an XDP data plane on any box it runs on,
        // whatever a future edit does to the driver pins below

        RocketNet.ForbidXdpCommit();
        Ok( $"init (protocol version {RocketNet.ProtocolVersion():x}, native time {RocketNet.Time():F2}, user id {RocketNet.UserIdString( 0x1234567812345678UL )})" );

        string address = "127.0.0.1:0";

        // the config table mirror (#106): the default carries driver "auto",
        // the struct round-trips a pinned list through the binding, a typo
        // refuses at create, and the granted driver reads back by name

        RocketNet.ServerTransportDefaultConfig( out RocketNet.ServerTransportConfig defaultConfig );
        if ( defaultConfig.GetDriver() != "auto" )
            Fail( $"default config driver is '{defaultConfig.GetDriver()}', expected 'auto'" );
        if ( defaultConfig.GetPublicAddress() != "" )
            Fail( $"default config public address is '{defaultConfig.GetPublicAddress()}', expected empty" );
        if ( RocketNet.ServerTransportRequiredMemory( ref defaultConfig, "portable" ) == 0 ||
             RocketNet.ServerTransportRequiredMemory( ref defaultConfig, "auto" ) != 0 )
            Fail( "memory planner requires a concrete built-in driver" );
        defaultConfig.SetPublicAddress( "10.0.0.1:5000" );
        if ( defaultConfig.GetPublicAddress() != "10.0.0.1:5000" )
            Fail( "public address does not round-trip through the binding" );

        RocketNet.ServerTransportDevConfig( out RocketNet.ServerTransportConfig typoConfig );
        typoConfig.SetDriver( "protable" );
        RocketNet.LogLevel( RocketNet.LogLevelNone );       // the refusal is the assertion; its chatter is not
        ServerTransportHandle typoServer = RocketNet.ServerTransportCreate( IntPtr.Zero, address, ref typoConfig );
        RocketNet.LogLevel( RocketNet.LogLevelError );
        if ( !typoServer.IsInvalid )
            Fail( "a typo driver list must refuse create" );
        typoServer.Dispose();
        Ok( "config mirror: default driver 'auto', typo driver list refused at create" );

        RocketNet.ServerTransportDevConfig( out RocketNet.ServerTransportConfig serverConfig );
        serverConfig.MaxClientSlots = 4;
        string serverDriver = args.Length == 1 && args[0] == "--iocp" ? "iocp" : "portable";
        serverConfig.SetDriver( serverDriver );
        ServerTransportHandle server = RocketNet.ServerTransportCreate( IntPtr.Zero, address, ref serverConfig );
        if ( server.IsInvalid )
            Fail( $"server create on {address}" );
        if ( RocketNet.ServerTransportDriver( server ) != serverDriver )
            Fail( $"granted driver is '{RocketNet.ServerTransportDriver( server )}', expected the pinned '{serverDriver}'" );
        ushort port = RocketNet.ServerTransportPort( server );
        if ( port == 0 ) Fail( "server bound port remains zero" );
        address = $"127.0.0.1:{port}";
        Ok( $"server up on {address} [{RocketNet.ServerTransportDriver( server )}]" );

        // connect with an explicitly defaulted config: the config struct layout
        // crosses the interop boundary here — a wrong filter value would leave
        // every packet prefiltered on the floor

        ulong clientId = RocketNet.RandomUint64();
        byte[] token = new byte[RocketNet.ConnectTokenBytes];
#if ROCKETNET_DEVELOPMENT
        if ( RocketNet.WriteDevConnectToken( address, clientId, (ulong) DateTimeOffset.UtcNow.ToUnixTimeSeconds(), token.AsSpan() ) == 0 )
            Fail( "write dev connect token" );
#endif

        RocketNet.ClientTransportDefaultConfig( out RocketNet.ClientTransportConfig clientConfig );
        if ( clientConfig.GetDriver() != "auto" )
            Fail( $"client default config driver is '{clientConfig.GetDriver()}', expected 'auto'" );
        ClientTransportHandle client = RocketNet.ClientTransportCreate( IntPtr.Zero, (ReadOnlySpan<byte>) token, ref clientConfig, time );
        if ( client.IsInvalid )
            Fail( "client create" );
        if ( RocketNet.ClientTransportDriver( client ).Length == 0 )
            Fail( "client granted driver reads back empty" );

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

        // Both abort entry points consume the lease without a payload.
        RocketNet.PacketContext abortUp = default, abortDown = default;
        if ( RocketNet.ClientTransportStartPacket( client, 64, ref abortUp ) == IntPtr.Zero ||
             RocketNet.ServerTransportStartPacket( server, clientIndex, 64, ref abortDown ) == IntPtr.Zero )
            Fail( "start packets for explicit abort" );
        RocketNet.PacketContext abortUpCopy = abortUp, abortDownCopy = abortDown;
        RocketNet.ClientTransportAbortPacket( client, ref abortUp );
        RocketNet.ServerTransportAbortPacket( server, ref abortDown );
        if ( !RocketNet.ClientTransportPacketPayloadSpan( client, in abortUpCopy ).IsEmpty ||
             !RocketNet.ServerTransportPacketPayloadSpan( server, in abortDownCopy ).IsEmpty ||
             RocketNet.ClientTransportFinishPacket( client, ref abortUpCopy, 1 ) != 0 ||
             RocketNet.ServerTransportFinishPacket( server, ref abortDownCopy, 1 ) != 0 )
            Fail( "explicit abort left a live lease" );
        Ok( "client and server abort consume their leases" );

        // client -> server: one payload through acquire/commit, one through the
        // convenience send

        RocketNet.PacketContext context = default;
        if ( RocketNet.ClientTransportStartPacket( client, RocketNet.MaxPayloadBytes, ref context ) == IntPtr.Zero )
            Fail( "client start packet" );
        Span<byte> payload = RocketNet.ClientTransportPacketPayloadSpan( client, in context );
        if ( payload.Length < 256 || payload.Length % 8 != 0 )
            Fail( $"payload capacity {payload.Length}" );
        RocketNet.PacketContext original = context;
        RocketNet.PacketContext tampered = context;
        tampered.PayloadCapacity = int.MaxValue;
        if (!RocketNet.ClientTransportPacketPayloadSpan(client, in tampered).IsEmpty ||
            !RocketNet.ServerTransportPacketPayloadSpan(server, in context).IsEmpty)
            Fail("invalid packet owner or metadata exposed a payload span");
        if (RocketNet.ClientTransportFinishPacket(client, ref tampered, 1) != 0)
            Fail("tampered packet context was accepted");
        FillPattern( payload.Slice( 0, 256 ), 1 );
        if ( RocketNet.ClientTransportFinishPacket( client, ref context, 256 ) == 0 )
            Fail( "client finish packet" );
        if (!RocketNet.ClientTransportPacketPayloadSpan(client, in original).IsEmpty ||
            RocketNet.ClientTransportFinishPacket(client, ref original, 1) != 0)
            Fail("a copied consumed context remained live");
        RocketNet.ClientTransportSendPackets( client );

        byte[] convenience = new byte[64];
        FillPattern( convenience, 2 );
        RocketNet.ClientTransportSendPacket( client, convenience );

        // the oversize finish is dropped with its sequence burned, never sent

        RocketNet.PacketContext oversize = default;
        if ( RocketNet.ClientTransportStartPacket( client, RocketNet.MaxPayloadBytes, ref oversize ) == IntPtr.Zero )
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
                ReadOnlySpan<byte> received = batch[j].Span;
                ReadOnlySpan<byte> indexed = RocketNet.ServerTransportPayloadSpan( server, j, out int indexedClient ).Span;
                if ( indexedClient != clientIndex || !received.SequenceEqual( indexed ) )
                    Fail( "server payload mismatch between accessors" );
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
        if ( RocketNet.ServerTransportStartPacket( server, clientIndex, RocketNet.MaxPayloadBytes, ref down ) == IntPtr.Zero )
            Fail( "server start packet" );
        FillPattern( RocketNet.ServerTransportPacketPayloadSpan( server, in down ).Slice( 0, 256 ), 1 );
        if ( RocketNet.ServerTransportFinishPacket( server, ref down, 256 ) == 0 )
            Fail( "server finish packet" );
        RocketNet.ServerTransportSendPacket( server, clientIndex, new ReadOnlySpan<byte>( ConvenienceDown() ) );

        byte[] receiveCopy = new byte[RocketNet.ClientTransportMaxPayloadBytes( client )];
        bool shortReceiveRefused = false;
        try { RocketNet.ClientTransportReceivePacket( client, new byte[1].AsSpan() ); }
        catch ( ArgumentException ) { shortReceiveRefused = true; }
        if ( !shortReceiveRefused ) Fail( "copying receive accepted an undersized span" );
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
                ReadOnlySpan<byte> received = batch[j].Span;
                int copied = RocketNet.ClientTransportReceivePacket( client, receiveCopy.AsSpan() );
                if ( copied != received.Length || !received.SequenceEqual( receiveCopy.AsSpan( 0, copied ) ) )
                    Fail( "copying receive differs from the client batch" );
                if ( !received.SequenceEqual( RocketNet.ClientTransportPayloadSpan( client, j ).Span ) )
                    Fail( "client payload mismatch between accessors" );
                if ( received.Length == 256 && MatchesPattern( received, 1 ) )
                    gotEcho = true;
                else if ( received.Length == 32 && MatchesPattern( received, 3 ) )
                    gotDown = true;
                else
                    Fail( $"unexpected payload at client: {received.Length} bytes" );
            }
        }
        if ( RocketNet.ClientTransportReceivePacket( client, receiveCopy.AsSpan() ) != 0 )
            Fail( "copying receive cursor did not drain" );
        if ( !gotEcho || !gotDown )
            Fail( $"client did not receive both payloads (acquire/commit {gotEcho}, convenience {gotDown})" );
        Ok( "server -> client: acquire/commit and convenience payloads byte-exact" );

        // received-payload views are checked: a view taken from the last update
        // carries the transport's update epoch and throws once the next update
        // has reused the storage it points at

        {
            RocketNet.PacketContext staleUp = default;
            if ( RocketNet.ClientTransportStartPacket( client, 64, ref staleUp ) == IntPtr.Zero )
                Fail( "start packet for received-span staleness test" );
            FillPattern( RocketNet.ClientTransportPacketPayloadSpan( client, in staleUp ).Slice( 0, 64 ), 7 );
            if ( RocketNet.ClientTransportFinishPacket( client, ref staleUp, 64 ) == 0 )
                Fail( "finish packet for received-span staleness test" );
            RocketNet.ClientTransportSendPackets( client );

            RocketNet.ReceivedPayload staleView = default;
            bool gotStaleCandidate = false;
            for ( int i = 0; i < 1000 && !gotStaleCandidate; i++ )
            {
                time += TimeStep;
                RocketNet.ClientTransportUpdate( client, time );
                RocketNet.ServerTransportUpdate( server, time );
                RocketNet.ServerTransportPayloadBatch staleBatch = RocketNet.ServerTransportPayloads( server );
                if ( staleBatch.Count > 0 )
                {
                    staleView = staleBatch[0];
                    gotStaleCandidate = true;
                }
                RocketNet.ServerTransportSendPackets( server );
                Thread.Sleep( 1 );
            }
            if ( !gotStaleCandidate )
                Fail( "no received payload to test staleness against" );

            // the next update reuses the storage the view points at
            time += TimeStep;
            RocketNet.ServerTransportUpdate( server, time );
            try
            {
                _ = staleView.Length;
                Fail( "a received payload was read after the next update" );
            }
            catch ( StalePayloadException )
            {
                Ok( "smoke_csharp_received_span_stale_after_update" );
            }
            RocketNet.ServerTransportSendPackets( server );
        }

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
        if ( RocketNet.ClientTransportStartPacket( client, RocketNet.MaxPayloadBytes, ref ackedUp ) == IntPtr.Zero )
            Fail( "client start packet (acked)" );
        FillPattern( RocketNet.ClientTransportPacketPayloadSpan( client, in ackedUp ).Slice( 0, 16 ), 4 );
        if ( RocketNet.ClientTransportFinishPacket( client, ref ackedUp, 16 ) == 0 )
            Fail( "client finish packet (acked)" );
        RocketNet.ClientTransportSendPackets( client );

        RocketNet.PacketContext ackedDown = default;
        if ( RocketNet.ServerTransportStartPacket( server, clientIndex, RocketNet.MaxPayloadBytes, ref ackedDown ) == IntPtr.Zero )
            Fail( "server start packet (acked)" );
        FillPattern( RocketNet.ServerTransportPacketPayloadSpan( server, in ackedDown ).Slice( 0, 16 ), 5 );
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

        // fragmentation through the binding: a request above the wire's
        // capacity fragments transparently at finish and reassembles
        // byte-exact into the ordinary payload batch on the far side, and
        // the payload's one packet id surfaces when every fragment's wire
        // packet is acknowledged

        RocketNet.PacketContext bigUp = default;
        if ( RocketNet.ClientTransportStartPacket( client, 5000, ref bigUp ) == IntPtr.Zero )
            Fail( "client start packet (fragmented)" );
        if ( bigUp.PayloadCapacity != RocketNet.DefaultMaxPayloadBytes )
            Fail( $"fragmented payload capacity {bigUp.PayloadCapacity}" );
        FillPattern( RocketNet.ClientTransportPacketPayloadSpan( client, in bigUp ).Slice( 0, 5000 ), 6 );
        if ( RocketNet.ClientTransportFinishPacket( client, ref bigUp, 5000 ) == 0 )
            Fail( "client finish packet (fragmented)" );
        RocketNet.ClientTransportSendPackets( client );

        bool gotFragmented = false, fragmentedAcked = false;
        for ( int i = 0; i < 2000 && !( gotFragmented && fragmentedAcked ); i++ )
        {
            time += TimeStep;
            RocketNet.ClientTransportUpdate( client, time );
            RocketNet.ServerTransportUpdate( server, time );
            RocketNet.ServerTransportPayloadBatch fragmentBatch = RocketNet.ServerTransportPayloads( server );
            for ( int j = 0; j < fragmentBatch.Count; j++ )
            {
                ReadOnlySpan<byte> received = fragmentBatch[j].Span;
                if ( received.Length == 5000 && MatchesPattern( received, 6 ) )
                    gotFragmented = true;
                else
                    Fail( $"unexpected payload at server during fragment exchange: {received.Length} bytes" );
            }
            ReadOnlySpan<ulong> fragmentAcks = RocketNet.ClientTransportAckedPacketsSpan( client );
            for ( int j = 0; j < fragmentAcks.Length; j++ )
                if ( fragmentAcks[j] == bigUp.PacketId )
                    fragmentedAcked = true;
            RocketNet.ServerTransportSendPackets( server );
            Thread.Sleep( 1 );
        }
        if ( !gotFragmented || !fragmentedAcked )
            Fail( $"fragmented payload did not cross (received {gotFragmented}, acked {fragmentedAcked})" );
        if ( RocketNet.ClientTransportMaxPayloadBytes( client ) != RocketNet.DefaultMaxPayloadBytes ||
             RocketNet.ServerTransportMaxPayloadBytes( server ) != RocketNet.DefaultMaxPayloadBytes )
            Fail( "configured maximum payload getters" );
        ReadOnlySpan<ulong> fragmentCounters = RocketNet.CountersSpan( RocketNet.ServerTransportCounters( server ) );
        if ( fragmentCounters[(int) RocketNet.Counter.FragmentPayloadReassembled] != 1 )
            Fail( "fragment payload reassembled counter" );
        if ( fragmentCounters[(int) RocketNet.Counter.FragmentPayloadAbandoned] != 0 )
            Fail( "fragment payload abandoned on a clean loopback" );
        Ok( $"fragmented payload byte-exact both ways across the binding (id {bigUp.PacketId} acked)" );

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
            Fail( "counter name mapping (ack tail)" );
        if ( RocketNet.CounterName( RocketNet.Counter.FragmentPayloadReassembled ) != "FRAGMENT_PAYLOAD_REASSEMBLED" )
            Fail( "counter name mapping (fragment family)" );
        if ( RocketNet.CounterName( RocketNet.Counter.ReceiveCursorTooSmall ) != "RECEIVE_CURSOR_TOO_SMALL" )
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

        // destroy is destroy-once and any use after destroy is refused in
        // managed code, before the native handle is touched again

        try
        {
            RocketNet.ClientTransportDestroy( client );
            Fail( "destroying a client twice did not throw" );
        }
        catch ( ObjectDisposedException )
        {
            Ok( "smoke_csharp_double_destroy_throws" );
        }
        try
        {
            RocketNet.ClientTransportState( client );
            Fail( "using a client after destroy did not throw" );
        }
        catch ( ObjectDisposedException )
        {
            Ok( "smoke_csharp_use_after_destroy_throws" );
        }

        // span-after-update: a span taken before Update is empty after update.
        // The earlier client is already destroyed; this block uses its own live client.
        {
            ulong spanClientId = RocketNet.RandomUint64();
            byte[] spanToken = new byte[RocketNet.ConnectTokenBytes];
#if ROCKETNET_DEVELOPMENT
            if ( RocketNet.WriteDevConnectToken( address, spanClientId, (ulong) DateTimeOffset.UtcNow.ToUnixTimeSeconds(), spanToken.AsSpan() ) == 0 )
                Fail( "write dev connect token (span-after-update)" );
#endif
            RocketNet.ClientTransportDefaultConfig( out RocketNet.ClientTransportConfig spanConfig );
            ClientTransportHandle spanClient = RocketNet.ClientTransportCreate( IntPtr.Zero, (ReadOnlySpan<byte>) spanToken, ref spanConfig, time );
            if ( spanClient.IsInvalid )
                Fail( "span-after-update client create" );
            for ( int i = 0; i < 2000 && RocketNet.ClientTransportState( spanClient ) != RocketNet.ClientTransportStateConnected; i++ )
                Pump( spanClient, server );
            if ( RocketNet.ClientTransportState( spanClient ) != RocketNet.ClientTransportStateConnected )
                Fail( "span-after-update client did not connect" );

            RocketNet.PacketContext spanContext = default;
            if ( RocketNet.ClientTransportStartPacket( spanClient, 64, ref spanContext ) == IntPtr.Zero )
                Fail( "start packet for span-after-update test" );
            Span<byte> span = RocketNet.ClientTransportPacketPayloadSpan( spanClient, in spanContext );
            if ( span.IsEmpty )
                Fail( "span-after-update: span should not be empty" );
            RocketNet.ClientTransportUpdate( spanClient, time );
            Span<byte> spanAfter = RocketNet.ClientTransportPacketPayloadSpan( spanClient, in spanContext );
            if ( !spanAfter.IsEmpty )
                Fail( "span-after-update: span remained valid after update (should be invalidated)" );
            RocketNet.ClientTransportAbortPacket( spanClient, ref spanContext );
            RocketNet.ClientTransportDestroy( spanClient );
            Ok( "smoke_csharp_span_after_update_invalidated" );
        }

        // negative control 1: one corrupted byte in the clear signed fields
        // (byte 0 is the version) and the client refuses the token at create

        byte[] badToken = new byte[RocketNet.ConnectTokenBytes];
#if ROCKETNET_DEVELOPMENT
        if ( RocketNet.WriteDevConnectToken( address, RocketNet.RandomUint64(), (ulong) DateTimeOffset.UtcNow.ToUnixTimeSeconds(), badToken ) == 0 )
            Fail( "write dev connect token (negative 1)" );
#endif
        badToken[0] ^= 0xFF;
        RocketNet.Quiet( true );        // the refusal logs an error by design; keep the receipt clean
        ClientTransportHandle badClient = RocketNet.ClientTransportCreate( IntPtr.Zero, badToken, IntPtr.Zero, time );
        RocketNet.Quiet( false );
        if ( !badClient.IsInvalid )
            Fail( "corrupted token (version byte) was accepted at client create" );
        badClient.Dispose();
        Ok( "negative control: corrupted token byte 0 refused at create (invalid connect token)" );

        // negative control 2: one corrupted byte inside the sealed block and the
        // server's signature check refuses the connection request; the client
        // must never reach connected

        byte[] sealedBad = new byte[RocketNet.ConnectTokenBytes];
#if ROCKETNET_DEVELOPMENT
        if ( RocketNet.WriteDevConnectToken( address, RocketNet.RandomUint64(), (ulong) DateTimeOffset.UtcNow.ToUnixTimeSeconds(), sealedBad ) == 0 )
            Fail( "write dev connect token (negative 2)" );
#endif
        sealedBad[100] ^= 0xFF;         // inside the sealed private block, under the signature
        ClientTransportHandle sealedClient = RocketNet.ClientTransportCreate( IntPtr.Zero, sealedBad, IntPtr.Zero, time );
        if ( sealedClient.IsInvalid )
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

        // frame sync: a safe handle owns the native frame sync exactly once, so
        // a second destroy and any use after destroy are refused in managed code
        // before native memory is touched. Three real-time waits at 60fps and
        // sane stats.

        FrameSyncHandle frameSync = RocketNet.FrameSyncCreate( IntPtr.Zero, 60.0 );
        if ( frameSync.IsInvalid )
            Fail( "frame sync create" );
        for ( int i = 0; i < 3; i++ )
            RocketNet.FrameSyncWait( frameSync );
        RocketNet.FrameSyncGetStats( frameSync, out RocketNet.FrameSyncStats stats );
        if ( stats.Frame < 1 || stats.GuardSeconds < 0.0 || stats.GuardSeconds > 1.0 )
            Fail( $"frame sync stats implausible (frame {stats.Frame}, guard {stats.GuardSeconds})" );
        Ok( $"frame sync: {stats.Frame} frames, {stats.DroppedFrames} dropped, priority {stats.PriorityHeld}, spin {stats.Spinning}" );

        RocketNet.FrameSyncDestroy( frameSync );
        try
        {
            RocketNet.FrameSyncDestroy( frameSync );
            Fail( "destroying a frame sync twice did not throw" );
        }
        catch ( ObjectDisposedException )
        {
            Ok( "smoke_csharp_frame_sync_double_dispose_safe: second destroy throws" );
        }
        try
        {
            RocketNet.FrameSyncWait( frameSync );
            Fail( "waiting on a destroyed frame sync did not throw" );
        }
        catch ( ObjectDisposedException )
        {
            Ok( "smoke_csharp_frame_sync_double_dispose_safe: wait after destroy throws" );
        }

        // stop the server and shut down

        RocketNet.ServerTransportStop( server );
        for ( int i = 0; i < 1000 && RocketNet.ServerTransportState( server ) != RocketNet.ServerTransportStopped; i++ )
            Pump( null, server );
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
