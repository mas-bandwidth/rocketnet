# Sessions and connect tokens

RocketNet authenticates a connection with a signed connect token.
The backend chooses a client id and one server, writes one IPv4 address, one
IPv6 address, or one of each into the token, and gives the token to the client
over its authenticated service channel. The client connects with the token;
it does not receive a separate server address argument.

A server verifies the token signature, expiry, protocol id, advertised address
for the request's address family, sealed private data and handshake proof. A
valid token identifies one client and permits one live connection. The
used-token history binds it to the source address whose response completes the
handshake, so another source address cannot reuse the active token. A second
connection for the same client id is refused while its slot remains live.

The session keys derive from the token keys, fresh endpoint ephemeral keys and
the connection salt. Handshake secrets are wiped when their lifetime ends.
Authenticated session packets pass the replay window before their contents are
admitted. Disconnect resets per-connection protocol and message state while
preserving the client object's configured transport and reusable allocation;
a later connect supplies a fresh token, which names the server address.

[SECURITY.md](../SECURITY.md), [STANDARD.md](../STANDARD.md) and
[standard/TRANSPORT.md](../standard/TRANSPORT.md) define the shipped security
and wire contracts. Backend-wide identity coordination and rotating keyed
prefilters are planned extensions and are not part of this release.
