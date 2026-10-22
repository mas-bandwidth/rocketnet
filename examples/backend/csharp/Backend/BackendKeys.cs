/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

using System;
using System.IO;
using System.Security.Cryptography;
using Sodium;

namespace RocketNet.Backend
{
    internal static class BackendKeys
    {
        // The entropy callback lets the unit fixture supply a known signing seed.
        public static void Write( TextWriter output, Action<byte[]> random )
        {
            byte[] seed = new byte[32];
            byte[] privateKey = Array.Empty<byte>();
            try
            {
                random( seed );
                var pair = PublicKeyAuth.GenerateKeyPair( seed );
                privateKey = pair.PrivateKey;
                output.WriteLine( $"ROCKETNET_SERVER_BACKEND_PUBLIC_KEY={Convert.ToBase64String( pair.PublicKey )}" );
                output.WriteLine( $"ROCKETNET_BACKEND_PRIVATE_KEY={Convert.ToBase64String( privateKey )}" );
            }
            finally
            {
                CryptographicOperations.ZeroMemory( seed );
                CryptographicOperations.ZeroMemory( privateKey );
            }
        }
    }
}
