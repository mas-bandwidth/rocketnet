/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
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
                throw new ArgumentException( "server id is empty or longer than 128 characters" );
            if ( addresses == null )
                throw new ArgumentException( "addresses is required" );
            ConnectToken.WriteServerAddresses( new SignedConnectData(), addresses );
            byte[] publicKey = ( string.IsNullOrEmpty( publicKeyText ) ? null : BackendConfig.Base64( publicKeyText, ConnectToken.KeyBytes ) )
                ?? throw new ArgumentException( "public_key is not the base64 of a 32 byte server public key" );
            RegisteredServer server = new RegisteredServer( addresses.ToArray(), publicKey );
            lock ( mutex )
                servers[id] = server;
        }

        public RegisteredServer? Random()
        {
            lock ( mutex )
            {
                if ( servers.Count == 0 )
                    return null;
                int index = choose( servers.Count );
                if ( index < 0 || index >= servers.Count )
                    throw new InvalidOperationException( "failed to select a registered server" );
                foreach ( RegisteredServer server in servers.Values )
                {
                    if ( index-- == 0 )
                        return new RegisteredServer( server.Addresses.ToArray(), server.PublicKey.ToArray() );
                }
                throw new InvalidOperationException( "failed to select a registered server" );
            }
        }
    }
}
