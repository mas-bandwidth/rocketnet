/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0 or later, or the Más Bandwidth Source License 1.0 (MBSL-1.0). See LICENSE and MBSL.md.
*/

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace RocketNet.Backend
{
    internal sealed class ServerRegistrationRequest
    {
        [JsonPropertyName( "addresses" )]
        public string[] Addresses { get; set; } = Array.Empty<string>();

        [JsonPropertyName( "public_key" )]
        public string PublicKey { get; set; } = "";
    }

    internal sealed class RegisteredServer
    {
        public string[] Addresses { get; }
        public byte[] PublicKey { get; }

        public RegisteredServer( string[] addresses, byte[] publicKey )
        {
            Addresses = addresses;
            PublicKey = publicKey;
        }
    }

    internal sealed class ServerRegistry
    {
        readonly object mutex = new object();
        readonly Dictionary<string, RegisteredServer> servers = new Dictionary<string, RegisteredServer>();
        readonly Func<int, int> choose;

        public ServerRegistry( Func<int, int> choose )
        {
            this.choose = choose;
        }

        public void Register( string id, string[] addresses, string publicKeyText )
        {
            if ( string.IsNullOrEmpty( id ) || id.Length > 128 )
            {
                throw new ArgumentException( "server id is empty or longer than 128 characters" );
            }
            if ( addresses == null )
            {
                throw new ArgumentException( "addresses is required" );
            }
            foreach ( string? address in addresses )
            {
                if ( string.IsNullOrWhiteSpace( address ) )
                {
                    throw new ArgumentException( "addresses contains an empty address" );
                }
            }
            ConnectToken.WriteServerAddresses( new SignedConnectData(), addresses );
            byte[] publicKey = ( string.IsNullOrEmpty( publicKeyText ) ? null : BackendConfig.Base64( publicKeyText, ConnectToken.KeyBytes ) )
                ?? throw new ArgumentException( "public_key is not the base64 of a 32 byte server public key" );
            RegisteredServer server = new RegisteredServer( addresses.ToArray(), publicKey );
            lock ( mutex )
            {
                servers[id] = server;
            }
        }

        public RegisteredServer? Random() => Pick()?.Server;

        // selects a registered server with choose, in id order so a choice
        // of index always names the same server, and returns its id with it
        public ( string Id, RegisteredServer Server )? Pick()
        {
            lock ( mutex )
            {
                if ( servers.Count == 0 )
                {
                    return null;
                }
                int index = choose( servers.Count );
                if ( index < 0 || index >= servers.Count )
                {
                    throw new InvalidOperationException( "failed to select a registered server" );
                }
                string id = servers.Keys.Order( StringComparer.Ordinal ).ElementAt( index );
                RegisteredServer server = servers[id];
                return ( id, new RegisteredServer( server.Addresses.ToArray(), server.PublicKey.ToArray() ) );
            }
        }

        public bool Has( string id )
        {
            lock ( mutex )
            {
                return servers.ContainsKey( id );
            }
        }
    }
}
