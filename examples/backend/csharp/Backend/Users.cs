/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Users: who a player is, whether they are banned, and which one server
    they may play on. The same design, wire forms and hashes as the Go
    backend's users.go.

    A player signs in with an auth provider and gets a ticket. The backend
    checks the ticket in ONE place, the VerifyTicket seam, which turns it
    into the provider's name and the provider's user id. This example has
    no real provider: MockProvider stands in for one. A title replaces the
    seam with its platform's own ticket check.

    The client id in every connect token this backend mints is the user
    hash: 64 bits of a BLAKE2b keyed with a backend secret, over
    "<provider>:<user id>". The client never sends or picks a client id.
    The id travels inside the private block, sealed to the server, and the
    backend's signature covers it, so the client can neither read nor
    change it.

    Bans are by user hash. A banned user gets no token. Each user hash sits
    on at most one server's whitelist, the server it was last matched to,
    and a server fetches its whitelist and disconnects any client that is
    not on it. A player who shares a token only lets someone else play as
    them, in their one seat, under their bans.
*/

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Sodium;

namespace RocketNet.Backend
{
    internal sealed class TicketException : Exception
    {
        public TicketException( string message ) : base( message ) {}
    }

    // The mock auth provider. It signs a ticket for any user id it is asked
    // for, so it stands in for a platform's sign-in, never for a real one. A
    // ticket is
    //
    //     expire timestamp (8 bytes, little endian) | user id length (1) | user id | MAC (32)
    //
    // as standard base64, where the MAC is BLAKE2b-256 keyed with the
    // provider's own secret over everything before it. Only the holder of
    // that secret can make a ticket or change one, and a ticket stops
    // working at its expire timestamp.

    internal sealed class MockProvider
    {
        public const string Name = "mock";
        public const ulong TicketSeconds = 60;
        public const int SecretKeyBytes = 32;
        public const int MaxUserIdBytes = 128;
        const int MacBytes = 32;
        const int HeaderBytes = 9;
        const int MaxTicketBytes = HeaderBytes + MaxUserIdBytes + MacBytes;
        public const int MaxTicketTextBytes = 4 * ( ( MaxTicketBytes + 2 ) / 3 );

        readonly byte[] key;

        public MockProvider( byte[] key )
        {
            if ( key.Length != SecretKeyBytes )
            {
                throw new ArgumentException( "the mock provider key is 32 bytes" );
            }
            this.key = key.ToArray();
        }

        public static bool ValidUserId( string userId ) => userId.Length > 0 && Encoding.UTF8.GetByteCount( userId ) <= MaxUserIdBytes;

        byte[] Mac( ReadOnlySpan<byte> data ) => GenericHash.Hash( data.ToArray(), key, MacBytes );

        // issues a ticket for userId that expires expireSeconds after now (unix seconds)
        public string Issue( string userId, ulong now, ulong expireSeconds )
        {
            if ( !ValidUserId( userId ) )
            {
                throw new ArgumentException( $"a user id is 1 to {MaxUserIdBytes} bytes" );
            }
            byte[] user = Encoding.UTF8.GetBytes( userId );
            byte[] ticket = new byte[HeaderBytes + user.Length + MacBytes];
            BinaryPrimitives.WriteUInt64LittleEndian( ticket, now + expireSeconds );
            ticket[8] = (byte) user.Length;
            user.CopyTo( ticket, HeaderBytes );
            Mac( ticket.AsSpan( 0, HeaderBytes + user.Length ) ).CopyTo( ticket, HeaderBytes + user.Length );
            return Convert.ToBase64String( ticket );
        }

        // checks a ticket at now (unix seconds) and returns its user id;
        // throws TicketException
        public string Verify( string text, ulong now )
        {
            if ( text.Length > MaxTicketTextBytes )
            {
                throw new TicketException( "the ticket is not valid" );
            }
            byte[] ticket = new byte[MaxTicketBytes + 3];
            if ( !Convert.TryFromBase64String( text, ticket, out int bytes ) || bytes < HeaderBytes + 1 + MacBytes )
            {
                throw new TicketException( "the ticket is not valid" );
            }
            int userBytes = ticket[8];
            if ( bytes != HeaderBytes + userBytes + MacBytes )
            {
                throw new TicketException( "the ticket is not valid" );
            }
            int signedBytes = HeaderBytes + userBytes;
            if ( !CryptographicOperations.FixedTimeEquals( Mac( ticket.AsSpan( 0, signedBytes ) ), ticket.AsSpan( signedBytes, MacBytes ) ) )
            {
                throw new TicketException( "the ticket is not valid" );
            }
            if ( BinaryPrimitives.ReadUInt64LittleEndian( ticket ) <= now )
            {
                throw new TicketException( "the ticket has expired" );
            }
            string userId;
            try
            {
                userId = new UTF8Encoding( false, true ).GetString( ticket, HeaderBytes, userBytes );
            }
            catch ( ArgumentException )
            {
                throw new TicketException( "the ticket is not valid" );
            }
            if ( !ValidUserId( userId ) )
            {
                throw new TicketException( "the ticket is not valid" );
            }
            return userId;
        }
    }

    // THE AUTH PROVIDER SEAM. It checks the ticket a client sent and returns
    // the provider's name and that provider's user id for the player, or
    // throws TicketException. A title replaces it with its platform's own
    // ticket check (or several, one per platform, each returning its own
    // provider name). The provider name may not contain ':', so
    // "<provider>:<user id>" reads only one way.

    internal delegate (string Provider, string UserId) VerifyTicket( string ticket, ulong now );

    // The users: bans and seats, keyed by user hash, in memory like the
    // server registry. A real backend keeps them in its database.

    internal sealed class Users
    {
        public const int WhitelistEntryBytes = 8;

        readonly object mutex = new object();
        readonly byte[] hashKey;
        readonly HashSet<ulong> banned = new HashSet<ulong>();
        readonly Dictionary<ulong, string> seats = new Dictionary<ulong, string>();  // user hash -> the id of the one server whose whitelist holds it

        public Users( byte[] hashKey )
        {
            if ( hashKey.Length != MockProvider.SecretKeyBytes )
            {
                throw new ArgumentException( "the user hash key is 32 bytes" );
            }
            this.hashKey = hashKey.ToArray();
        }

        public static VerifyTicket VerifyWith( MockProvider mock ) => ( ticket, now ) => ( MockProvider.Name, mock.Verify( ticket, now ) );

        // the user hash, which is the client id: the first 8 bytes, read
        // little endian, of BLAKE2b-256 keyed with the backend's secret over
        // "<provider>:<user id>"
        public ulong Hash( string provider, string userId )
        {
            if ( string.IsNullOrEmpty( provider ) || provider.Contains( ':' ) )
            {
                throw new ArgumentException( "a provider name is not empty and has no ':'" );
            }
            if ( !MockProvider.ValidUserId( userId ) )
            {
                throw new ArgumentException( $"a user id is 1 to {MockProvider.MaxUserIdBytes} bytes" );
            }
            byte[] hash = GenericHash.Hash( Encoding.UTF8.GetBytes( $"{provider}:{userId}" ), hashKey, 32 );
            return BinaryPrimitives.ReadUInt64LittleEndian( hash );
        }

        public void SetBanned( ulong userHash, bool isBanned )
        {
            lock ( mutex )
            {
                if ( isBanned )
                {
                    banned.Add( userHash );
                }
                else
                {
                    banned.Remove( userHash );
                }
            }
        }

        // seats userHash on serverId, moving it off any other server's
        // whitelist; a banned user is refused and seated nowhere
        public bool Seat( ulong userHash, string serverId )
        {
            lock ( mutex )
            {
                if ( banned.Contains( userHash ) )
                {
                    return false;
                }
                seats[userHash] = serverId;
                return true;
            }
        }

        // the client ids allowed on serverId: matched there and not banned, ascending
        public ulong[] Whitelist( string serverId )
        {
            ulong[] list;
            lock ( mutex )
            {
                list = seats.Where( seat => seat.Value == serverId && !banned.Contains( seat.Key ) ).Select( seat => seat.Key ).ToArray();
            }
            Array.Sort( list );
            return list;
        }

        // The whitelist on the wire: each client id as 8 bytes little endian,
        // ascending, nothing else. An empty list is an empty body.
        public static byte[] WriteWhitelist( ulong[] list )
        {
            byte[] data = new byte[list.Length * WhitelistEntryBytes];
            for ( int i = 0; i < list.Length; i++ )
            {
                BinaryPrimitives.WriteUInt64LittleEndian( data.AsSpan( i * WhitelistEntryBytes ), list[i] );
            }
            return data;
        }

        // a backend secret from the environment (base64 of 32 bytes), or a
        // fresh random one when the variable is unset
        public static byte[] ReadSecretKey( Func<string, string?> getenv, string name, Action<byte[]> random, out bool set )
        {
            string? text = getenv( name );
            set = !string.IsNullOrEmpty( text );
            if ( set )
            {
                return BackendConfig.Base64( text!, MockProvider.SecretKeyBytes ) ?? throw new ArgumentException( $"{name} is not the base64 of a 32 byte key" );
            }
            byte[] key = new byte[MockProvider.SecretKeyBytes];
            random( key );
            return key;
        }
    }

    // What one route answers: a status, and a text or binary body.

    internal readonly record struct Reply( int Status, string Text, byte[]? Data = null );

    // The routes for users, as plain functions of their inputs and the time
    // (unix seconds), so the unit tests run them without a socket or a
    // clock; Program.cs maps them onto HTTP.
    //
    //     POST   /mock/ticket/{userId}            the mock auth provider signs a ticket
    //     POST   /match                           ticket in, connect token out
    //     PUT    /bans/{provider}/{userId}        ban a user
    //     DELETE /bans/{provider}/{userId}        lift the ban
    //     GET    /servers/{serverId}/whitelist    the client ids allowed on a server

    internal sealed class UserRoutes
    {
        readonly BackendConfig config;
        readonly ServerRegistry registry;
        readonly Users users;
        readonly MockProvider mock;
        readonly VerifyTicket verify;
        readonly Action<byte[]> random;
        readonly Func<byte[], byte[], byte[]> seal;

        public UserRoutes( BackendConfig config, ServerRegistry registry, Users users, MockProvider mock, VerifyTicket verify, Action<byte[]> random, Func<byte[], byte[], byte[]> seal )
        {
            this.config = config;
            this.registry = registry;
            this.users = users;
            this.mock = mock;
            this.verify = verify;
            this.random = random;
            this.seal = seal;
        }

        // the mock auth provider's sign-in: any user id gets a ticket. It
        // stands in for a platform's own sign-in, which runs on the client
        // and on the platform's servers, not in the game's backend
        public Reply MockTicket( string userId, ulong now )
        {
            if ( !MockProvider.ValidUserId( userId ) )
            {
                return new Reply( 400, $"a user id is 1 to {MockProvider.MaxUserIdBytes} bytes" );
            }
            return new Reply( 200, mock.Issue( userId, now, MockProvider.TicketSeconds ) );
        }

        // the match: the ticket in, a connect token out. A bad or expired
        // ticket is 401, a banned user 403, no servers 503; otherwise the
        // user is seated on one server and gets a connect token for it whose
        // client id is their user hash
        public Reply Match( string ticket, ulong now )
        {
            ulong userHash;
            try
            {
                ( string provider, string userId ) = verify( ticket.Trim(), now );
                userHash = users.Hash( provider, userId );
            }
            catch ( TicketException e )
            {
                return new Reply( 401, e.Message );
            }
            catch ( ArgumentException e )
            {
                return new Reply( 401, e.Message );
            }
            ( string Id, RegisteredServer Server )? picked = registry.Pick();
            if ( picked == null )
            {
                return new Reply( 503, "no servers are registered" );
            }
            // seated before the token goes out, so the user is on the
            // server's whitelist before they can connect; a ban refuses the seat
            if ( !users.Seat( userHash, picked.Value.Id ) )
            {
                return new Reply( 403, "this user is banned" );
            }
            byte[] token = ConnectToken.Generate( config.ForServer( picked.Value.Server ), userHash, now, random, seal );
            Console.Error.WriteLine( $"rocketnet-backend: matched user {userHash:x16} to server {picked.Value.Id}" );
            return new Reply( 200, Convert.ToBase64String( token ) );
        }

        public Reply Ban( string provider, string userId, bool banned )
        {
            ulong userHash;
            try
            {
                userHash = users.Hash( provider, userId );
            }
            catch ( ArgumentException e )
            {
                return new Reply( 400, e.Message );
            }
            users.SetBanned( userHash, banned );
            Console.Error.WriteLine( $"rocketnet-backend: {( banned ? "banned" : "unbanned" )} user {userHash:x16}" );
            return new Reply( 204, "" );
        }

        // the whitelist: 8 bytes little endian per client id, ascending; 404
        // for a server that is not registered, so it registers again
        public Reply Whitelist( string serverId )
        {
            if ( !registry.Has( serverId ) )
            {
                return new Reply( 404, "this server is not registered" );
            }
            return new Reply( 200, "", Users.WriteWhitelist( users.Whitelist( serverId ) ) );
        }
    }
}
