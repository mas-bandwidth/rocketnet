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
using Microsoft.Win32.SafeHandles;

/// <summary>Owns a native client transport. Passing it to a RocketNet call adds a reference for
/// the duration of that call, so a destroyed handle throws ObjectDisposedException before native
/// code runs. The runtime runs ReleaseHandle exactly once, when the handle is disposed.</summary>
public sealed class ClientTransportHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>Creates an empty handle. P/Invoke needs a public parameterless constructor to
    /// marshal a transport-returning native call; the native function never returns an invalid
    /// handle for a successful create.</summary>
    public ClientTransportHandle() : base( true ) { }

    /// <summary>Destroys the owned native client transport exactly once.</summary>
    /// <returns>Always true.</returns>
    protected override bool ReleaseHandle()
    {
        RocketNet.ClientTransportDestroyNative( handle );
        return true;
    }
}

/// <summary>Owns a native server transport. Passing it to a RocketNet call adds a reference for
/// the duration of that call, so a destroyed handle throws ObjectDisposedException before native
/// code runs. The runtime runs ReleaseHandle exactly once, when the handle is disposed.</summary>
public sealed class ServerTransportHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>Creates an empty handle. P/Invoke needs a public parameterless constructor to
    /// marshal a transport-returning native call; the native function never returns an invalid
    /// handle for a successful create.</summary>
    public ServerTransportHandle() : base( true ) { }

    /// <summary>Destroys the owned native server transport exactly once.</summary>
    /// <returns>Always true.</returns>
    protected override bool ReleaseHandle()
    {
        RocketNet.ServerTransportDestroyNative( handle );
        return true;
    }
}

/// <summary>The rocketnet C SDK surface: raw P/Invoke plus thin span helpers.</summary>
public static unsafe partial class RocketNet
{
    const string Lib = "rocketnet";

    static RocketNet()
    {
        NativeLibrary.SetDllImportResolver( typeof(RocketNet).Assembly, Resolve );
    }

    // -----------------------------------------
    //                               constants
    // -----------------------------------------

    /// <summary>Maximum client slots a server supports.</summary>
    public const int MaxClients = 1000;

    /// <summary>Largest payload one wire packet carries: the 1232-byte send budget less the
    /// fixed overhead and the minimum ack block, floored to a multiple of eight. A StartPacket
    /// request above this routes through the transport's arena and fragments at finish.</summary>
    public const int MaxPayloadBytes = 1192;

    /// <summary>Largest single-packet payload the receive calls can deliver: the receive side
    /// accepts up to the fixed 1426-byte ceiling whatever this end's own send budget. Reassembled
    /// fragmented payloads are larger -- size receive buffers by the configured maximum payload.</summary>
    public const int MaxReceivePayloadBytes = 1393;

    /// <summary>Default configured maximum payload: the largest payload a transport sends
    /// (fragmenting transparently) and reassembles, when the config leaves it zero.</summary>
    public const int DefaultMaxPayloadBytes = 16384;

    /// <summary>Default per-connection reassembly slot count, when the config leaves it zero.</summary>
    public const int DefaultReassemblySlots = 8;

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
    public const int NumCounters = 193;

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
        ConnectionResponseWrongSize,
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
        KeepAliveWrongSize,
        KeepAliveBadAckBlock,
        PayloadWrongSize,
        PayloadBadAckBlock,
        DisconnectWrongSize,
        DisconnectBadAckBlock,
        AckReceived,
        AckWrongSize,
        AckNoMapping,
        AckZeroSequence,
        AckAlreadyReceived,
        AckDidNotOpen,
        AckNotConnected,
        AckBadAckBlock,
        AckAccepted,
        AckUnknownSequence,
        AckedBatchFull,
        ClientKeepAliveWrongSize,
        ClientKeepAliveBadAckBlock,
        ClientPayloadWrongSize,
        ClientPayloadBadAckBlock,
        ClientDisconnectWrongSize,
        ClientDisconnectBadAckBlock,
        ClientAckReceived,
        ClientAckWrongState,
        ClientAckWrongSize,
        ClientAckZeroSequence,
        ClientAckAlreadyReceived,
        ClientAckDidNotOpen,
        ClientAckBadAckBlock,
        ClientAckAccepted,
        ClientAckUnknownSequence,
        ClientAckedBatchFull,
        KeepAliveWrongOpenedSize,
        PayloadWrongOpenedSize,
        DisconnectWrongOpenedSize,
        AckWrongOpenedSize,
        ClientKeepAliveWrongOpenedSize,
        ClientPayloadWrongOpenedSize,
        ClientDisconnectWrongOpenedSize,
        ClientAckWrongOpenedSize,
        FragmentReceived,
        FragmentWrongSize,
        FragmentNoMapping,
        FragmentZeroSequence,
        FragmentAlreadyReceived,
        FragmentDidNotOpen,
        FragmentNotConnected,
        FragmentBadAckBlock,
        FragmentWrongOpenedSize,
        FragmentZeroPayloadId,
        FragmentStale,
        FragmentCountInvalid,
        FragmentIndexOutOfRange,
        FragmentCountMismatch,
        FragmentSizeMismatch,
        FragmentPayloadTooLarge,
        FragmentDuplicateIndex,
        FragmentAccepted,
        FragmentPayloadReassembled,
        FragmentPayloadAbandoned,
        FragmentPayloadBatchFull,
        ClientFragmentReceived,
        ClientFragmentWrongState,
        ClientFragmentWrongSize,
        ClientFragmentZeroSequence,
        ClientFragmentAlreadyReceived,
        ClientFragmentDidNotOpen,
        ClientFragmentBadAckBlock,
        ClientFragmentWrongOpenedSize,
        ClientFragmentZeroPayloadId,
        ClientFragmentStale,
        ClientFragmentCountInvalid,
        ClientFragmentIndexOutOfRange,
        ClientFragmentCountMismatch,
        ClientFragmentSizeMismatch,
        ClientFragmentPayloadTooLarge,
        ClientFragmentDuplicateIndex,
        ClientFragmentAccepted,
        ClientFragmentPayloadReassembled,
        ClientFragmentPayloadAbandoned,
        ClientFragmentPayloadBatchFull,
        SendArenaFull,
        ReceiveCursorTooSmall,
        SendHistoryOverflow,
        SendLeaseFull,
        IntegrityLimitReached,
        ClientCookieWrongSize,
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

        /// <summary>Largest payload this client sends (fragmenting transparently) and
        /// reassembles; zero means <see cref="DefaultMaxPayloadBytes"/>.</summary>
        public int MaxPayloadBytes;

        /// <summary>Reassembly slots for the connection; zero means <see cref="DefaultReassemblySlots"/>.</summary>
        public int NumReassemblySlots;

        /// <summary>The driver candidate list (#106), a NUL-terminated ASCII string of up to
        /// <see cref="DriverStringLength"/> - 1 characters: an ordered comma-separated list
        /// tried left to right at create, first grant wins ("auto" walks the platform's
        /// recommended order; an applicable one-element list is a pin that refuses loudly;
        /// a list whose every name is inapplicable to this endpoint falls to auto, loudly).
        /// Read and write it through <see cref="GetDriver"/> / <see cref="SetDriver"/>.</summary>
        public fixed byte Driver[DriverStringLength];

        /// <summary>Native function pointer for void (*)( void * context, int previous_state,
        /// int current_state ), or IntPtr.Zero for none. Delegate marshaling is deliberately
        /// not built here: the callback fires on library threads, and the pinning and lifetime
        /// rules that makes safe are not worth the surface in v1 — poll <see cref="ClientTransportState(ClientTransportHandle)"/>.</summary>
        public IntPtr StateChangeCallback;

        /// <summary>Reads the driver candidate list as a string.</summary>
        public string GetDriver() { fixed ( byte * driver = Driver ) return Marshal.PtrToStringUTF8( (IntPtr) driver ) ?? string.Empty; }

        /// <summary>Writes the driver candidate list. Throws when the value cannot fit —
        /// mirroring the C surface, which refuses loudly and never truncates.</summary>
        public void SetDriver( string value ) { fixed ( byte * driver = Driver ) WriteFixedString( driver, DriverStringLength, value, "driver list" ); }
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
        /// <summary>The staged drivers' queue count at runtime -- XDP queues on an xdp grant,
        /// reuseport sockets on a gso grant; zero means the driver default. Bounds are per
        /// driver, refused loudly at the rung (gso refuses past 32). The portable driver
        /// ignores it.</summary>
        public int NumXdpQueues;
        /// <summary>The xdp driver's OS cpu count (its workers pin from this core upward);
        /// zero means the driver default. The other drivers ignore it.</summary>
        public int NumOsCpus;

        /// <summary>Largest payload this server sends (fragmenting transparently) and
        /// reassembles per client; zero means <see cref="DefaultMaxPayloadBytes"/>.</summary>
        public int MaxPayloadBytes;

        /// <summary>Reassembly slots per client slot; zero means <see cref="DefaultReassemblySlots"/>.</summary>
        public int NumReassemblySlots;

        /// <summary>The driver candidate list (#106), a NUL-terminated ASCII string of up to
        /// <see cref="DriverStringLength"/> - 1 characters: an ordered comma-separated list
        /// tried left to right at create, first grant wins — on linux the auto ladder is
        /// xdp,gso,portable — and an applicable one-element list is a pin that refuses
        /// loudly rather than falling. Known names absent on the platform or endpoint are
        /// skipped softly (a list of nothing but skips falls to auto, loudly); unknown
        /// names refuse the create at parse. Read and write it through
        /// <see cref="GetDriver"/> / <see cref="SetDriver"/>.</summary>
        public fixed byte Driver[DriverStringLength];

        /// <summary>The server's public address (#110 review), a NUL-terminated string of up
        /// to <see cref="AddressStringLength"/> - 1 characters; empty (the default) means
        /// "use the create call's address parameter". When set — struct, or the
        /// ROCKETNET_PUBLIC_ADDRESS env var (deprecated alias ROCKETNET_SERVER_PUBLIC_ADDRESS)
        /// — it displaces that parameter before anything reads it, so the address the driver
        /// ladder verifies, the address the granted driver binds, and the address connect
        /// tokens validate against are always the same one. Read and write through
        /// <see cref="GetPublicAddress"/> / <see cref="SetPublicAddress"/>.</summary>
        public fixed byte PublicAddress[AddressStringLength];

        /// <summary>Connection timeout in seconds, 1 to 3600; the native default is 60.</summary>
        public int MaxTimeoutSeconds;

        /// <summary>Reads the driver candidate list as a string.</summary>
        public string GetDriver() { fixed ( byte * driver = Driver ) return Marshal.PtrToStringUTF8( (IntPtr) driver ) ?? string.Empty; }

        /// <summary>Writes the driver candidate list. Throws when the value cannot fit —
        /// mirroring the C surface, which refuses loudly and never truncates.</summary>
        public void SetDriver( string value ) { fixed ( byte * driver = Driver ) WriteFixedString( driver, DriverStringLength, value, "driver list" ); }

        /// <summary>Reads the public address override as a string.</summary>
        public string GetPublicAddress() { fixed ( byte * address = PublicAddress ) return Marshal.PtrToStringUTF8( (IntPtr) address ) ?? string.Empty; }

        /// <summary>Writes the public address override. Throws when the value cannot fit —
        /// mirroring the C surface, which refuses loudly and never truncates.</summary>
        public void SetPublicAddress( string value ) { fixed ( byte * address = PublicAddress ) WriteFixedString( address, AddressStringLength, value, "public address" ); }
    }

    /// <summary>rocketnet.h ROCKETNET_DRIVER_STRING_LENGTH: the driver field's capacity,
    /// terminator included.</summary>
    public const int DriverStringLength = 64;

    /// <summary>rocketnet.h ROCKETNET_MAX_ADDRESS_STRING_LENGTH: the public address field's
    /// capacity, terminator included.</summary>
    public const int AddressStringLength = 256;

    static void WriteFixedString( byte * buffer, int capacity, string value, string what )
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes( value ?? string.Empty );
        if ( bytes.Length >= capacity )
            throw new ArgumentException( $"{what} is too long ({capacity - 1} character maximum)" );
        for ( int i = 0; i < bytes.Length; i++ )
            buffer[i] = bytes[i];
        buffer[bytes.Length] = 0;
    }

    /// <summary>rocketnet_packet_context_t. Filled by StartPacket, consumed by FinishPacket or
    /// AbortPacket. The fields are read-only to the caller; the payload is written at
    /// Frame + PayloadIndex, up to PayloadCapacity bytes — use the checked client/server packet payload span accessor.</summary>
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
    /// the trailing second of updates. rtt seeds from the handshake's round trips and is
    /// continuously refreshed by the acks, as the C header says.</summary>
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

    /// <summary>rocketnet_server_transport_client_stats_t. Read with <see cref="ServerTransportGetClientStats"/>.
    /// Per-client connection quality, per slot: rtt and jitter in milliseconds, packet loss as
    /// the percentage of this end's sent packets never acknowledged, and the trailing bandwidth
    /// sent, received and acknowledged. Fed by the ack blocks every session packet carries;
    /// recomputed round-robin, a sixth of the slots per update, every slot covered.</summary>
    [StructLayout( LayoutKind.Sequential )]
    public struct ServerTransportClientStats
    {
        /// <summary>Round trip time in milliseconds.</summary>
        public float Rtt;
        /// <summary>Deviation of rtt samples above the minimum, in milliseconds.</summary>
        public float Jitter;
        /// <summary>Percent of sent packets never acknowledged.</summary>
        public float PacketLoss;
        /// <summary>Bandwidth sent to this client, kilobits per second.</summary>
        public float BandwidthSentKbps;
        /// <summary>Bandwidth received from this client, kilobits per second.</summary>
        public float BandwidthReceivedKbps;
        /// <summary>Bandwidth acknowledged by this client, kilobits per second.</summary>
        public float BandwidthAckedKbps;
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
    public static int WriteDevConnectToken( string serverAddress, ulong clientId, ulong unixTimestamp, byte[] connectToken )
    {
        ArgumentNullException.ThrowIfNull( connectToken );
        return WriteDevConnectToken( serverAddress, clientId, unixTimestamp, connectToken.AsSpan() );
    }

    /// <summary>Mints a development token into a buffer of exactly <see cref="ConnectTokenBytes"/> bytes.</summary>
    public static int WriteDevConnectToken( string serverAddress, ulong clientId, ulong unixTimestamp, Span<byte> connectToken )
    {
        ArgumentNullException.ThrowIfNull( serverAddress );
        ValidateConnectTokenLength( connectToken.Length );
        fixed ( byte* token = connectToken )
            return WriteDevConnectTokenNative( serverAddress, clientId, unixTimestamp, token );
    }

    [DllImport( Lib, EntryPoint = "rocketnet_write_dev_connect_token", CallingConvention = CallingConvention.Cdecl )]
    private static extern int WriteDevConnectTokenNative( [MarshalAs( UnmanagedType.LPUTF8Str )] string serverAddress, ulong clientId, ulong unixTimestamp, byte* connectToken );

    /// <summary>Creates a client connecting with the token. The token names the server, carries
    /// the keys, and is the credential. time is the game's clock, a double in seconds,
    /// monotonic; pass the same clock to every call that takes one. On refusal the returned
    /// handle has IsInvalid true — an invalid token refuses here.</summary>
    public static ClientTransportHandle ClientTransportCreate( IntPtr context, byte[] connectToken, ref ClientTransportConfig config, double time )
    {
        ArgumentNullException.ThrowIfNull( connectToken );
        return ClientTransportCreate( context, (ReadOnlySpan<byte>) connectToken, ref config, time );
    }

    /// <summary>Creates a client from a token of exactly <see cref="ConnectTokenBytes"/> bytes.</summary>
    public static ClientTransportHandle ClientTransportCreate( IntPtr context, ReadOnlySpan<byte> connectToken, ref ClientTransportConfig config, double time )
    {
        ValidateConnectTokenLength( connectToken.Length );
        fixed ( byte* token = connectToken )
            return ClientTransportCreateNative( context, token, ref config, time );
    }

    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_create", CallingConvention = CallingConvention.Cdecl )]
    private static extern ClientTransportHandle ClientTransportCreateNative( IntPtr context, byte* connectToken, ref ClientTransportConfig config, double time );

    /// <summary>Creates a client with the default (development) config.</summary>
    public static ClientTransportHandle ClientTransportCreate( IntPtr context, byte[] connectToken, IntPtr nullConfig, double time )
    {
        ArgumentNullException.ThrowIfNull( connectToken );
        return ClientTransportCreate( context, (ReadOnlySpan<byte>) connectToken, nullConfig, time );
    }

    /// <summary>Creates a client with default config from a token of exactly <see cref="ConnectTokenBytes"/> bytes.</summary>
    public static ClientTransportHandle ClientTransportCreate( IntPtr context, ReadOnlySpan<byte> connectToken, IntPtr nullConfig, double time )
    {
        ValidateConnectTokenLength( connectToken.Length );
        fixed ( byte* token = connectToken )
            return ClientTransportCreateNative( context, token, nullConfig, time );
    }

    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_create", CallingConvention = CallingConvention.Cdecl )]
    private static extern ClientTransportHandle ClientTransportCreateNative( IntPtr context, byte* connectToken, IntPtr nullConfig, double time );

    private static void ValidateConnectTokenLength( int length )
    {
        if ( length != ConnectTokenBytes )
            throw new ArgumentException( $"A connect token must contain exactly {ConnectTokenBytes} bytes.", "connectToken" );
    }

    /// <summary>Destroys the client. Disconnect and pump a few updates first. Destroy runs
    /// exactly once: a second call throws ObjectDisposedException before native code runs.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_destroy", CallingConvention = CallingConvention.Cdecl )]
    internal static extern void ClientTransportDestroyNative( IntPtr client );

    /// <summary>Destroys the client. Disconnect and pump a few updates first. The owned native
    /// transport is destroyed exactly once; the handle is closed afterwards.</summary>
    public static void ClientTransportDestroy( ClientTransportHandle client )
    {
        ArgumentNullException.ThrowIfNull( client );
        if ( client.IsClosed )
            throw new ObjectDisposedException( nameof(ClientTransportHandle) );
        client.Dispose();
    }

    /// <summary>Advances the client one frame at the given game time.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_update", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportUpdate( ClientTransportHandle client, double time );

    /// <summary>The convenience send: copies and flushes inline. Main thread only, never
    /// during an open fill — the simple form, not a fill-phase participant.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_send_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportSendPacket( ClientTransportHandle client, byte* packetData, int packetBytes );

    /// <summary>The convenience send over a span.</summary>
    public static void ClientTransportSendPacket( ClientTransportHandle client, ReadOnlySpan<byte> payload )
    {
        fixed ( byte* p = payload )
            ClientTransportSendPacket( client, p, payload.Length );
    }

    /// <summary>The copying receive cursor: copies the next received payload that fits the
    /// buffer and returns its size, or zero when drained. A payload larger than the buffer --
    /// possible only for reassembled fragmented payloads -- is refused whole and counted on
    /// <see cref="Counter.ReceiveCursorTooSmall"/>, never truncated. Size the buffer by
    /// <see cref="ClientTransportMaxPayloadBytes"/> to never refuse.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_receive_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportReceivePacket( ClientTransportHandle client, byte* packetData, int packetCapacity );

    // the guard's configured maximum, cached so the wrapper costs one
    // P/Invoke per client rather than one per receive call. The pair is
    // written without synchronization on purpose: a torn read across
    // threads only mis-sizes the guard, and the native call is safe at
    // any capacity -- an oversize payload is refused and counted there.

    static ClientTransportHandle? cachedMaxPayloadClient;
    static int cachedMaxPayloadBytes;

    /// <summary>The copying receive cursor over a span: requires the configured maximum
    /// payload so nothing is ever refused through this wrapper.</summary>
    public static int ClientTransportReceivePacket( ClientTransportHandle client, Span<byte> buffer )
    {
        if ( !ReferenceEquals( client, cachedMaxPayloadClient ) )
        {
            cachedMaxPayloadBytes = ClientTransportMaxPayloadBytes( client );
            cachedMaxPayloadClient = client;
        }
        if ( buffer.Length < cachedMaxPayloadBytes )
            throw new ArgumentException( "buffer must hold ClientTransportMaxPayloadBytes(client)", nameof(buffer) );
        fixed ( byte* p = buffer )
            return ClientTransportReceivePacket( client, p, buffer.Length );
    }

    /// <summary>The configured maximum payload this client sends (fragmenting transparently)
    /// and reassembles -- the config's MaxPayloadBytes after defaulting. Size receive
    /// buffers by this.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_max_payload_bytes", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportMaxPayloadBytes( ClientTransportHandle client );

    /// <summary>Number of payloads delivered by the last update. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_num_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportNumPayloads( ClientTransportHandle client );

    /// <summary>One delivered payload by index. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_payload", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportPayload( ClientTransportHandle client, int payloadIndex, out int payloadBytes );

    /// <summary>One delivered payload by index as a span. Valid until the next update.</summary>
    public static ReadOnlySpan<byte> ClientTransportPayloadSpan( ClientTransportHandle client, int payloadIndex )
    {
        IntPtr data = ClientTransportPayload( client, payloadIndex, out int bytes );
        return data == IntPtr.Zero ? ReadOnlySpan<byte>.Empty : new ReadOnlySpan<byte>( (void*) data, bytes );
    }

    /// <summary>The whole receive batch as parallel arrays in one call — the zero-per-item
    /// form. Valid until the next update. Returns the number of payloads.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportPayloads( ClientTransportHandle client, out byte** payloadData, out int* payloadBytes );

    /// <summary>The receive batch as a zero-copy indexed view — one interop crossing.
    /// Valid until the next update.</summary>
    public static ClientTransportPayloadBatch ClientTransportPayloads( ClientTransportHandle client )
    {
        int count = ClientTransportPayloads( client, out byte** data, out int* bytes );
        return new ClientTransportPayloadBatch( count, data, bytes );
    }

    /// <summary>Acquires a packet to fill, declaring the capacity the caller needs; see the
    /// send family contract on <see cref="ServerTransportStartPacket"/>. A request above
    /// <see cref="MaxPayloadBytes"/> routes to the arena and fragments at finish. The
    /// context's PayloadCapacity reports the frame's real capacity -- the wire capacity or
    /// the configured maximum, never the request itself. Returns the frame, or IntPtr.Zero
    /// when no packet is available (ring or arena full, not connected, request out of range).</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_start_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportStartPacket( ClientTransportHandle client, int requestBytes, ref PacketContext context );

    /// <summary>Seals and stages the filled packet in place. Returns the sealed wire size, or
    /// zero when the packet was dropped instead (payload out of range, or the connection
    /// gone) — the sequence is burned either way, which the replay window tolerates.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_finish_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportFinishPacket( ClientTransportHandle client, ref PacketContext context, int payloadBytes );

    /// <summary>Releases an acquired packet without sending it.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_abort_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportAbortPacket( ClientTransportHandle client, ref PacketContext context );

    /// <summary>Flushes everything staged. Required once per frame, after update; see the
    /// send family contract on <see cref="ServerTransportStartPacket"/>.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_send_packets", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportSendPackets( ClientTransportHandle client );

    /// <summary>The client state, one of the ClientTransportState constants.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_state", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportState( ClientTransportHandle client );

    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_driver", CallingConvention = CallingConvention.Cdecl )]
    static extern IntPtr client_transport_driver( ClientTransportHandle client );

    /// <summary>The granted driver, request-vs-granted style (#106): the name of the candidate
    /// that won the ladder at create ("portable", "msgx"), never the requested list.</summary>
    public static string ClientTransportDriver( ClientTransportHandle client ) => Marshal.PtrToStringUTF8( client_transport_driver( client ) ) ?? string.Empty;

    /// <summary>The client's slot index on the server, -1 when not connected.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_client_index", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportClientIndex( ClientTransportHandle client );

    /// <summary>Disconnects deliberately. Main thread, never while a fill is open.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_disconnect", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportDisconnect( ClientTransportHandle client );

    /// <summary>The client's counters block; read it with <see cref="CountersSpan"/>.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_counters", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ClientTransportCounters( ClientTransportHandle client );

    /// <summary>Reads the client's connection quality stats (rocketnet_client_transport_stats).</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_stats", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ClientTransportGetStats( ClientTransportHandle client, out ClientTransportStats stats );

    /// <summary>The client's delivery notification batch: packet ids newly acknowledged by the
    /// last update, valid until the next update. The packet id is the one in
    /// <see cref="PacketContext.PacketId"/> from StartPacket. Returns the batch count.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_acked_packets", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ClientTransportAckedPackets( ClientTransportHandle client, out ulong* packetIds );

    /// <summary>The client's acked packet ids as a span -- one interop crossing.
    /// Valid until the next update.</summary>
    public static ReadOnlySpan<ulong> ClientTransportAckedPacketsSpan( ClientTransportHandle client )
    {
        int count = ClientTransportAckedPackets( client, out ulong* packetIds );
        return count == 0 ? ReadOnlySpan<ulong>.Empty : new ReadOnlySpan<ulong>( packetIds, count );
    }

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

    /// <summary>Constructor allocator requests plus XDP UMEM for one built-in driver.
    /// Excludes OS/kernel/application memory and environment overrides. Zero means
    /// invalid geometry, overflow, or an unknown/unavailable driver; use a concrete name.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_required_memory", CallingConvention = CallingConvention.Cdecl )]
    public static extern ulong ServerTransportRequiredMemory( ref ServerTransportConfig config, [MarshalAs( UnmanagedType.LPUTF8Str )] string driver );

    /// <summary>Creates a server on the public address clients dial, e.g. "127.0.0.1:40000".
    /// On refusal the returned handle has IsInvalid true.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_create", CallingConvention = CallingConvention.Cdecl )]
    public static extern ServerTransportHandle ServerTransportCreate( IntPtr context, [MarshalAs( UnmanagedType.LPUTF8Str )] string publicAddress, ref ServerTransportConfig config, double time );

    /// <summary>Destroys the server. Stop and wait for stopped first. Destroy runs exactly once:
    /// a second call throws ObjectDisposedException before native code runs.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_destroy", CallingConvention = CallingConvention.Cdecl )]
    internal static extern void ServerTransportDestroyNative( IntPtr server );

    /// <summary>Destroys the server. Stop and wait for stopped first. The owned native transport
    /// is destroyed exactly once; the handle is closed afterwards.</summary>
    public static void ServerTransportDestroy( ServerTransportHandle server )
    {
        ArgumentNullException.ThrowIfNull( server );
        if ( server.IsClosed )
            throw new ObjectDisposedException( nameof(ServerTransportHandle) );
        server.Dispose();
    }

    /// <summary>Begins the stop: redundant disconnects go out, then the state reaches stopped.
    /// Main thread, never while a fill is open.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_stop", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportStop( ServerTransportHandle server );

    /// <summary>The server state, one of the Server state constants.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_state", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportState( ServerTransportHandle server );

    /// <summary>The commit barrier for host processes that must never commit an XDP data
    /// plane (test harnesses, smokes): installed once, the xdp driver's create refuses
    /// before its first act; the non-mutating pre-flight stays reachable. A no-op on
    /// platforms without the xdp driver.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_forbid_xdp_commit", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ForbidXdpCommit();

    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_driver", CallingConvention = CallingConvention.Cdecl )]
    static extern IntPtr server_transport_driver( ServerTransportHandle server );

    /// <summary>The granted driver, request-vs-granted style (#106): the name of the candidate
    /// that won the ladder at create ("xdp", "gso", "msgx", "portable"), never the requested
    /// list. Performance claims attach to what this returns.</summary>
    public static string ServerTransportDriver( ServerTransportHandle server ) => Marshal.PtrToStringUTF8( server_transport_driver( server ) ) ?? string.Empty;

    /// <summary>The server's actual bound UDP port, including a port selected from zero.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_port", CallingConvention = CallingConvention.Cdecl )]
    public static extern ushort ServerTransportPort( ServerTransportHandle server );

    /// <summary>Advances the server one frame at the given game time.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_update", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportUpdate( ServerTransportHandle server, double time );

    /// <summary>Number of payloads delivered by the last update. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_num_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportNumPayloads( ServerTransportHandle server );

    /// <summary>One delivered payload by index, with its sender. Valid until the next update.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_payload", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ServerTransportPayload( ServerTransportHandle server, int payloadIndex, out int clientIndex, out int payloadBytes );

    /// <summary>One delivered payload by index as a span, with its sender. Valid until the next update.</summary>
    public static ReadOnlySpan<byte> ServerTransportPayloadSpan( ServerTransportHandle server, int payloadIndex, out int clientIndex )
    {
        IntPtr data = ServerTransportPayload( server, payloadIndex, out clientIndex, out int bytes );
        return data == IntPtr.Zero ? ReadOnlySpan<byte>.Empty : new ReadOnlySpan<byte>( (void*) data, bytes );
    }

    /// <summary>The whole receive batch as parallel arrays in one call — the zero-per-item
    /// form. Valid until the next update. Returns the number of payloads.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_payloads", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportPayloads( ServerTransportHandle server, out int* clientIndex, out byte** payloadData, out int* payloadBytes );

    /// <summary>The receive batch as a zero-copy indexed view — one interop crossing.
    /// Valid until the next update.</summary>
    public static ServerTransportPayloadBatch ServerTransportPayloads( ServerTransportHandle server )
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

    /// <summary>Acquires a packet to fill for one client, declaring the capacity the caller
    /// needs; the threading contract is in the comment above. A request above
    /// <see cref="MaxPayloadBytes"/> routes to the arena and fragments at finish -- the
    /// context's one PacketId surfaces acknowledged when every fragment's wire packet is
    /// acknowledged, and its PayloadCapacity reports the frame's real capacity (the wire
    /// capacity or the configured maximum), never the request itself. Returns the frame, or
    /// IntPtr.Zero when no packet is available.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_start_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ServerTransportStartPacket( ServerTransportHandle server, int clientIndex, int requestBytes, ref PacketContext context );

    /// <summary>Seals and stages the filled packet in place. Returns the sealed wire size, or
    /// zero when the packet was dropped instead (payload out of range, or the connection
    /// gone) — the sequence is burned either way, which the replay window tolerates.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_finish_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportFinishPacket( ServerTransportHandle server, ref PacketContext context, int payloadBytes );

    /// <summary>Releases an acquired packet without sending it.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_abort_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportAbortPacket( ServerTransportHandle server, ref PacketContext context );

    /// <summary>Flushes everything staged. Required once per frame, after update; see the
    /// send family contract above.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_send_packets", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportSendPackets( ServerTransportHandle server );

    /// <summary>The convenience send: copies into an acquired packet and stages it.
    /// Flushed by the frame's SendPackets.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_send_packet", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportSendPacket( ServerTransportHandle server, int clientIndex, byte* payloadData, int payloadBytes );

    /// <summary>The convenience send over a span.</summary>
    public static void ServerTransportSendPacket( ServerTransportHandle server, int clientIndex, ReadOnlySpan<byte> payload )
    {
        fixed ( byte* p = payload )
            ServerTransportSendPacket( server, clientIndex, p, payload.Length );
    }

    /// <summary>Disconnects one connected client deliberately: the redundant disconnect burst
    /// goes out on the next flush and the slot frees immediately. Main thread, never while
    /// a fill is open.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_disconnect_client", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportDisconnectClient( ServerTransportHandle server, int clientIndex );

    /// <summary>Number of connected clients.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_num_connected_clients", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportNumConnectedClients( ServerTransportHandle server );

    /// <summary>Nonzero when the slot holds a connected client.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_is_client_connected", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportIsClientConnected( ServerTransportHandle server, int clientIndex );

    /// <summary>The connected client's id from its connect token.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_client_id", CallingConvention = CallingConvention.Cdecl )]
    public static extern ulong ServerTransportClientId( ServerTransportHandle server, int clientIndex );

    /// <summary>The server's counters block; read it with <see cref="CountersSpan"/>.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_counters", CallingConvention = CallingConvention.Cdecl )]
    public static extern IntPtr ServerTransportCounters( ServerTransportHandle server );

    /// <summary>The configured maximum payload this server sends (fragmenting transparently)
    /// and reassembles per client -- the config's MaxPayloadBytes after defaulting. Size the
    /// buffers payloads are copied into by this.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_max_payload_bytes", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportMaxPayloadBytes( ServerTransportHandle server );

    /// <summary>Reads the server's whole-socket rate stats (rocketnet_server_transport_stats).</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_stats", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportGetStats( ServerTransportHandle server, out ServerTransportStats stats );

    /// <summary>Reads one slot's connection quality stats (rocketnet_server_transport_client_stats).
    /// An out-of-range slot reads as zeros.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_client_stats", CallingConvention = CallingConvention.Cdecl )]
    public static extern void ServerTransportGetClientStats( ServerTransportHandle server, int clientIndex, out ServerTransportClientStats stats );

    /// <summary>The delivery notification batch: packet ids newly acknowledged by the last
    /// update, as parallel arrays with the client index of each. Valid until the next update.
    /// The packet id is the one in <see cref="PacketContext.PacketId"/> from StartPacket --
    /// the join key back to whatever the game sent. Returns the batch count.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_acked_packets", CallingConvention = CallingConvention.Cdecl )]
    public static extern int ServerTransportAckedPackets( ServerTransportHandle server, out int* clientIndex, out ulong* packetIds );

    /// <summary>The server's acked batch as a zero-copy indexed view -- one interop crossing.
    /// Valid until the next update.</summary>
    public static ServerTransportAckedBatch ServerTransportAckedPackets( ServerTransportHandle server )
    {
        int count = ServerTransportAckedPackets( server, out int* clientIndex, out ulong* packetIds );
        return new ServerTransportAckedBatch( count, clientIndex, packetIds );
    }

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

    [DllImport( Lib, EntryPoint = "rocketnet_client_transport_packet_payload", CallingConvention = CallingConvention.Cdecl )]
    static extern byte* ClientPacketPayloadNative( ClientTransportHandle client, in PacketContext context, out int capacity );

    [DllImport( Lib, EntryPoint = "rocketnet_server_transport_packet_payload", CallingConvention = CallingConvention.Cdecl )]
    static extern byte* ServerPacketPayloadNative( ServerTransportHandle server, in PacketContext context, out int capacity );

    /// <summary>Validates the packet's owner, metadata and active lease before exposing memory.
    /// Empty for invalid contexts. The span expires at finish, abort, update, flush or destroy;
    /// callers must not retain it across those operations or race them.</summary>
    public static Span<byte> ClientTransportPacketPayloadSpan( ClientTransportHandle client, in PacketContext context )
    {
        byte* payload = ClientPacketPayloadNative(client, in context, out int capacity);
        return payload == null ? Span<byte>.Empty : new Span<byte>(payload, capacity);
    }

    /// <summary>The server counterpart of <see cref="ClientTransportPacketPayloadSpan"/>,
    /// with the same lease validation and lifetime.</summary>
    public static Span<byte> ServerTransportPacketPayloadSpan( ServerTransportHandle server, in PacketContext context )
    {
        byte* payload = ServerPacketPayloadNative(server, in context, out int capacity);
        return payload == null ? Span<byte>.Empty : new Span<byte>(payload, capacity);
    }

    /// <summary>Number of counters in the loaded native library.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_num_counters", CallingConvention = CallingConvention.Cdecl )]
    public static extern int CounterCount();

    /// <summary>Reads a counter outside update/fill; an invalid index or null block yields zero.</summary>
    [DllImport( Lib, EntryPoint = "rocketnet_counter_value", CallingConvention = CallingConvention.Cdecl )]
    public static extern ulong CounterValue( IntPtr counters, int counter );

    /// <summary>A counters block as a span of <see cref="NumCounters"/> monotonic values,
    /// indexed by <see cref="Counter"/>.</summary>
    public static ReadOnlySpan<ulong> CountersSpan( IntPtr counters )
    {
        return counters == IntPtr.Zero
            ? ReadOnlySpan<ulong>.Empty
            : new ReadOnlySpan<ulong>( (void*) counters, CounterCount() );
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

    /// <summary>The server's acked batch: packet ids newly acknowledged by the last update,
    /// with the client index of each. Valid until the next update.</summary>
    public readonly ref struct ServerTransportAckedBatch
    {
        readonly int * clientIndex;
        readonly ulong * packetIds;

        /// <summary>Number of newly acknowledged packets in the batch.</summary>
        public readonly int Count;

        internal ServerTransportAckedBatch( int count, int * clientIndex, ulong * packetIds )
        {
            Count = count;
            this.clientIndex = clientIndex;
            this.packetIds = packetIds;
        }

        /// <summary>One acknowledged packet id of the batch.</summary>
        public ulong PacketId( int index )
        {
            if ( (uint) index >= (uint) Count )
                throw new IndexOutOfRangeException();
            return packetIds[index];
        }

        /// <summary>The destination client of one acknowledged packet of the batch.</summary>
        public int ClientIndex( int index )
        {
            if ( (uint) index >= (uint) Count )
                throw new IndexOutOfRangeException();
            return clientIndex[index];
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
