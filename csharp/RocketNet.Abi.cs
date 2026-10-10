/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

public static unsafe partial class RocketNet
{
    /// <summary>SDK calling/layout revision, independent of the wire version.</summary>
    public const uint AbiVersion = 1;

    // Lazy: invalid managed token buffers still fail without loading native code.
    // Retain the accepted library for the process lifetime, as P/Invoke does.
    static readonly Lazy<IntPtr> nativeLibraryHandle = new Lazy<IntPtr>( LoadAndValidate );

    static IntPtr Resolve( string name, Assembly assembly, DllImportSearchPath? searchPath )
        => name == Lib ? nativeLibraryHandle.Value : IntPtr.Zero;

    /// <summary>Load and validate the native ABI without initializing RocketNet.
    /// Every P/Invoke also performs this check automatically on first resolution.
    /// Throws BadImageFormatException for an incompatible library.</summary>
    public static void ValidateAbi() => _ = nativeLibraryHandle.Value;

    static IntPtr LoadAndValidate()
    {
        string fileName = OperatingSystem.IsWindows() ? "rocketnet.dll"
                        : OperatingSystem.IsMacOS() ? "librocketnet.dylib" : "librocketnet.so";
        string? env = Environment.GetEnvironmentVariable( "ROCKETNET_LIBRARY_PATH" );
        IntPtr handle;
        if ( env != null )
        {
            string candidate = Directory.Exists( env ) ? Path.Combine( env, fileName ) : env;
            // An explicit selection is authoritative: never silently use another library.
            handle = NativeLibrary.Load( Path.GetFullPath( candidate ) );
        }
        else
        {
            string local = Path.Combine( AppContext.BaseDirectory, fileName );
            handle = File.Exists( local ) ? NativeLibrary.Load( local )
                : NativeLibrary.Load( Lib, typeof(RocketNet).Assembly, null );
        }
        try
        {
            CheckNativeAbi( handle );
            return handle;
        }
        catch
        {
            NativeLibrary.Free( handle );
            throw;
        }
    }

    [UnmanagedFunctionPointer( CallingConvention.Cdecl )]
    delegate uint AbiVersionQuery();
    [UnmanagedFunctionPointer( CallingConvention.Cdecl )]
    delegate nuint AbiTypeSizeQuery( [MarshalAs(UnmanagedType.LPUTF8Str)] string type );
    [UnmanagedFunctionPointer( CallingConvention.Cdecl )]
    delegate int AbiFieldCountQuery( [MarshalAs(UnmanagedType.LPUTF8Str)] string type );
    [UnmanagedFunctionPointer( CallingConvention.Cdecl )]
    delegate nuint AbiFieldQuery( [MarshalAs(UnmanagedType.LPUTF8Str)] string type,
                                 [MarshalAs(UnmanagedType.LPUTF8Str)] string field );
    [UnmanagedFunctionPointer( CallingConvention.Cdecl )]
    delegate int NumCountersQuery();

    static T AbiExport<T>( IntPtr handle, string name ) where T : Delegate
    {
        if ( !NativeLibrary.TryGetExport( handle, name, out IntPtr address ) )
            throw new BadImageFormatException( $"RocketNet ABI: missing {name}; rebuild the native library and binding together." );
        return Marshal.GetDelegateForFunctionPointer<T>( address );
    }

    static string NativeFieldName( string name )
    {
        if ( name == nameof(ServerTransportConfig.MaxClientSlots) ) return "max_clients";
        var result = new StringBuilder();
        for ( int i = 0; i < name.Length; ++i )
        {
            if ( i > 0 && char.IsUpper(name[i]) ) result.Append('_');
            result.Append(char.ToLowerInvariant(name[i]));
        }
        return result.ToString();
    }

    static void CheckNativeAbi( IntPtr handle )
    {
        uint version = AbiExport<AbiVersionQuery>( handle, "rocketnet_abi_version" )();
        if ( version != AbiVersion )
            throw new BadImageFormatException( $"RocketNet ABI: native revision {version}, binding requires {AbiVersion}." );
        int nativeCounters = AbiExport<NumCountersQuery>( handle, "rocketnet_num_counters" )();
        if ( nativeCounters != NumCounters )
            throw new BadImageFormatException( $"RocketNet ABI: native counter count {nativeCounters}, binding requires {NumCounters}." );
        var typeSize = AbiExport<AbiTypeSizeQuery>( handle, "rocketnet_abi_type_size" );
        var fieldCount = AbiExport<AbiFieldCountQuery>( handle, "rocketnet_abi_field_count" );
        var fieldOffset = AbiExport<AbiFieldQuery>( handle, "rocketnet_abi_field_offset" );
        var fieldSize = AbiExport<AbiFieldQuery>( handle, "rocketnet_abi_field_size" );
        var types = new (Type Managed, string Native)[] {
            (typeof(ClientTransportConfig), "rocketnet_client_transport_config_t"),
            (typeof(ServerTransportConfig), "rocketnet_server_transport_config_t"),
            (typeof(PacketContext), "rocketnet_packet_context_t"),
            (typeof(ClientTransportStats), "rocketnet_client_transport_stats_t"),
            (typeof(ServerTransportStats), "rocketnet_server_transport_stats_t"),
            (typeof(ServerTransportClientStats), "rocketnet_server_transport_client_stats_t"),
            (typeof(FrameSyncStats), "rocketnet_frame_sync_stats_t")
        };
        foreach ( var type in types )
        {
            nuint expected = (nuint)Marshal.SizeOf(type.Managed);
            nuint actual = typeSize(type.Native);
            if ( actual != expected )
                throw new BadImageFormatException( $"RocketNet ABI: {type.Native} size {actual}, binding requires {expected}." );
            var fields = type.Managed.GetFields(BindingFlags.Public | BindingFlags.Instance);
            if ( fieldCount(type.Native) != fields.Length )
                throw new BadImageFormatException( $"RocketNet ABI: {type.Native} field count differs." );
            foreach ( var field in fields )
            {
                string nativeName = NativeFieldName(field.Name);
                expected = (nuint)Marshal.OffsetOf(type.Managed, field.Name);
                actual = fieldOffset(type.Native, nativeName);
                if ( actual != expected )
                    throw new BadImageFormatException( $"RocketNet ABI: {type.Native}.{nativeName} offset {actual}, binding requires {expected}." );
                var buffer = field.GetCustomAttribute<FixedBufferAttribute>();
                expected = (nuint)(buffer != null ? buffer.Length * Marshal.SizeOf(buffer.ElementType) : Marshal.SizeOf(field.FieldType));
                actual = fieldSize(type.Native, nativeName);
                if ( actual != expected )
                    throw new BadImageFormatException( $"RocketNet ABI: {type.Native}.{nativeName} width {actual}, binding requires {expected}." );
            }
        }
    }
}
