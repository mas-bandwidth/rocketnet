/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0 or later, or the Más Bandwidth Source License 1.0 (MBSL-1.0). See LICENSE and MBSL.md.

    Unit tests for Users.cs: tickets, user hashes, bans and whitelists, and
    the routes as plain functions with a fixed clock. No sockets, no real
    time. The cross-backend vectors are the Go backend's too (users_test.go):
    both backends hash and sign the same way.
*/

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using RocketNet.Backend;
using Sodium;

static class UsersTests
{
    const ulong Now = 1_800_000_000;

    public sealed class TestFailure : Exception
    {
        public TestFailure( string message ) : base( message ) {}
    }

    static void Check( bool condition, string what )
    {
        if ( !condition )
        {
            throw new TestFailure( what );
        }
    }

    static readonly string[] ServerOne = { "127.0.0.1:40000" };
    static readonly string[] ServerTwo = { "127.0.0.1:40001" };

    static byte[] Fill( byte value ) => Enumerable.Repeat( value, MockProvider.SecretKeyBytes ).ToArray();

    static MockProvider TestMock() => new MockProvider( Fill( 0x31 ) );

    static Users TestUsers() => new Users( Fill( 0x47 ) );

    static bool Refused( MockProvider mock, string ticket, ulong now )
    {
        try
        {
            mock.Verify( ticket, now );
            return false;
        }
        catch ( TicketException )
        {
            return true;
        }
    }

    // the same values the Go backend's TestCrossBackendVectors checks
    public const string AliceTicketVector = "PNJJawAAAAAFYWxpY2UVSwku1PojgU3PTAD0HSHXU7W62CcrHPOs0kJVr/0h0A==";
    public const ulong AliceHashVector = 0x5098a1e5575a941f;

    // alice's ticket with each of these put inside it is refused by both
    public static readonly string[] WhitespaceVectors = { " ", "\n", "\r\n", "\t" };

    public static void test_mock_ticket()
    {
        MockProvider mock = TestMock();
        string ticket = mock.Issue( "alice", Now, MockProvider.TicketSeconds );
        Check( mock.Verify( ticket, Now ) == "alice", "a fresh ticket verifies" );
        Check( mock.Verify( ticket, Now + MockProvider.TicketSeconds - 1 ) == "alice", "a ticket verifies in its last second" );
        Check( Refused( mock, ticket, Now + MockProvider.TicketSeconds ), "an expired ticket is refused" );

        byte[] raw = Convert.FromBase64String( ticket );
        for ( int i = 0; i < raw.Length; i++ )
        {
            byte[] edited = raw.ToArray();
            edited[i] ^= 0x01;
            Check( Refused( mock, Convert.ToBase64String( edited ), Now ), $"a ticket with byte {i} edited is refused" );
        }
        byte[] bob = raw.ToArray();
        "bobby"u8.CopyTo( bob.AsSpan( 9 ) );
        Check( Refused( mock, Convert.ToBase64String( bob ), Now ), "a ticket with its user id replaced is refused" );

        MockProvider forger = new MockProvider( Fill( 0x32 ) );
        Check( Refused( mock, forger.Issue( "alice", Now, 60 ), Now ), "a ticket signed with another key is refused" );

        foreach ( string text in new[] { "", "@@@@", Convert.ToBase64String( raw.AsSpan( 0, raw.Length - 1 ) ), new string( 'A', MockProvider.MaxTicketTextBytes + 4 ) } )
        {
            Check( Refused( mock, text, Now ), $"ticket \"{text}\" is refused" );
        }
        bool threw = false;
        try
        {
            mock.Issue( new string( 'x', MockProvider.MaxUserIdBytes + 1 ), Now, 60 );
        }
        catch ( ArgumentException )
        {
            threw = true;
        }
        Check( threw, "a long user id gets no ticket" );
    }

    public static void test_user_hash()
    {
        Users users = TestUsers();
        ulong alice = users.Hash( "mock", "alice" );
        Check( users.Hash( "mock", "alice" ) == alice, "the same user hashes one way" );
        Check( users.Hash( "other", "alice" ) != alice, "the same user id on another provider has another hash" );
        Check( users.Hash( "mock", "bob" ) != alice, "another user has another hash" );
        Check( new Users( new byte[32] ).Hash( "mock", "alice" ) != alice, "another backend secret gives another hash" );
        foreach ( string provider in new[] { "", "mo:ck" } )
        {
            bool threw = false;
            try
            {
                users.Hash( provider, "alice" );
            }
            catch ( ArgumentException )
            {
                threw = true;
            }
            Check( threw, $"provider name \"{provider}\" is refused" );
        }
    }

    public static void test_cross_backend_vectors()
    {
        Check( TestMock().Issue( "alice", Now, MockProvider.TicketSeconds ) == AliceTicketVector, $"alice's ticket is {TestMock().Issue( "alice", Now, MockProvider.TicketSeconds )}" );
        Check( TestUsers().Hash( "mock", "alice" ) == AliceHashVector, $"alice's hash is {TestUsers().Hash( "mock", "alice" ):x16}" );
        Check( TestMock().Verify( AliceTicketVector, Now ) == "alice", "alice's ticket verifies" );
        foreach ( string inserted in WhitespaceVectors )
        {
            Check( Refused( TestMock(), AliceTicketVector.Insert( 8, inserted ), Now ), $"alice's ticket with {inserted.Length} whitespace character(s) inside is refused" );
        }
    }

    public static void test_whitelist()
    {
        Users users = TestUsers();
        ulong alice = users.Hash( "mock", "alice" ), bob = users.Hash( "mock", "bob" ), carol = users.Hash( "mock", "carol" );
        Check( users.Whitelist( "one" ).Length == 0, "a server nobody matched to has an empty whitelist" );
        Check( users.Seat( alice, "one" ) && users.Seat( bob, "one" ) && users.Seat( carol, "two" ), "unbanned users are seated" );
        Check( users.Whitelist( "one" ).SequenceEqual( new[] { alice, bob }.Order() ), "whitelist one" );
        Check( users.Whitelist( "two" ).SequenceEqual( new[] { carol } ), "whitelist two" );

        users.Seat( alice, "two" );
        Check( users.Whitelist( "one" ).SequenceEqual( new[] { bob } ) && users.Whitelist( "two" ).SequenceEqual( new[] { alice, carol }.Order() ), "a new match moves the user: one seat" );

        users.SetBanned( alice, true );
        Check( users.Whitelist( "two" ).SequenceEqual( new[] { carol } ), "a ban takes the user off the whitelist" );
        Check( !users.Seat( alice, "one" ) && !users.Whitelist( "one" ).Contains( alice ), "a banned user is not seated" );
        users.SetBanned( alice, false );
        Check( users.Seat( alice, "one" ) && users.Whitelist( "one" ).Contains( alice ), "a lifted ban lets the user back" );

        Check( Users.WriteWhitelist( Array.Empty<ulong>() ).Length == 0, "an empty whitelist is an empty body" );
        Check( Users.WriteWhitelist( new ulong[] { 1, 0x0102030405060708 } ).SequenceEqual( new byte[] { 1, 0, 0, 0, 0, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1 } ), "whitelist wire" );
    }

    static ulong[] ReadWhitelist( Reply reply )
    {
        Check( reply.Status == 200 && reply.Data != null && reply.Data.Length % 8 == 0, $"whitelist reply {reply.Status}" );
        ulong[] list = new ulong[reply.Data!.Length / 8];
        for ( int i = 0; i < list.Length; i++ )
        {
            list[i] = BinaryPrimitives.ReadUInt64LittleEndian( reply.Data.AsSpan( i * 8 ) );
        }
        return list;
    }

    // the client id sealed in a token, read the way the server reads it
    static ulong TokenClientId( Reply reply )
    {
        Check( reply.Status == 200, $"match answered {reply.Status} {reply.Text}" );
        byte[] token = Convert.FromBase64String( reply.Text );
        Check( token.Length == ConnectToken.ConnectTokenBytes, "match answered a token" );
        byte[] block = SealedPublicKeyBox.Open( token.AsSpan( ConnectToken.OffsetSealedBlock, ConnectToken.SealedBlockBytes ).ToArray(), Tests.DevServerSecretKey, Tests.DevServerPublicKey );
        return BinaryPrimitives.ReadUInt64LittleEndian( block );
    }

    public static void test_match_routes()
    {
        BackendConfig config = BackendConfig.Read( name => null, requireServer: false );
        // always the last server id in order, so a test picks the server
        ServerRegistry registry = new ServerRegistry( max => max - 1 );
        Users users = TestUsers();
        MockProvider mock = TestMock();
        UserRoutes routes = new UserRoutes( config, registry, users, mock, Users.VerifyWith( mock ), buffer => buffer.AsSpan().Fill( 0x5a ), ConnectToken.SealAnonymous );
        ulong alice = users.Hash( "mock", "alice" ), bob = users.Hash( "mock", "bob" );
        string serverKey = Convert.ToBase64String( Tests.DevServerPublicKey );

        Check( routes.Match( routes.MockTicket( "alice", Now ).Text, Now ).Status == 503, "match with no servers is 503" );
        Check( routes.Whitelist( "one" ).Status == 404, "the whitelist of an unregistered server is 404" );
        registry.Register( "one", ServerOne, serverKey );
        Check( ReadWhitelist( routes.Whitelist( "one" ) ).Length == 0, "a fresh server's whitelist is empty" );

        string ticket = routes.MockTicket( "alice", Now ).Text;
        Check( TokenClientId( routes.Match( ticket, Now ) ) == alice, "the token's client id is the user hash" );
        Check( ReadWhitelist( routes.Whitelist( "one" ) ).SequenceEqual( new[] { alice } ), "the matched user is on the server's whitelist" );
        Check( TokenClientId( routes.Match( routes.MockTicket( "alice", Now ).Text, Now ) ) == alice, "the same user gets the same client id" );

        byte[] raw = Convert.FromBase64String( ticket );
        raw[9] ^= 0x20;
        Check( routes.Match( Convert.ToBase64String( raw ), Now ).Status == 401, "an edited ticket is 401" );
        Check( routes.Match( new MockProvider( Fill( 0 ) ).Issue( "alice", Now, 60 ), Now ).Status == 401, "a forged ticket is 401" );
        Check( routes.Match( "", Now ).Status == 401, "an empty ticket is 401" );
        Check( routes.Match( ticket, Now + MockProvider.TicketSeconds ).Status == 401, "an expired ticket is 401" );

        registry.Register( "two", ServerTwo, serverKey );
        UserRoutes failing = new UserRoutes( config, registry, users, mock, Users.VerifyWith( mock ),
            buffer => throw new InvalidOperationException( "test mint failure" ), ConnectToken.SealAnonymous );
        try
        {
            failing.Match( ticket, Now );
            throw new TestFailure( "token generation should have failed" );
        }
        catch ( InvalidOperationException e ) when ( e.Message == "test mint failure" )
        {
            Check( ReadWhitelist( routes.Whitelist( "one" ) ).SequenceEqual( new[] { alice } ) &&
                   ReadWhitelist( routes.Whitelist( "two" ) ).Length == 0, "failed mint moved the existing seat" );
        }

        UserRoutes interrupted = new UserRoutes( config, registry, users, mock, Users.VerifyWith( mock ),
            buffer => buffer.AsSpan().Fill( 0x5a ), ( block, key ) => {
                users.SetBanned( alice, true );
                return ConnectToken.SealAnonymous( block, key );
            } );
        Check( interrupted.Match( ticket, Now ).Status == 403, "ban during mint refuses the token" );
        users.SetBanned( alice, false );
        Check( ReadWhitelist( routes.Whitelist( "one" ) ).SequenceEqual( new[] { alice } ) &&
               ReadWhitelist( routes.Whitelist( "two" ) ).Length == 0, "ban during mint moved the existing seat" );

        Check( TokenClientId( routes.Match( routes.MockTicket( "alice", Now ).Text, Now ) ) == alice, "match to server two" );
        Check( ReadWhitelist( routes.Whitelist( "one" ) ).Length == 0 && ReadWhitelist( routes.Whitelist( "two" ) ).SequenceEqual( new[] { alice } ), "alice is on exactly one server's whitelist" );

        routes.Match( routes.MockTicket( "bob", Now ).Text, Now );
        Check( routes.Ban( "mock", "alice", true ).Status == 204, "ban is 204" );
        Check( ReadWhitelist( routes.Whitelist( "two" ) ).SequenceEqual( new[] { bob } ), "a banned user is off the whitelist" );
        Reply banned = routes.Match( routes.MockTicket( "alice", Now ).Text, Now );
        Check( banned.Status == 403 && banned.Text.Length < 100, "a banned user gets 403 and no token" );
        Check( routes.Ban( "mo:ck", "alice", true ).Status == 400, "a ban with a bad provider name is 400" );
        Check( routes.Ban( "mock", "alice", false ).Status == 204, "unban is 204" );
        Check( routes.Match( routes.MockTicket( "alice", Now ).Text, Now ).Status == 200, "an unbanned user gets a token" );
    }
}
