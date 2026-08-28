/*
    RocketNet.cs — C# binding for the rocketnet SDK.

    P/Invoke over the public C surface in sdk/rocketnet.h, against the shared
    library (cmake target rocketnet_shared). The raw statics mirror the C
    functions one to one — same names minus the rocketnet_ prefix, PascalCase —
    and the structs are blittable, laid out to match the C exactly. A thin
    span layer wraps the pointer-shaped calls; the raw layer is the product.

    Not bound: the varargs logging calls (rocketnet_printf/info/warn/error/
    debug/spam) and rocketnet_log_function — C varargs are not portably
    callable through P/Invoke. rocketnet_log_level and rocketnet_quiet are
    bound and control the library's own logging. Also not bound: the assert
    handler, the allocator hooks, and rocketnet_copy_string (a C string util).

    The native library resolves by the plain name "rocketnet": the runtime
    probes the executable's directory and the system paths; set
    ROCKETNET_LIBRARY_PATH (a file, or a directory holding librocketnet) to
    load from anywhere else.

    Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0 (see LICENSE).
*/

using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

/// <summary>The rocketnet C SDK surface: raw P/Invoke plus thin span helpers.</summary>
public static unsafe class RocketNet
{
    const string Lib = "rocketnet";

    static RocketNet()
    {
        NativeLibrary.SetDllImportResolver( typeof(RocketNet).Assembly, Resolve );
    }

    static IntPtr Resolve( string name, Assembly assembly, DllImportSearchPath? searchPath )
    {
        if ( name != Lib )
            return IntPtr.Zero;
        string fileName = OperatingSystem.IsWindows() ? "rocketnet.dll"
                        : OperatingSystem.IsMacOS()   ? "librocketnet.dylib"
                        :                               "librocketnet.so";
        string? env = Environment.GetEnvironmentVariable( "ROCKETNET_LIBRARY_PATH" );
        if ( env != null )
        {
            string candidate = Directory.Exists( env ) ? Path.Combine( env, fileName ) : env;
            if ( File.Exists( candidate ) && NativeLibrary.TryLoad( Path.GetFullPath( candidate ), out IntPtr handle ) )
                return handle;
        }
        string local = Path.Combine( AppContext.BaseDirectory, fileName );
        if ( File.Exists( local ) && NativeLibrary.TryLoad( local, out IntPtr baseHandle ) )
            return baseHandle;
        return IntPtr.Zero;                     // fall through to the runtime's default probing
    }

    // -----------------------------------------
    //                               constants
    // -----------------------------------------

    /// <summary>Maximum client slots a server supports.</summary>
    public const int MaxClients = 1000;

    /// <summary>Largest payload accepted by the send calls and delivered by the receive calls.</summary>
    public const int MaxPayloadBytes = 1400;

    /// <summary>Size of a connect token in bytes.</summary>
    public const int ConnectTokenBytes = 768;

    /// <summary>Log level: nothing.</summary>
    public const int LogLevelNone = 0;
    /// <summary>Log level: errors only.</summary>
    public const int LogLevelError = 1;
    /// <summary>Log level: warnings and errors.</summary>
    public const int LogLevelWarn = 2;
    /// <summary>Log level: informational and below. The default.</summary>
    public const int LogLevelInfo = 3;
    /// <summary>Log level: debug and below.</summary>
    public const int LogLevelDebug = 4;
    /// <summary>Log level: everything.</summary>
    public const int LogLevelSpam = 5;

    /// <summary>Client state: the connect token expired before the connection completed.</summary>
    public const int ClientTransportStateConnectTokenExpired = -7;
    /// <summary>Client state: the connect token did not parse or failed its pre-send checks.</summary>
    public const int ClientTransportStateInvalidConnectToken = -6;
    /// <summary>Client state: an established connection timed out.</summary>
    public const int ClientTransportStateConnectionTimedOut = -5;
    /// <summary>Client state: the connection response phase timed out.</summary>
    public const int ClientTransportStateConnectionResponseTimedOut = -4;
    /// <summary>Client state: the connection request phase timed out.</summary>
    public const int ClientTransportStateConnectionRequestTimedOut = -3;
    /// <summary>Client state: the cookie request phase timed out.</summary>
    public const int ClientTransportStateCookieRequestTimedOut = -2;
    /// <summary>Client state: the server denied the connection.</summary>
    public const int ClientTransportStateConnectionDenied = -1;
    /// <summary>Client state: not connected.</summary>
    public const int ClientTransportStateDisconnected = 0;
    /// <summary>Client state: requesting a cookie from the server.</summary>
    public const int ClientTransportStateRequestingCookie = 1;
    /// <summary>Client state: sending the connection request.</summary>
    public const int ClientTransportStateSendingConnectionRequest = 2;
    /// <summary>Client state: sending the connection response.</summary>
    public const int ClientTransportStateSendingConnectionResponse = 3;
    /// <summary>Client state: connected.</summary>
    public const int ClientTransportStateConnected = 4;

    /// <summary>Server state: running.</summary>
    public const int ServerTransportRunning = 0;
    /// <summary>Server state: stopping, redundant disconnects in flight.</summary>
    public const int ServerTransportStopping = 1;
    /// <summary>Server state: stopped.</summary>
    public const int ServerTransportStopped = 2;

    /// <summary>Number of counters in <see cref="Counter"/>. The length of the counters block.</summary>
    public const int NumCounters = 105;

    /// <summary>Counter indices, mirroring sdk/rocketnet_counters.h: one counter per distinct
    /// reason a packet is rejected, arrival counted separately from outcome, monotonic and
    /// never reset. Indices are the C implementation's contract and are never renumbered.</summary>
#pragma warning disable 1591        // the member names are the C names; the C header is the reference
    public enum Counter
    {
        PacketsReceived = 0,
        PrefilterTooShort,
        PrefilterTooLarge,
        PrefilterWrongEpoch,
        PrefilterWrongValue,
        ReservedType,
        WrongDirection,
        ConnectionRequestReceived,
        ConnectionRequestWrongSize,
        ConnectionRequestAlreadyConnected,
        ConnectionRequestChallengeResent,
        ConnectionRequestWrongVersion,
        ConnectionRequestWrongProtocolId,
        ConnectionRequestTokenExpired,
        ConnectionRequestTimeoutInvalid,
        ConnectionRequestBadAddressFlags,
        ConnectionRequestAddressNotAdvertised,
        ConnectionRequestWrongServerKey,
        ConnectionRequestNoCookie,
        ConnectionRequestCookieDidNotVerify,
        ConnectionRequestCookieExpired,
        ConnectionRequestTokenAlreadyUsed,
        ConnectionRequestSignatureDidNotVerify,
        ConnectionRequestCouldNotOpenSealedBlock,
        ConnectionRequestSealedMismatch,
        ConnectionRequestClientIdAlreadyConnected,
        ConnectionRequestTokenAlreadyConnected,
        ConnectionRequestSmallOrderKey,
        ConnectionRequestNoSlots,
        ConnectionRequestCouldNotAddMapping,
        ConnectionRequestSentChallenge,
        ConnectionRequestInternalError,
        ConnectionResponseReceived,
        ConnectionResponseNoMapping,
        ConnectionResponseZeroSequence,
        ConnectionResponseDidNotOpen,
        ConnectionResponseAlreadyConnected,
        ConnectionResponseClientIdAlreadyConnected,
        ConnectionResponseAdmittedSetFull,
        ConnectionResponseNoSlots,
        ConnectionResponseConnected,
        KeepAliveReceived,
        KeepAliveNoMapping,
        KeepAliveZeroSequence,
        KeepAliveAlreadyReceived,
        KeepAliveDidNotOpen,
        KeepAliveNotConnected,
        KeepAliveAccepted,
        PayloadReceived,
        PayloadNoMapping,
        PayloadZeroSequence,
        PayloadAlreadyReceived,
        PayloadDidNotOpen,
        PayloadNotConnected,
        PayloadDelivered,
        DisconnectReceived,
        DisconnectNoMapping,
        DisconnectZeroSequence,
        DisconnectAlreadyReceived,
        DisconnectDidNotOpen,
        DisconnectNotConnected,
        DisconnectDisconnected,
        MappingCreated,
        MappingIdleTimeout,
        MappingHandshakeDeadlineExpired,
        MappingTableFull,
        ClientConnected,
        SendRingFull,
        SlotWithoutMapping,
        ClientDisconnected,
        ServerDisconnectedClient,
        ClientTimedOut,
        ClientCookieReceived,
        ClientCookieWrongState,
        ClientCookieSaltMismatch,
        ClientCookieStored,
        ClientChallengeReceived,
        ClientChallengeWrongState,
        ClientChallengeZeroSequence,
        ClientChallengeDidNotOpen,
        ClientChallengeSmallOrderKey,
        ClientChallengeAccepted,
        ClientDeniedReceived,
        ClientDeniedWrongState,
        ClientDeniedZeroSequence,
        ClientDeniedDidNotOpen,
        ClientDeniedAccepted,
        ClientKeepAliveReceived,
        ClientKeepAliveWrongState,
        ClientKeepAliveZeroSequence,
        ClientKeepAliveAlreadyReceived,
        ClientKeepAliveDidNotOpen,
        ClientKeepAliveAccepted,
        ClientPayloadReceived,
        ClientPayloadWrongState,
        ClientPayloadZeroSequence,
        ClientPayloadAlreadyReceived,
        ClientPayloadDidNotOpen,
        ClientPayloadDelivered,
        ClientDisconnectReceived,
        ClientDisconnectWrongState,
        ClientDisconnectZeroSequence,
        ClientDisconnectAlreadyReceived,
        ClientDisconnectDidNotOpen,
        ClientDisconnectAccepted,
    }
#pragma warning restore 1591

    // -----------------------------------------
    //                                 structs
    // -----------------------------------------

    /// <summary>rocketnet_client_transport_config_t. The per-title filter value and an optional native
    /// state-change callback. <see cref="ClientTransportDefaultConfig"/> fills the development posture.</summary>
    [StructLayout( LayoutKind.Sequential )]
    public struct ClientTransportConfig
    {
        /// <summary>The per-title filter value, 4 bytes.</summary>
        public fixed byte FilterValue[4];

        /// <summary>Native function pointer for void (*)( void * context, int previous_state,
        /// int current_state ), or IntPtr.Zero for none. Delegate marshaling is deliberately
        /// not built here: the callback fires on library threads, and the pinning and lifetime
        /// rules that makes safe are not worth the surface in v1 — poll <see cref="ClientTransportState(IntPtr)"/>.</summary>
        public IntPtr StateChangeCallback;
    }

    /// <summary>rocketnet_server_transport_config_t. The server is configured explicitly;
    /// <see cref="ServerTransportDefaultConfig"/> gives the shape with no keys (create refuses it),
    /// <see cref="ServerTransportDevConfig"/> fills the published development keys and test filter.</summary>
    [StructLayout( LayoutKind.Sequential )]
    public struct ServerTransportConfig
    {
        /// <summary>The protocol id clients must present.</summary>
        public ulong ProtocolId;
        /// <summary>Client slots, at most <see cref="MaxClients"/>.</summary>
        public int MaxClientSlots;
        /// <summary>The per-title filter value, 4 bytes.</summary>
        public fixed byte FilterValue[4];
        /// <summary>The backend's public signing key, 32 bytes.</summary>
        public fixed byte BackendPublicKey[32];
        /// <summary>The server's public key, 32 bytes.</summary>
        public fixed byte ServerPublicKey[32];
        /// <summary>The server's secret key, 32 bytes.</summary>
        public fixed byte ServerSecretKey[32];
        /// <summary>XDP transport queue count; zero means the transport default. The portable build ignores it.</summary>
        public int NumXdpQueues;
        /// <summary>XDP transport OS cpu count; zero means the transport default. The portable build ignores it.</summary>
        public int NumOsCpus;
    }

    /// <summary>rocketnet_packet_context_t. Filled by StartPacket, consumed by FinishPacket or
    /// AbortPacket. The fields are read-only to the caller; the payload is written at
    /// Frame + PayloadIndex, up to PayloadCapacity bytes — <see cref="PayloadSpan"/> is that span.</summary>
    [StructLayout( LayoutKind.Sequential )]
    public struct PacketContext
    {
        /// <summary>The 16-aligned frame the packet is built in.</summary>
        public IntPtr Frame;
        /// <summary>Offset of the payload within the frame, itself 16-aligned.</summary>
        public int PayloadIndex;
        /// <summary>Payload capacity in bytes, a multiple of eight.</summary>
        public int PayloadCapacity;
        /// <summary>The library's identifier for this in-flight packet.</summary>
        public ulong PacketId;
        /// <summary>The packet sequence number.</summary>
        public ulong Sequence;
        /// <summary>The destination client index (server side).</summary>
        public int ClientIndex;
    }

    /// <summary>rocketnet_client_transport_stats_t. Read with <see cref="ClientTransportGetStats"/>. rtt and
    /// jitter are milliseconds, packet loss is a percentage; the rates and bandwidth cover
    /// the trailing second of updates. rtt is measured from the handshake's round trips
    /// and holds from there, as the C header says.</summary>
    [StructLayout( LayoutKind.Sequential )]
    public struct ClientTransportStats
    {
        /// <summary>Round trip time in milliseconds.</summary>
        public float Rtt;
        /// <summary>Inter-arrival jitter in milliseconds.</summary>
        public float Jitter;
        /// <summary>Packet loss percentage.</summary>
        public float PacketLoss;
        /// <summary>Packets sent over the last second.</summary>
        public int PacketsSentPerSecond;
        /// <summary>Packets received over the last second.</summary>
        public int PacketsReceivedPerSecond;
        /// <summary>Bandwidth sent over the last second, kilobits per second.</summary>
        public int BandwidthSentKbps;
        /// <summary>Bandwidth received over the last second, kilobits per second.</summary>
        public int BandwidthReceivedKbps;
    }

    /// <summary>rocketnet_server_transport_stats_t. Read with <see cref="ServerTransportGetStats"/>. Whole-socket
    /// packet rates and bandwidth both ways over the trailing second, at flat per-frame cost.</summary>
    [StructLayout( LayoutKind.Sequential )]
    public struct ServerTransportStats
    {
        /// <summary>Packets sent over the last second.</summary>
        public ulong PacketsSentPerSecond;
        /// <summary>Packets received over the last second.</summary>
        public ulong PacketsReceivedPerSecond;
        /// <summary>Bandwidth sent over the last second, kilobits per second.</summary>
        public float BandwidthSentKbps;
        /// <summary>Bandwidth received over the last second, kilobits per second.</summary>
        public float BandwidthReceivedKbps;
    }

    /// <summary>rocketnet_frame_sync_stats_t. Read with <see cref="FrameSyncGetStats"/>.</summary>
    [StructLayout( LayoutKind.Sequential )]
    public struct FrameSyncStats
    {
        /// <summary>Frames completed.</summary>
        public ulong Frame;
        /// <summary>Frames lost to overruns, total.</summary>
        public ulong DroppedFrames;
        /// <summary>Nonzero when platform thread priority was obtained.</summary>
        public int PriorityHeld;
        /// <summary>Nonzero when the spin guard is active (requires priority).</summary>
        public int Spinning;
        /// <summary>The current adaptive guard in seconds.</summary>
        public double GuardSeconds;
        /// <summary>The last wake's lateness past its deadline in seconds.</summary>
        public double WakeupErrorSeconds;
    }

    // -----------------------------------------
    //                                    core
    // -----------------------------------------

    /// <summary>Initializes the library. Call once before anything else. Returns false on failure.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_init", CallingConvention = CallingConvention.Cdecl )]
    [return: MarshalAs( UnmanagedType.I1 )]
    public static extern bool Init( IntPtr context );

    /// <summary>Initializes the library with no context.</summary>
    public static bool Init() => Init( IntPtr.Zero );

    /// <summary>Shuts the library down. Call after every client and server is destroyed.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_term", CallingConvention = CallingConvention.Cdecl )]
    public static extern void Term();

    /// <summary>Suppresses all library logging when true.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_quiet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void Quiet( [MarshalAs( UnmanagedType.I1 )] bool flag );

    /// <summary>Sets the library log level, one of the LogLevel constants.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_log_level", CallingConvention = CallingConvention.Cdecl )]
    public static extern void LogLevel( int level );

    /// <summary>Formats a user id into the caller's buffer and returns it.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_user_id_string", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr UserIdString( ulong userId, byte* buffer, nuint bufferSize );

    /// <summary>Formats a user id the way the library prints one.</summary>
    public static string UserIdString( ulong userId )
    {
        byte* buffer = stackalloc byte[64];
        UserIdString( userId, buffer, 64 );
        return Marshal.PtrToStringUTF8( (IntPtr) buffer ) ?? string.Empty;
    }

    /// <summary>A uniform random float in [0,1].</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_random_float", CallingConvention = CallingConvention.Cdecl )]
    public static extern float RandomFloat();

    /// <summary>A random uint8.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_random_uint8", CallingConvention = CallingConvention.Cdecl )]
    public static extern byte RandomUint8();

    /// <summary>A random uint16.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_random_uint16", CallingConvention = CallingConvention.Cdecl )]
    public static extern ushort RandomUint16();

    /// <summary>A random uint32.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_random_uint32", CallingConvention = CallingConvention.Cdecl )]
    public static extern uint RandomUint32();

    /// <summary>A random uint64.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_random_uint64", CallingConvention = CallingConvention.Cdecl )]
    public static extern ulong RandomUint64();

    /// <summary>Fills the buffer with random bytes.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_random_bytes", CallingConvention = CallingConvention.Cdecl )]
    public static extern void RandomBytes( byte* data, nuint bytes );

    /// <summary>Fills the span with random bytes.</summary>
    public static void RandomBytes( Span<byte> data )
    {
        fixed ( byte* p = data )
            RandomBytes( p, (nuint) data.Length );
    }

    /// <summary>The protocol version the library implements.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_protocol_version", CallingConvention = CallingConvention.Cdecl )]
    public static extern ulong ProtocolVersion();

    /// <summary>The platform monotonic clock in seconds. A convenience — the library never
    /// reads it on the timed paths; the game owns time.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_time", CallingConvention = CallingConvention.Cdecl )]
    public static extern double Time();

    /// <summary>Sleeps the calling thread.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_sleep", CallingConvention = CallingConvention.Cdecl )]
    public static extern void Sleep( double seconds );

    // -----------------------------------------
    //                                  client
    // -----------------------------------------

    /// <summary>Fills the development posture: the published test filter, no callback.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_default_config", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportDefaultConfig( out ClientTransportConfig config );

    /// <summary>Mints a connect token locally against the published development keys.
    /// A production client is handed its token by a backend. The token buffer is
    /// <see cref="ConnectTokenBytes"/>. Returns nonzero on success.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_write_dev_connect_token", CallingConvention = CallingConvention.Cdecl )]
    public static extern int WriteDevConnectToken( [MarshalAs( UnmanagedType.LPUTF8Str )] string serverAddress, ulong clientId, ulong unixTimestamp, byte[] connectToken );

    /// <summary>Creates a client connecting with the token. The token names the server, carries
    /// the keys, and is the credential. time is the game's clock, a double in seconds,
    /// monotonic; pass the same clock to every call that takes one. Returns IntPtr.Zero on
    /// refusal — an invalid token refuses here.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_create", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportCreate( IntPtr context, byte[] connectToken, ref ClientTransportConfig config, double time );

    /// <summary>Creates a client with the default (development) config.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_create", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportCreate( IntPtr context, byte[] connectToken, IntPtr nullConfig, double time );

    /// <summary>Destroys the client. Disconnect and pump a few updates first.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_destroy", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportDestroy( IntPtr client );

    /// <summary>Advances the client one frame at the given game time.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_update", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportUpdate( IntPtr client, double time );

    /// <summary>The convenience send: copies and flushes inline. Main thread only, never
    /// during an open fill — the simple form, not a fill-phase participant.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_send_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportSendPacket( IntPtr client, byte* packetData, int packetBytes );

    /// <summary>The convenience send over a span.</summary>
    public static void ClientTransportSendPacket( IntPtr client, ReadOnlySpan<byte> payload )
    {
        fixed ( byte* p = payload )
            ClientTransportSendPacket( client, p, payload.Length );
    }

    /// <summary>The copying receive cursor: copies the next received payload into the buffer
    /// (at least <see cref="MaxPayloadBytes"/>) and returns its size, or zero when drained.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_receive_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportReceivePacket( IntPtr client, byte* packetData );

    /// <summary>The copying receive cursor over a span.</summary>
    public static int ClientTransportReceivePacket( IntPtr client, Span<byte> buffer )
    {
        if ( buffer.Length < MaxPayloadBytes )
            throw new ArgumentException( "buffer must hold MaxPayloadBytes", nameof(buffer) );
        fixed ( byte* p = buffer )
            return ClientTransportReceivePacket( client, p );
    }

    /// <summary>Number of payloads delivered by the last update. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_num_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportNumPayloads( IntPtr client );

    /// <summary>One delivered payload by index. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_payload", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportPayload( IntPtr client, int payloadIndex, out int payloadBytes );

    /// <summary>One delivered payload by index as a span. Valid until the next update.</summary>
    public static ReadOnlySpan<byte> ClientTransportPayloadSpan( IntPtr client, int payloadIndex )
    {
        IntPtr data = ClientTransportPayload( client, payloadIndex, out int bytes );
        return data == IntPtr.Zero ? ReadOnlySpan<byte>.Empty : new ReadOnlySpan<byte>( (void*) data, bytes );
    }

    /// <summary>The whole receive batch as parallel arrays in one call — the zero-per-item
    /// form. Valid until the next update. Returns the number of payloads.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportPayloads( IntPtr client, out byte** payloadData, out int* payloadBytes );

    /// <summary>The receive batch as a zero-copy indexed view — one interop crossing.
    /// Valid until the next update.</summary>
    public static ClientTransportPayloadBatch ClientTransportPayloads( IntPtr client )
    {
        int count = ClientTransportPayloads( client, out byte** data, out int* bytes );
        return new ClientTransportPayloadBatch( count, data, bytes );
    }

    /// <summary>Acquires a packet to fill; see the send family contract on <see cref="ServerTransportStartPacket"/>.
    /// Returns the frame, or IntPtr.Zero when no packet is available (ring full, not connected).</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_start_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportStartPacket( IntPtr client, ref PacketContext context );

    /// <summary>Seals and stages the filled packet in place. Returns the sealed wire size, or
    /// zero when the packet was dropped instead (payload out of range, or the connection
    /// gone) — the sequence is burned either way, which the replay window tolerates.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_finish_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportFinishPacket( IntPtr client, ref PacketContext context, int payloadBytes );

    /// <summary>Releases an acquired packet without sending it.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_abort_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportAbortPacket( IntPtr client, ref PacketContext context );

    /// <summary>Flushes everything staged. Required once per frame, after update; see the
    /// send family contract on <see cref="ServerTransportStartPacket"/>.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_send_packets", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportSendPackets( IntPtr client );

    /// <summary>The client state, one of the ClientTransportState constants.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_state", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportState( IntPtr client );

    /// <summary>The client's slot index on the server, -1 when not connected.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_client_index", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportClientIndex( IntPtr client );

    /// <summary>Disconnects deliberately. Main thread, never while a fill is open.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_disconnect", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportDisconnect( IntPtr client );

    /// <summary>The client's counters block; read it with <see cref="CountersSpan"/>.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_counters", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportCounters( IntPtr client );

    /// <summary>Reads the client's connection quality stats (rocketnet_client_transport_stats).</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_stats", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportGetStats( IntPtr client, out ClientTransportStats stats );

    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_state_name", CallingConvention = CallingConvention.Cdecl )]
    static extern IntPtr client_transport_state_name( int state );

    /// <summary>The name of a client state.</summary>
    public static string ClientTransportStateName( int state ) => Marshal.PtrToStringUTF8( client_transport_state_name( state ) ) ?? string.Empty;

    // -----------------------------------------
    //                                  server
    // -----------------------------------------

    /// <summary>Fills the shape with no keys — create refuses it until keys are supplied.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_default_config", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportDefaultConfig( out ServerTransportConfig config );

    /// <summary>Fills the published development keys and test filter for local work, loudly.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_dev_config", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportDevConfig( out ServerTransportConfig config );

    /// <summary>Creates a server on the public address clients dial, e.g. "127.0.0.1:40000".
    /// Returns IntPtr.Zero on refusal.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_create", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ServerTransportCreate( IntPtr context, [MarshalAs( UnmanagedType.LPUTF8Str )] string publicAddress, ref ServerTransportConfig config, double time );

    /// <summary>Destroys the server. Stop and wait for stopped first.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_destroy", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportDestroy( IntPtr server );

    /// <summary>Begins the stop: redundant disconnects go out, then the state reaches stopped.
    /// Main thread, never while a fill is open.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_stop", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportStop( IntPtr server );

    /// <summary>The server state, one of the Server state constants.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_state", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportState( IntPtr server );

    /// <summary>Advances the server one frame at the given game time.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_update", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportUpdate( IntPtr server, double time );

    /// <summary>Number of payloads delivered by the last update. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_num_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportNumPayloads( IntPtr server );

    /// <summary>One delivered payload by index, with its sender. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_payload", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ServerTransportPayload( IntPtr server, int payloadIndex, out int clientIndex, out int payloadBytes );

    /// <summary>One delivered payload by index as a span, with its sender. Valid until the next update.</summary>
    public static ReadOnlySpan<byte> ServerTransportPayloadSpan( IntPtr server, int payloadIndex, out int clientIndex )
    {
        IntPtr data = ServerTransportPayload( server, payloadIndex, out clientIndex, out int bytes );
        return data == IntPtr.Zero ? ReadOnlySpan<byte>.Empty : new ReadOnlySpan<byte>( (void*) data, bytes );
    }

    /// <summary>The whole receive batch as parallel arrays in one call — the zero-per-item
    /// form. Valid until the next update. Returns the number of payloads.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportPayloads( IntPtr server, out int* clientIndex, out byte** payloadData, out int* payloadBytes );

    /// <summary>The receive batch as a zero-copy indexed view — one interop crossing.
    /// Valid until the next update.</summary>
    public static ServerTransportPayloadBatch ServerTransportPayloads( IntPtr server )
    {
        int count = ServerTransportPayloads( server, out int* clientIndex, out byte** data, out int* bytes );
        return new ServerTransportPayloadBatch( count, clientIndex, data, bytes );
    }

    // The send surface is acquire, write, commit: StartPacket hands back a 16-aligned
    // frame; the payload is written at Frame + PayloadIndex — PayloadSpan is that span —
    // and FinishPacket seals it in place on the calling thread and stages it. Nothing is
    // copied at any step. SendPackets flushes everything staged.
    //
    // Threading contract, for the client and server families alike: Update and SendPackets
    // run on the main thread, and so do Disconnect, DisconnectClient and Stop — never while
    // a fill is open. StartPacket, FinishPacket and AbortPacket may run on any number of
    // game threads strictly between an Update and the following SendPackets, and the
    // handoff between fill and flush must be a synchronizing operation (a join or a
    // barrier), not a wall-clock wait. SendPackets is required once per frame, after
    // Update: packets are never sent without it. Calling it again in the same frame is a
    // no-op. The fields of the context are read-only to the caller.

    /// <summary>Acquires a packet to fill for one client; the threading contract is in the
    /// comment above. Returns the frame, or IntPtr.Zero when no packet is available.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_start_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ServerTransportStartPacket( IntPtr server, int clientIndex, ref PacketContext context );

    /// <summary>Seals and stages the filled packet in place. Returns the sealed wire size, or
    /// zero when the packet was dropped instead (payload out of range, or the connection
    /// gone) — the sequence is burned either way, which the replay window tolerates.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_finish_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportFinishPacket( IntPtr server, ref PacketContext context, int payloadBytes );

    /// <summary>Releases an acquired packet without sending it.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_abort_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportAbortPacket( IntPtr server, ref PacketContext context );

    /// <summary>Flushes everything staged. Required once per frame, after update; see the
    /// send family contract above.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_send_packets", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportSendPackets( IntPtr server );

    /// <summary>The convenience send: copies into an acquired packet and stages it.
    /// Flushed by the frame's SendPackets.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_send_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportSendPacket( IntPtr server, int clientIndex, byte* payloadData, int payloadBytes );

    /// <summary>The convenience send over a span.</summary>
    public static void ServerTransportSendPacket( IntPtr server, int clientIndex, ReadOnlySpan<byte> payload )
    {
        fixed ( byte* p = payload )
            ServerTransportSendPacket( server, clientIndex, p, payload.Length );
    }

    /// <summary>Disconnects one connected client deliberately: the redundant disconnect burst
    /// goes out on the next flush and the slot frees immediately. Main thread, never while
    /// a fill is open.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_disconnect_client", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportDisconnectClient( IntPtr server, int clientIndex );

    /// <summary>Number of connected clients.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_num_connected_clients", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportNumConnectedClients( IntPtr server );

    /// <summary>Nonzero when the slot holds a connected client.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_is_client_connected", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportIsClientConnected( IntPtr server, int clientIndex );

    /// <summary>The connected client's id from its connect token.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_client_id", CallingConvention = CallingConvention.Cdecl )]
    public static extern ulong ServerTransportClientId( IntPtr server, int clientIndex );

    /// <summary>The server's counters block; read it with <see cref="CountersSpan"/>.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_counters", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ServerTransportCounters( IntPtr server );

    /// <summary>Reads the server's whole-socket rate stats (rocketnet_server_transport_stats).</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_stats", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportGetStats( IntPtr server, out ServerTransportStats stats );

    [DllImport( Lib, EntryPoint = "rocketnet_counter_name", CallingConvention = CallingConvention.Cdecl )]
    static extern IntPtr counter_name( int counter );

    /// <summary>The name of a counter.</summary>
    public static string CounterName( Counter counter ) => Marshal.PtrToStringUTF8( counter_name( (int) counter ) ) ?? string.Empty;

    // -----------------------------------------
    //                              frame sync
    // -----------------------------------------

    /// <summary>Creates a frame sync pegging a headless loop to real time at the frame rate.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_frame_sync_create", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr FrameSyncCreate( IntPtr context, double frameRate );

    /// <summary>Destroys the frame sync.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_frame_sync_destroy", CallingConvention = CallingConvention.Cdecl )]
    public static extern void FrameSyncDestroy( IntPtr frameSync );

    /// <summary>Waits until the frame boundary. Call once per loop iteration.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_frame_sync_wait", CallingConvention = CallingConvention.Cdecl )]
    public static extern void FrameSyncWait( IntPtr frameSync );

    /// <summary>Reads the frame sync stats (rocketnet_frame_sync_stats).</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_frame_sync_stats", CallingConvention = CallingConvention.Cdecl )]
    public static extern void FrameSyncGetStats( IntPtr frameSync, out FrameSyncStats stats );

    // -----------------------------------------
    //                            span helpers
    // -----------------------------------------

    /// <summary>The payload span of an acquired packet: PayloadCapacity bytes at
    /// Frame + PayloadIndex. Empty when the context holds no frame.</summary>
    public static Span<byte> PayloadSpan( in PacketContext context )
    {
        return context.Frame == IntPtr.Zero
            ? Span<byte>.Empty
            : new Span<byte>( (byte*) context.Frame + context.PayloadIndex, context.PayloadCapacity );
    }

    /// <summary>A counters block as a span of <see cref="NumCounters"/> monotonic values,
    /// indexed by <see cref="Counter"/>.</summary>
    public static ReadOnlySpan<ulong> CountersSpan( IntPtr counters )
    {
        return counters == IntPtr.Zero
            ? ReadOnlySpan<ulong>.Empty
            : new ReadOnlySpan<ulong>( (void*) counters, NumCounters );
    }

    /// <summary>A client receive batch: zero-copy spans over the library's payload storage.
    /// Valid until the next update.</summary>
    public readonly ref struct ClientTransportPayloadBatch
    {
        readonly byte** data;
        readonly int* bytes;

        /// <summary>Number of payloads in the batch.</summary>
        public readonly int Count;

        internal ClientTransportPayloadBatch( int count, byte** data, int* bytes )
        {
            Count = count;
            this.data = data;
            this.bytes = bytes;
        }

        /// <summary>One payload of the batch.</summary>
        public ReadOnlySpan<byte> this[int index]
        {
            get
            {
                if ( (uint) index >= (uint) Count )
                    throw new IndexOutOfRangeException();
                return new ReadOnlySpan<byte>( data[index], bytes[index] );
            }
        }
    }

    /// <summary>A server receive batch: zero-copy spans over the library's payload storage,
    /// with each payload's sender. Valid until the next update.</summary>
    public readonly ref struct ServerTransportPayloadBatch
    {
        readonly int* clientIndex;
        readonly byte** data;
        readonly int* bytes;

        /// <summary>Number of payloads in the batch.</summary>
        public readonly int Count;

        internal ServerTransportPayloadBatch( int count, int* clientIndex, byte** data, int* bytes )
        {
            Count = count;
            this.clientIndex = clientIndex;
            this.data = data;
            this.bytes = bytes;
        }

        /// <summary>One payload of the batch.</summary>
        public ReadOnlySpan<byte> this[int index]
        {
            get
            {
                if ( (uint) index >= (uint) Count )
                    throw new IndexOutOfRangeException();
                return new ReadOnlySpan<byte>( data[index], bytes[index] );
            }
        }

        /// <summary>The sender of one payload of the batch.</summary>
        public int ClientIndex( int index )
        {
            if ( (uint) index >= (uint) Count )
                throw new IndexOutOfRangeException();
            return clientIndex[index];
        }
    }
}
