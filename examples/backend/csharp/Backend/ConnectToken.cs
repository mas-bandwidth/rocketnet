/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The connect token a backend mints: the 768 byte token of STANDARD.md,
    byte for byte the one sdk/rocketnet_connect_token.cpp writes. The backend
    signs the signed connect data with its Ed25519 key, and the private block
    inside it is sealed (crypto_box_seal) to the server's X25519 public key,
    so only that server can read the session keys.

    This is a backend for C game clients and servers: RocketNet has no C#
    client API, and this file is not one.
*/

using System;
using System.Buffers.Binary;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using Sodium;

namespace RocketNet.Backend
{
    // The fields the backend signs: everything a server checks before it
    // opens the sealed private block.

    internal sealed class SignedConnectData
    {
        public ulong ProtocolId;
        public ulong CreateTimestamp;
        public ulong ExpireTimestamp;
        public int TimeoutSeconds;
        public byte AddressFlags;
        public byte[] Ipv4Address = new byte[6];                        // a,b,c,d octets then the port little-endian
        public byte[] Ipv6Address = new byte[18];                       // 8 uint16 groups little-endian then the port little-endian
        public byte[] ServerPublicKey = new byte[ConnectToken.KeyBytes];
    }

    // The fields sealed to the server. The private block's protocol id and
    // expire are always the signed ones.

    internal sealed class ConnectTokenPrivate
    {
        public ulong ClientId;
        public byte[] ClientToServerKey = new byte[ConnectToken.KeyBytes];
        public byte[] ServerToClientKey = new byte[ConnectToken.KeyBytes];
        public byte[] UserData = new byte[ConnectToken.UserDataBytes];
    }

    internal static class ConnectToken
    {
        // the wire format (sdk/rocketnet_connect_token.h)

        public const uint WireVersion = 0x4E7C1A93;
        public const int ConnectTokenBytes = 768;
        public const int SignedConnectDataBytes = 576;
        public const int PrivateBlockBytes = 384;
        public const int SealedBlockBytes = 432;
        public const int SignatureBytes = 64;
        public const int KeyBytes = 32;
        public const int UserDataBytes = 256;
        public const byte AddressFlagIpv4 = 1 << 0;
        public const byte AddressFlagIpv6 = 1 << 1;
        public const int OffsetSealedBlock = 89;

        // seals the private block to the server public key: crypto_box_seal in
        // production; the known-answer tests pass a seal with a fixed
        // ephemeral key

        public static byte[] SealAnonymous( byte[] block, byte[] serverPublicKey )
        {
            return SealedPublicKeyBox.Create( block, serverPublicKey );
        }

        public static byte[] WritePrivate( SignedConnectData signedFields, ConnectTokenPrivate privateFields )
        {
            ArgumentNullException.ThrowIfNull( signedFields );
            ArgumentNullException.ThrowIfNull( privateFields );
            byte[] block = new byte[PrivateBlockBytes];
            BinaryPrimitives.WriteUInt64LittleEndian( block.AsSpan( 0 ), privateFields.ClientId );
            BinaryPrimitives.WriteUInt64LittleEndian( block.AsSpan( 8 ), signedFields.ProtocolId );
            BinaryPrimitives.WriteUInt64LittleEndian( block.AsSpan( 16 ), signedFields.ExpireTimestamp );
            privateFields.ClientToServerKey.CopyTo( block, 24 );
            privateFields.ServerToClientKey.CopyTo( block, 24 + KeyBytes );
            privateFields.UserData.CopyTo( block, 24 + KeyBytes * 2 );
            return block;
        }

        public static byte[] Write( SignedConnectData signedFields, ConnectTokenPrivate privateFields, byte[] backendPrivateKey, Func<byte[], byte[], byte[]> seal )
        {
            ArgumentNullException.ThrowIfNull( signedFields );
            ArgumentNullException.ThrowIfNull( privateFields );
            ArgumentNullException.ThrowIfNull( seal );
            byte[] block = WritePrivate( signedFields, privateFields );
            byte[] sealedBlock = seal( block, signedFields.ServerPublicKey );
            CryptographicOperations.ZeroMemory( block );
            if ( sealedBlock.Length != SealedBlockBytes )
                throw new CryptographicException( $"sealed block is {sealedBlock.Length} bytes, not {SealedBlockBytes}" );

            byte[] token = new byte[ConnectTokenBytes];
            BinaryPrimitives.WriteUInt32LittleEndian( token.AsSpan( 0 ), WireVersion );
            BinaryPrimitives.WriteUInt64LittleEndian( token.AsSpan( 4 ), signedFields.ProtocolId );
            BinaryPrimitives.WriteUInt64LittleEndian( token.AsSpan( 12 ), signedFields.CreateTimestamp );
            BinaryPrimitives.WriteUInt64LittleEndian( token.AsSpan( 20 ), signedFields.ExpireTimestamp );
            BinaryPrimitives.WriteInt32LittleEndian( token.AsSpan( 28 ), signedFields.TimeoutSeconds );
            token[32] = signedFields.AddressFlags;
            signedFields.Ipv4Address.CopyTo( token, 33 );
            signedFields.Ipv6Address.CopyTo( token, 39 );
            signedFields.ServerPublicKey.CopyTo( token, 57 );
            sealedBlock.CopyTo( token, OffsetSealedBlock );

            byte[] signature = PublicKeyAuth.SignDetached( token.AsSpan( 0, SignedConnectDataBytes ).ToArray(), backendPrivateKey );
            signature.CopyTo( token, SignedConnectDataBytes );
            privateFields.ClientToServerKey.CopyTo( token, SignedConnectDataBytes + SignatureBytes );
            privateFields.ServerToClientKey.CopyTo( token, SignedConnectDataBytes + SignatureBytes + KeyBytes );
            return token;
        }

        // one server's addresses: at most one IPv4 and one IPv6, as
        // "a.b.c.d:port" and "[ipv6]:port"

        public static void WriteServerAddresses( SignedConnectData signedFields, string[] addresses )
        {
            ArgumentNullException.ThrowIfNull( signedFields );
            ArgumentNullException.ThrowIfNull( addresses );
            if ( addresses.Length < 1 || addresses.Length > 2 )
                throw new ArgumentException( $"a token names one server by one or two addresses, not {addresses.Length}" );
            foreach ( string text in addresses )
            {
                IPEndPoint address = ParseAddress( text.Trim() ) ?? throw new ArgumentException( $"invalid server address \"{text}\"" );
                ushort port = (ushort) address.Port;
                if ( address.AddressFamily == AddressFamily.InterNetwork )
                {
                    if ( ( signedFields.AddressFlags & AddressFlagIpv4 ) != 0 )
                        throw new ArgumentException( "more than one IPv4 server address" );
                    signedFields.AddressFlags |= AddressFlagIpv4;
                    address.Address.GetAddressBytes().CopyTo( signedFields.Ipv4Address, 0 );
                    BinaryPrimitives.WriteUInt16LittleEndian( signedFields.Ipv4Address.AsSpan( 4 ), port );
                }
                else
                {
                    if ( ( signedFields.AddressFlags & AddressFlagIpv6 ) != 0 )
                        throw new ArgumentException( "more than one IPv6 server address" );
                    signedFields.AddressFlags |= AddressFlagIpv6;
                    byte[] octets = address.Address.GetAddressBytes();
                    for ( int i = 0; i < 8; i++ )
                        BinaryPrimitives.WriteUInt16LittleEndian( signedFields.Ipv6Address.AsSpan( i * 2 ), BinaryPrimitives.ReadUInt16BigEndian( octets.AsSpan( i * 2 ) ) );
                    BinaryPrimitives.WriteUInt16LittleEndian( signedFields.Ipv6Address.AsSpan( 16 ), port );
                }
            }
        }

        // "a.b.c.d:port" or "[ipv6]:port", port 1 to 65535, no scope; null otherwise

        static IPEndPoint? ParseAddress( string text )
        {
            string host;
            string port;
            if ( text.StartsWith( '[' ) )
            {
                int close = text.IndexOf( "]:", StringComparison.Ordinal );
                if ( close < 0 ) return null;
                host = text.Substring( 1, close - 1 );
                port = text.Substring( close + 2 );
            }
            else
            {
                int colon = text.LastIndexOf( ':' );
                if ( colon < 0 ) return null;
                host = text.Substring( 0, colon );
                port = text.Substring( colon + 1 );
            }
            if ( !ushort.TryParse( port, NumberStyles.None, CultureInfo.InvariantCulture, out ushort portNumber ) || portNumber == 0 )
                return null;
            if ( !IPAddress.TryParse( host, out IPAddress? address ) )
                return null;
            bool bracketed = text.StartsWith( '[' );
            if ( address.AddressFamily == AddressFamily.InterNetwork )
            {
                // IPAddress.TryParse takes "1" as 0.0.0.1; take only a dotted quad
                if ( bracketed || address.ToString() != host ) return null;
            }
            else if ( address.AddressFamily != AddressFamily.InterNetworkV6 || !bracketed || address.ScopeId != 0 || host.Contains( '%', StringComparison.Ordinal ) )
            {
                return null;
            }
            return new IPEndPoint( address, portNumber );
        }

        // mints a token for clientId with fresh session keys, created at
        // createTimestamp (unix seconds)

        public static byte[] Generate( BackendConfig config, ulong clientId, ulong createTimestamp, Action<byte[]> random, Func<byte[], byte[], byte[]> seal )
        {
            ArgumentNullException.ThrowIfNull( config );
            ArgumentNullException.ThrowIfNull( random );
            SignedConnectData signedFields = new SignedConnectData
            {
                ProtocolId = config.ProtocolId,
                CreateTimestamp = createTimestamp,
                ExpireTimestamp = createTimestamp + config.ExpireSeconds,
                TimeoutSeconds = config.TimeoutSeconds,
            };
            config.ServerPublicKey.CopyTo( signedFields.ServerPublicKey, 0 );
            WriteServerAddresses( signedFields, config.ServerAddresses );
            ConnectTokenPrivate privateFields = new ConnectTokenPrivate { ClientId = clientId };
            random( privateFields.ClientToServerKey );
            random( privateFields.ServerToClientKey );
            try
            {
                return Write( signedFields, privateFields, config.BackendPrivateKey, seal );
            }
            finally
            {
                CryptographicOperations.ZeroMemory( privateFields.ClientToServerKey );
                CryptographicOperations.ZeroMemory( privateFields.ServerToClientKey );
            }
        }
    }

    internal sealed class BackendConfig
    {
        // The published development backend key, from the standard.
        // DEVELOPMENT ONLY: its secret half is public, so a server that
        // accepts it accepts tokens from anyone. Its public half is
        // ROCKETNET_DEV_BACKEND_PUBLIC_KEY.

        public static readonly byte[] DevBackendPrivateKey =
        {
            0x75, 0x29, 0x02, 0x15, 0xe1, 0xff, 0x4d, 0xf2, 0x09, 0x92, 0x57, 0x8a,
            0x22, 0x3c, 0x7c, 0xd4, 0x63, 0xb9, 0xa2, 0x7c, 0xf8, 0x3a, 0xa7, 0x1c,
            0x7a, 0x16, 0x93, 0x1c, 0x01, 0x9b, 0x7c, 0x30, 0xb0, 0xab, 0x03, 0xb3,
            0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99, 0x29, 0xa5, 0xb4, 0xbf,
            0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d, 0x08, 0x4a, 0x08, 0xd8,
            0x58, 0xbd, 0x24, 0x2c,
        };

        // the examples' protocol id (examples/dev_keys.h)
        public const ulong ExampleProtocolId = 0x52434b544e455431;

        public int Port = 8080;
        public byte[] BackendPrivateKey = DevBackendPrivateKey;
        public ulong ProtocolId = ExampleProtocolId;
        public string[] ServerAddresses = { "127.0.0.1:40000" };
        public byte[] ServerPublicKey = new byte[ConnectToken.KeyBytes];
        public ulong ExpireSeconds = 30;
        public int TimeoutSeconds = 10;
        public bool DevelopmentKey = true;

        static byte[]? Base64( string text, int bytes )
        {
            byte[] buffer = new byte[bytes + 3];
            return Convert.TryFromBase64String( text, buffer, out int written ) && written == bytes ? buffer.AsSpan( 0, bytes ).ToArray() : null;
        }

        // reads the config from the environment through getenv, so tests pass
        // a dictionary; throws ArgumentException naming the variable

        public static BackendConfig Read( Func<string, string?> getenv )
        {
            ArgumentNullException.ThrowIfNull( getenv );
            BackendConfig config = new BackendConfig();

            string? text = getenv( "ROCKETNET_BACKEND_PORT" );
            if ( !string.IsNullOrEmpty( text ) )
            {
                if ( !ushort.TryParse( text, NumberStyles.None, CultureInfo.InvariantCulture, out ushort port ) || port == 0 )
                    throw new ArgumentException( $"ROCKETNET_BACKEND_PORT is not 1 to 65535: \"{text}\"" );
                config.Port = port;
            }

            text = getenv( "ROCKETNET_BACKEND_PRIVATE_KEY" );
            if ( !string.IsNullOrEmpty( text ) )
            {
                config.BackendPrivateKey = Base64( text, 64 ) ?? throw new ArgumentException( "ROCKETNET_BACKEND_PRIVATE_KEY is not the base64 of a 64 byte Ed25519 secret key (keygen prints one)" );
                config.DevelopmentKey = false;
            }
            // a 64 byte Ed25519 secret key is its seed then its public key; refuse one whose halves disagree
            byte[] expanded = PublicKeyAuth.GenerateKeyPair( config.BackendPrivateKey.AsSpan( 0, 32 ).ToArray() ).PrivateKey;
            if ( !CryptographicOperations.FixedTimeEquals( expanded, config.BackendPrivateKey ) )
                throw new ArgumentException( "ROCKETNET_BACKEND_PRIVATE_KEY: the public half does not match the seed" );
            CryptographicOperations.ZeroMemory( expanded );

            text = getenv( "ROCKETNET_BACKEND_PROTOCOL_ID" );
            if ( !string.IsNullOrEmpty( text ) )
            {
                bool hex = text.StartsWith( "0x", StringComparison.OrdinalIgnoreCase );
                if ( !ulong.TryParse( hex ? text.Substring( 2 ) : text, hex ? NumberStyles.AllowHexSpecifier : NumberStyles.None, CultureInfo.InvariantCulture, out ulong protocolId ) || protocolId == 0 )
                    throw new ArgumentException( $"ROCKETNET_BACKEND_PROTOCOL_ID is not a nonzero 64 bit id: \"{text}\"" );
                config.ProtocolId = protocolId;
            }

            text = getenv( "ROCKETNET_BACKEND_SERVER_ADDRESSES" );
            if ( !string.IsNullOrEmpty( text ) )
                config.ServerAddresses = text.Split( ',' );
            try
            {
                ConnectToken.WriteServerAddresses( new SignedConnectData(), config.ServerAddresses );
            }
            catch ( ArgumentException e )
            {
                throw new ArgumentException( $"ROCKETNET_BACKEND_SERVER_ADDRESSES: {e.Message}", e );
            }

            // each server makes its own keypair at create; the backend needs its public half
            text = getenv( "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY" );
            config.ServerPublicKey = ( string.IsNullOrEmpty( text ) ? null : Base64( text, ConnectToken.KeyBytes ) )
                ?? throw new ArgumentException( "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY is not the base64 of the server's 32 byte public key (example_backend_server prints it)" );

            text = getenv( "ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS" );
            if ( !string.IsNullOrEmpty( text ) )
            {
                if ( !uint.TryParse( text, NumberStyles.None, CultureInfo.InvariantCulture, out uint seconds ) || seconds == 0 )
                    throw new ArgumentException( $"ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS is not a positive number of seconds: \"{text}\"" );
                config.ExpireSeconds = seconds;
            }
            text = getenv( "ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS" );
            if ( !string.IsNullOrEmpty( text ) )
            {
                if ( !int.TryParse( text, NumberStyles.None, CultureInfo.InvariantCulture, out int seconds ) || seconds <= 0 )
                    throw new ArgumentException( $"ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS is not a positive number of seconds: \"{text}\"" );
                config.TimeoutSeconds = seconds;
            }

            return config;
        }
    }
}
