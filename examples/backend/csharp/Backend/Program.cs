/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    A very simple backend, in the spirit of yojimbo's matcher: one HTTP
    endpoint that mints a RocketNet connect token for a client and returns it
    as base64. The C client passes the string to rocketnet_client_connect,
    and the token sends it to the one server it names. ConnectToken.cs
    writes the token.

    Keys and settings come from the environment (see README.md). With none
    set, the backend uses the published development backend key, which the
    standard prints, so ANYONE can mint tokens a server using it accepts. It
    is for local runs only, and a release server refuses it at start.

    A real backend authenticates the player before minting a token for them;
    this one mints for any client id it is asked for.
*/

using System;
using System.Globalization;
using System.Security.Cryptography;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using RocketNet.Backend;

BackendConfig config;
try
{
    config = BackendConfig.Read( Environment.GetEnvironmentVariable );
}
catch ( ArgumentException e )
{
    Console.Error.WriteLine( $"rocketnet-backend: error: {e.Message}" );
    return 1;
}
if ( config.DevelopmentKey )
    Console.Error.WriteLine( "rocketnet-backend: WARNING: using the published development backend key. DEVELOPMENT ONLY: anyone can mint tokens for a server that accepts it. Set ROCKETNET_BACKEND_PRIVATE_KEY to deploy." );

string MintToken( ulong clientId )
{
    ulong now = (ulong) DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    return Convert.ToBase64String( ConnectToken.Generate( config, clientId, now, buffer => RandomNumberGenerator.Fill( buffer ), ConnectToken.SealAnonymous ) );
}

// --mint <client id> prints one connect token and exits, instead of serving
if ( args.Length == 2 && args[0] == "--mint" )
{
    if ( !ulong.TryParse( args[1], NumberStyles.None, CultureInfo.InvariantCulture, out ulong clientId ) )
    {
        Console.Error.WriteLine( $"rocketnet-backend: error: --mint takes a decimal client id, not \"{args[1]}\"" );
        return 1;
    }
    Console.WriteLine( MintToken( clientId ) );
    return 0;
}
if ( args.Length != 0 )
{
    Console.Error.WriteLine( "usage: rocketnet-backend [--mint <client id>]" );
    return 1;
}

WebApplicationBuilder builder = WebApplication.CreateSlimBuilder();
builder.WebHost.UseUrls( $"http://*:{config.Port}" );
WebApplication app = builder.Build();

app.MapGet( "/match/{clientId}", ( string clientId, HttpResponse response ) =>
{
    if ( !ulong.TryParse( clientId, NumberStyles.None, CultureInfo.InvariantCulture, out ulong id ) )
        return Results.BadRequest( "the client id is not a decimal 64 bit number" );
    string token = MintToken( id );
    // a token is for one client, once: never cached
    response.Headers.CacheControl = "no-store";
    Console.Error.WriteLine( $"rocketnet-backend: matched client {id:x16} to {string.Join( ',', config.ServerAddresses )}" );
    return Results.Text( token, "text/plain; charset=utf-8" );
} );

Console.Error.WriteLine( $"rocketnet-backend: started backend on port {config.Port}, minting tokens for {string.Join( ',', config.ServerAddresses )}" );
app.Run();
return 0;
