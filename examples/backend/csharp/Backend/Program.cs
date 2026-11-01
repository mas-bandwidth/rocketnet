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
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading.Tasks;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using RocketNet.Backend;

// Key generation needs no server configuration and starts no HTTP listener.
if ( args.Length == 1 && args[0] == "--keygen" )
{
    BackendKeys.Write( Console.Out, buffer => RandomNumberGenerator.Fill( buffer ) );
    return 0;
}
if ( args.Length != 0 && !( args.Length == 2 && args[0] == "--mint" ) )
{
    Console.Error.WriteLine( "usage: rocketnet-backend [--keygen | --mint <client id>]" );
    return 1;
}

BackendConfig config;
try
{
    config = BackendConfig.Read( Environment.GetEnvironmentVariable, requireServer: args.Length == 2 );
}
catch ( ArgumentException e )
{
    Console.Error.WriteLine( $"rocketnet-backend: error: {e.Message}" );
    return 1;
}
if ( config.DevelopmentKey )
    Console.Error.WriteLine( "rocketnet-backend: WARNING: using the published development backend key. DEVELOPMENT ONLY: anyone can mint tokens for a server that accepts it. Set ROCKETNET_BACKEND_PRIVATE_KEY to deploy." );

string MintToken( BackendConfig selected, ulong clientId )
{
    ulong now = (ulong) DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    return Convert.ToBase64String( ConnectToken.Generate( selected, clientId, now, buffer => RandomNumberGenerator.Fill( buffer ), ConnectToken.SealAnonymous ) );
}

// --mint <client id> prints one connect token and exits, instead of serving
if ( args.Length == 2 && args[0] == "--mint" )
{
    if ( !ulong.TryParse( args[1], NumberStyles.None, CultureInfo.InvariantCulture, out ulong clientId ) )
    {
        Console.Error.WriteLine( $"rocketnet-backend: error: --mint takes a decimal client id, not \"{args[1]}\"" );
        return 1;
    }
    Console.WriteLine( MintToken( config, clientId ) );
    return 0;
}

WebApplicationBuilder builder = WebApplication.CreateSlimBuilder();
string listenHost = config.ListenAddress.Contains( ':' ) ? $"[{config.ListenAddress}]" : config.ListenAddress;
builder.WebHost.UseUrls( $"http://{listenHost}:{config.Port}" );
if ( !config.ListenAddressIsLoopback )
    Console.Error.WriteLine( $"rocketnet-backend: warning: listening on {config.ListenAddress}, not loopback: server registration and token issuance are unauthenticated here; production needs authenticated registration and player authorization" );
WebApplication app = builder.Build();

ServerRegistry registry = new ServerRegistry( RandomNumberGenerator.GetInt32 );
if ( config.ServerConfigured )
    registry.Register( "environment", config.ServerAddresses, Convert.ToBase64String( config.ServerPublicKey ) );

const int RegistrationBodyBytes = 4096;
async Task<(byte[] Data, bool TooLarge)> ReadRegistrationBody( Stream body )
{
    byte[] data = new byte[RegistrationBodyBytes + 1];
    int used = 0;
    while ( used < data.Length )
    {
        int count = await body.ReadAsync( data.AsMemory( used, data.Length - used ) );
        if ( count == 0 )
            return ( data.AsSpan( 0, used ).ToArray(), false );
        used += count;
    }
    return ( Array.Empty<byte>(), true );
}

app.MapPut( "/servers/{serverId}", async ( string serverId, HttpRequest request ) =>
{
    ( byte[] data, bool tooLarge ) = await ReadRegistrationBody( request.Body );
    if ( tooLarge )
        return Results.StatusCode( StatusCodes.Status413PayloadTooLarge );
    try
    {
        ServerRegistrationRequest registration = JsonSerializer.Deserialize<ServerRegistrationRequest>( data )
            ?? throw new ArgumentException( "empty server registration" );
        registry.Register( serverId, registration.Addresses, registration.PublicKey );
        return Results.NoContent();
    }
    catch ( JsonException e )
    {
        return Results.BadRequest( $"invalid server registration: {e.Message}" );
    }
    catch ( ArgumentException e )
    {
        return Results.BadRequest( e.Message );
    }
} );

app.MapGet( "/match/{clientId}", ( string clientId, HttpResponse response ) =>
{
    if ( !ulong.TryParse( clientId, NumberStyles.None, CultureInfo.InvariantCulture, out ulong id ) )
        return Results.BadRequest( "the client id is not a decimal 64 bit number" );
    RegisteredServer? server = registry.Random();
    if ( server == null )
        return Results.StatusCode( StatusCodes.Status503ServiceUnavailable );
    string token = MintToken( config.ForServer( server ), id );
    // a token is for one client, once: never cached
    response.Headers.CacheControl = "no-store";
    Console.Error.WriteLine( $"rocketnet-backend: matched client {id:x16} to {string.Join( ',', server.Addresses )}" );
    return Results.Text( token, "text/plain; charset=utf-8" );
} );

Console.Error.WriteLine( $"rocketnet-backend: started backend on {listenHost}:{config.Port}" );
app.Run();
return 0;
