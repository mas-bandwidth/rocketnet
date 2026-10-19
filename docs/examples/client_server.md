# Client and server

Two separate programs over real UDP on loopback, showing RocketNet's security
model end to end with production keys.

## The model

The backend holds the Ed25519 backend secret key and gives the matching public
key to every server. The server holds its own X25519 server secret key and the
backend public key. The client's matchmaker stub holds the backend secret key
and the server's public key, and mints a production connect token: signed by
the backend, sealed to that server, naming the client id, protocol id, server
address, expiry and timeout. The server accepts only a token it can verify,
open, and find unexpired for its own protocol id, so another title is refused.
The development token that `hello` uses is for development only.

## The key file

The server generates the backend keypair and its own server keypair through the
public keygen calls and writes `client_server.keys` in the working directory.
The client reads it to play the backend. One small file is plainer than piping
a key through the run script, which starts the pair with no arguments, and it
keeps key material out of every log line. A deployment distributes these keys
out of band.

## Run it

From the repository root, after the normal build, in two terminals:

```sh
./bin/example_client_server_server
./bin/example_client_server_client
```

The server listens on `127.0.0.1:40000`, receives one reliable-ordered message
from the client, echoes it, and exits after its first client disconnects. Both
programs finish in under ten seconds and print `PASS` as their last line.
`tools/run_examples.py --build build` runs the pair in that order. To see the
refusal, start a fresh server and run the client's own flag:

```sh
./bin/example_client_server_client --wrong-protocol
```
The client mints the token for another protocol id, the server ignores it, and
the client prints `expected refusal:` with the refused state and then `PASS`.
Stop that server when the run finishes.

## What the output means

```
server received: hello from the client
client received: hello from the client
client state: connected -> disconnected
client disconnect reason: none
PASS
```
Each `client state:` line is a state change from the public callback. The
server prints the message it received; the client prints the echo. The
disconnect reason is `none` for a clean disconnect. Library log lines are
informational and not part of the check. The key file is a local artifact and
can be deleted after a run.
## Public calls it uses
- `rocketnet_init`, `rocketnet_term`
- `rocketnet_generate_backend_keypair`, `rocketnet_generate_server_keypair`,
  `rocketnet_write_production_connect_token`
- `rocketnet_server_default_config`, `rocketnet_server_create`, `rocketnet_server_update`,
  `rocketnet_server_port`, `rocketnet_server_max_clients`, `rocketnet_server_num_connected_clients`,
  `rocketnet_server_receive_message`, `rocketnet_server_release_message`, `rocketnet_server_start_message`,
  `rocketnet_server_finish_message`, `rocketnet_server_send_packets`, `rocketnet_server_disconnect_reason`,
  `rocketnet_server_destroy`
- `rocketnet_client_default_config`, `rocketnet_client_create`, `rocketnet_client_state`,
  `rocketnet_client_state_name`, `rocketnet_client_update`, `rocketnet_client_receive_message`,
  `rocketnet_client_release_message`, `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_send_packets`, `rocketnet_client_disconnect`, `rocketnet_client_disconnect_reason`,
  `rocketnet_client_destroy`
- `rocketnet_time`, `rocketnet_sleep`
The header at this base names start, finish, receive, release and `send_packets`
without the `*_message` suffix a later lifecycle card introduces, so the
examples use the header's names.
