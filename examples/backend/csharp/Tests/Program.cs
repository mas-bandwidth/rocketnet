/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Unit tests: no sockets, no real time. The known answers are the C
    library's own, tests/connect_token_vectors.json, minted by
    rocketnet_write_connect_token_deterministic; this backend must write
    every one of them byte for byte.

    Run from examples/backend/csharp: dotnet run --project Tests. Prints each
    test's name; exits 1 on the first failure.
*/

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using RocketNet.Backend;
using Sodium;

static class Tests
{
    sealed class TestFailure : Exception
    {
        public TestFailure( string message ) : base( message ) {}
    }

    static void Check( bool condition, string what )
    {
        if ( !condition )
            throw new TestFailure( what );
    }

    static byte[] Unhex( JsonElement vector, string field, int bytes )
    {
        byte[] data = Convert.FromHexString( vector.GetProperty( field ).GetString() ?? "" );
        Check( data.Length == bytes, $"{field} is not {bytes} bytes" );
        return data;
    }

    // the vectors' integers are fixed-width big-endian hex
    static ulong UnhexUInt64( JsonElement vector, string field ) => BinaryPrimitives.ReadUInt64BigEndian( Unhex( vector, field, 8 ) );

    static string VectorsPath()
    {
        // walk up from the working directory to the repository's tests/connect_token_vectors.json
        for ( DirectoryInfo? directory = new DirectoryInfo( Directory.GetCurrentDirectory() ); directory != null; directory = directory.Parent )
        {
            string path = Path.Combine( directory.FullName, "tests", "connect_token_vectors.json" );
            if ( File.Exists( path ) )
                return path;
        }
        throw new TestFailure( "tests/connect_token_vectors.json not found above the working directory" );
    }

    static List<JsonElement> LoadVectors()
    {
        using JsonDocument document = JsonDocument.Parse( File.ReadAllText( VectorsPath() ) );
        JsonElement root = document.RootElement;
        Check( root.GetProperty( "version" ).GetString() == ConnectToken.WireVersion.ToString( "x8", null ), "vectors file version" );
        List<JsonElement> vectors = root.GetProperty( "vectors" ).EnumerateArray().Select( v => v.Clone() ).ToList();
        Check( vectors.Count >= 4, "at least four vectors" );
        return vectors;
    }

    // crypto_box_seal with the ephemeral secret supplied, as the test-only C
    // writer does it: epk = X25519(esk, basepoint), nonce = BLAKE2b-192(epk ||
    // server_pk), sealed = epk || crypto_box_easy(block, nonce, server_pk, esk)

    static Func<byte[], byte[], byte[]> SealWithEphemeral( byte[] ephemeralSecret )
    {
        return ( block, serverPublicKey ) =>
        {
            byte[] epk = ScalarMult.Base( ephemeralSecret );
            byte[] nonce = GenericHash.Hash( epk.Concat( serverPublicKey ).ToArray(), (byte[]?) null, 24 );
            byte[] box = PublicKeyBox.Create( block, nonce, ephemeralSecret, serverPublicKey );
            return epk.Concat( box ).ToArray();
        };
    }

    static void test_connect_token_vectors()
    {
        foreach ( JsonElement v in LoadVectors() )
        {
            string name = v.GetProperty( "name" ).GetString() ?? "";
            SignedConnectData signedFields = new SignedConnectData
            {
                ProtocolId = UnhexUInt64( v, "protocol_id" ),
                CreateTimestamp = UnhexUInt64( v, "create_timestamp" ),
                ExpireTimestamp = UnhexUInt64( v, "expire_timestamp" ),
                TimeoutSeconds = BinaryPrimitives.ReadInt32BigEndian( Unhex( v, "timeout_seconds", 4 ) ),
                AddressFlags = Unhex( v, "address_flags", 1 )[0],
                Ipv4Address = Unhex( v, "ipv4_address", 6 ),
                Ipv6Address = Unhex( v, "ipv6_address", 18 ),
                ServerPublicKey = Unhex( v, "server_public_key", 32 ),
            };
            ConnectTokenPrivate privateFields = new ConnectTokenPrivate
            {
                ClientId = UnhexUInt64( v, "client_id" ),
                ClientToServerKey = Unhex( v, "client_to_server_key", 32 ),
                ServerToClientKey = Unhex( v, "server_to_client_key", 32 ),
                UserData = Unhex( v, "user_data", 256 ),
            };
            byte[] backendPrivateKey = PublicKeyAuth.GenerateKeyPair( Unhex( v, "backend_signing_seed", 32 ) ).PrivateKey;

            byte[] token = ConnectToken.Write( signedFields, privateFields, backendPrivateKey, SealWithEphemeral( Unhex( v, "ephemeral_secret", 32 ) ) );
            Check( token.AsSpan().SequenceEqual( Unhex( v, "token", ConnectToken.ConnectTokenBytes ) ), $"{name}: token differs from the vector: {Convert.ToHexString( token ).ToLowerInvariant()}" );
        }
    }

    // the address strings that encode to each vector's address fields

    static void test_server_addresses_match_vectors()
    {
        Dictionary<string, string[]> addresses = new Dictionary<string, string[]>
        {
            ["ipv4_appendix_inputs"] = new[] { "127.0.0.1:40000" },
            ["ipv6_loopback_counting_user_data"] = new[] { "[::1]:50000" },
            ["dual_stack_negative_timeout"] = new[] { "10.0.0.1:1", "[2001:db8::1]:65535" },
            ["extreme_values_text_user_data"] = new[] { "255.255.255.255:65535" },
        };
        foreach ( JsonElement v in LoadVectors() )
        {
            string name = v.GetProperty( "name" ).GetString() ?? "";
            Check( addresses.ContainsKey( name ), $"no address strings for vector {name}" );
            SignedConnectData signedFields = new SignedConnectData();
            ConnectToken.WriteServerAddresses( signedFields, addresses[name] );
            Check( signedFields.AddressFlags == Unhex( v, "address_flags", 1 )[0] &&
                   signedFields.Ipv4Address.AsSpan().SequenceEqual( Unhex( v, "ipv4_address", 6 ) ) &&
                   signedFields.Ipv6Address.AsSpan().SequenceEqual( Unhex( v, "ipv6_address", 18 ) ), $"{name}: addresses encode differently" );
        }
    }

    static void test_server_addresses_refused()
    {
        string[][] lists =
        {
            Array.Empty<string>(),
            new[] { "127.0.0.1:1", "[::1]:1", "10.0.0.1:1" },
            new[] { "127.0.0.1:1", "10.0.0.1:1" },
            new[] { "[::1]:1", "[::2]:1" },
            new[] { "127.0.0.1" },
            new[] { "127.0.0.1:0" },
            new[] { "localhost:40000" },
            new[] { "[fe80::1%en0]:40000" },
            new[] { "1:40000" },
            new[] { "::1:40000" },
        };
        foreach ( string[] list in lists )
        {
            bool refused = false;
            try { ConnectToken.WriteServerAddresses( new SignedConnectData(), list ); }
            catch ( ArgumentException ) { refused = true; }
            Check( refused, $"[{string.Join( ',', list )}] was accepted" );
        }
    }

    // the published development server keypair (sdk/rocketnet_connect_token.cpp),
    // standing in for a server's own

    static readonly byte[] DevServerPublicKey = Convert.FromHexString( "3d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801c" );
    static readonly byte[] DevServerSecretKey = Convert.FromHexString( "6ffb8f7337ec377697f568de3118d83959560b25928d22707ba2297589d08b97" );

    static Func<string, string?> Environment( Dictionary<string, string> values ) => name => values.TryGetValue( name, out string? value ) ? value : null;

    // a production mint (fresh ephemeral seal) verifies against the development
    // backend public key and opens with the server's secret key, in the order a
    // server checks it

    static void test_generated_token_opens()
    {
        BackendConfig config = BackendConfig.Read( Environment( new Dictionary<string, string>
        {
            ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = Convert.ToBase64String( DevServerPublicKey ),
            ["ROCKETNET_BACKEND_SERVER_ADDRESSES"] = "127.0.0.1:40000,[::1]:40001",
        } ) );
        Check( config.DevelopmentKey, "no ROCKETNET_BACKEND_PRIVATE_KEY selects the development key" );
        byte[] keys = Enumerable.Repeat( (byte) 0x5a, 64 ).ToArray();
        int offset = 0;
        byte[] token = ConnectToken.Generate( config, 0x0123456789abcdef, 1000, buffer => { Array.Copy( keys, offset, buffer, 0, buffer.Length ); offset += buffer.Length; }, ConnectToken.SealAnonymous );
        Check( token.Length == ConnectToken.ConnectTokenBytes && Convert.ToBase64String( token ).Length == 1024, "token size" );

        byte[] devBackendPublicKey = BackendConfig.DevBackendPrivateKey.AsSpan( 32, 32 ).ToArray();
        byte[] signedData = token.AsSpan( 0, ConnectToken.SignedConnectDataBytes ).ToArray();
        byte[] signature = token.AsSpan( ConnectToken.SignedConnectDataBytes, ConnectToken.SignatureBytes ).ToArray();
        Check( PublicKeyAuth.VerifyDetached( signature, signedData, devBackendPublicKey ), "signature verifies" );
        Check( BinaryPrimitives.ReadUInt32LittleEndian( token.AsSpan( 0 ) ) == ConnectToken.WireVersion &&
               BinaryPrimitives.ReadUInt64LittleEndian( token.AsSpan( 4 ) ) == BackendConfig.ExampleProtocolId &&
               BinaryPrimitives.ReadUInt64LittleEndian( token.AsSpan( 12 ) ) == 1000 &&
               BinaryPrimitives.ReadUInt64LittleEndian( token.AsSpan( 20 ) ) == 1030 &&
               BinaryPrimitives.ReadInt32LittleEndian( token.AsSpan( 28 ) ) == 10 &&
               token[32] == ( ConnectToken.AddressFlagIpv4 | ConnectToken.AddressFlagIpv6 ), "signed fields" );

        byte[] block = SealedPublicKeyBox.Open( token.AsSpan( ConnectToken.OffsetSealedBlock, ConnectToken.SealedBlockBytes ).ToArray(), DevServerSecretKey, DevServerPublicKey );
        Check( block.Length == ConnectToken.PrivateBlockBytes, "sealed block opens with the server key" );
        Check( BinaryPrimitives.ReadUInt64LittleEndian( block.AsSpan( 0 ) ) == 0x0123456789abcdef &&
               BinaryPrimitives.ReadUInt64LittleEndian( block.AsSpan( 8 ) ) == BackendConfig.ExampleProtocolId &&
               BinaryPrimitives.ReadUInt64LittleEndian( block.AsSpan( 16 ) ) == 1030 &&
               block.AsSpan( 24, 64 ).SequenceEqual( keys ) &&
               token.AsSpan( ConnectToken.SignedConnectDataBytes + ConnectToken.SignatureBytes, 64 ).SequenceEqual( keys ), "private block" );
        Check( block.AsSpan( 88 ).IndexOfAnyExcept( (byte) 0 ) < 0 && token.AsSpan( 704 ).IndexOfAnyExcept( (byte) 0 ) < 0, "padding is zero" );
    }

    static void test_config_refused()
    {
        string serverKey = Convert.ToBase64String( DevServerPublicKey );
        byte[] badPrivateKey = BackendConfig.DevBackendPrivateKey.ToArray();
        badPrivateKey[63] ^= 1;
        Dictionary<string, string>[] cases =
        {
            new(),
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = "AAAA" },
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = serverKey, ["ROCKETNET_BACKEND_PRIVATE_KEY"] = Convert.ToBase64String( BackendConfig.DevBackendPrivateKey.AsSpan( 0, 32 ) ) },
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = serverKey, ["ROCKETNET_BACKEND_PRIVATE_KEY"] = Convert.ToBase64String( badPrivateKey ) },
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = serverKey, ["ROCKETNET_BACKEND_PROTOCOL_ID"] = "0" },
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = serverKey, ["ROCKETNET_BACKEND_SERVER_ADDRESSES"] = "127.0.0.1:1,10.0.0.1:1" },
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = serverKey, ["ROCKETNET_BACKEND_PORT"] = "65536" },
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = serverKey, ["ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS"] = "0" },
            new() { ["ROCKETNET_BACKEND_SERVER_PUBLIC_KEY"] = serverKey, ["ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS"] = "-1" },
        };
        foreach ( Dictionary<string, string> values in cases )
        {
            bool refused = false;
            try { BackendConfig.Read( Environment( values ) ); }
            catch ( ArgumentException ) { refused = true; }
            Check( refused, $"{{{string.Join( ", ", values.Select( pair => pair.Key + "=" + pair.Value ) )}}} was accepted" );
        }
    }

    static void test_keygen_matches_signing_vector()
    {
        JsonElement vector = LoadVectors()[0];
        byte[] seed = Unhex( vector, "backend_signing_seed", 32 );
        var expected = PublicKeyAuth.GenerateKeyPair( seed );
        using StringWriter output = new StringWriter();
        BackendKeys.Write( output, buffer => seed.CopyTo( buffer, 0 ) );
        string[] lines = output.ToString().Split( '\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries );
        Check( lines.Length == 2, "keygen emits two environment assignments" );
        Check( lines[0] == "ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=" + Convert.ToBase64String( expected.PublicKey ), "keygen public key" );
        Check( lines[1] == "ROCKETNET_BACKEND_PRIVATE_KEY=" + Convert.ToBase64String( expected.PrivateKey ), "keygen expanded 64-byte private key" );
    }

    static void test_server_registry()
    {
        ServerRegistry registry = new ServerRegistry( count => count - 1 );
        Check( registry.Random() == null, "empty registry" );
        byte[] keyA = Enumerable.Repeat( (byte) 0x11, ConnectToken.KeyBytes ).ToArray();
        byte[] keyB = Enumerable.Repeat( (byte) 0x22, ConnectToken.KeyBytes ).ToArray();
        string[] addressA = { "127.0.0.1:40000" };
        string[] addressB = { "127.0.0.1:40001" };
        string[] addressC = { "[::1]:40002" };
        registry.Register( "one", addressA, Convert.ToBase64String( keyA ) );
        registry.Register( "one", addressB, Convert.ToBase64String( keyB ) );
        RegisteredServer? server = registry.Random();
        Check( server != null && server.Addresses.SequenceEqual( addressB ) && server.PublicKey.SequenceEqual( keyB ), "re-registration replaces" );
        registry.Register( "two", addressC, Convert.ToBase64String( keyA ) );
        server = registry.Random();
        Check( server != null &&
               ( server.Addresses.SequenceEqual( addressB ) && server.PublicKey.SequenceEqual( keyB ) ||
                 server.Addresses.SequenceEqual( addressC ) && server.PublicKey.SequenceEqual( keyA ) ),
               "random selection stays in the registered set" );
        bool refused = false;
        string[] invalidAddress = { "127.0.0.1:0" };
        try { registry.Register( "bad", invalidAddress, Convert.ToBase64String( keyA ) ); }
        catch ( ArgumentException ) { refused = true; }
        Check( refused, "invalid address refused" );
        refused = false;
        try { registry.Register( "bad", addressA, "AAAA" ); }
        catch ( ArgumentException ) { refused = true; }
        Check( refused, "invalid public key refused" );

        BackendConfig config = BackendConfig.Read( Environment( new Dictionary<string, string>() ), requireServer: false );
        Check( !config.ServerConfigured && config.ServerAddresses.Length == 0, "service starts with an empty registry" );
    }

    static int Main()
    {
        (string, Action)[] tests =
        {
            ( nameof( test_keygen_matches_signing_vector ), test_keygen_matches_signing_vector ),
            ( nameof( test_server_registry ), test_server_registry ),
            ( nameof( test_connect_token_vectors ), test_connect_token_vectors ),
            ( nameof( test_server_addresses_match_vectors ), test_server_addresses_match_vectors ),
            ( nameof( test_server_addresses_refused ), test_server_addresses_refused ),
            ( nameof( test_generated_token_opens ), test_generated_token_opens ),
            ( nameof( test_config_refused ), test_config_refused ),
        };
        foreach ( ( string name, Action test ) in tests )
        {
            Console.WriteLine( name );
            try
            {
                test();
            }
            catch ( TestFailure e )
            {
                Console.WriteLine( $"FAIL: {e.Message}" );
                return 1;
            }
        }
        return 0;
    }
}
