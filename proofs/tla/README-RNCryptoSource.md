# The random source shared by the generators, init and term

`RNCryptoSource.tla` models how the key and token generators, `rocketnet_init`
and `rocketnet_term` share the bundled sodium's random source
(`sdk/rocketnet.cpp`: `rocketnet_crypto_acquire`, `rocketnet_crypto_release`,
`rocketnet_crypto_open_locked`, `rocketnet_crypto_close`).

Sodium opens its source in two unlocked steps: it reads its flag and, when the
flag is clear, opens a descriptor and sets it. A draw does that on its own
whenever the flag is clear: before `sodium_init`, and on macOS after
`rocketnet_term` has closed the source. The generators run before
`rocketnet_init`, beside `rocketnet_term`, on any thread. Each takes
`rocketnet_crypto_mutex`, opens the source when it is not open (`sodium_init`,
then `randombytes_stir`), counts itself a holder and lets the mutex go; it
draws, then uncounts itself. Init opens the source the same way
(`rocketnet_crypto_open_source`: the mutex, then `rocketnet_crypto_open_locked`)
without counting itself a holder; it is the actor `I` (`Threads + 1`), taking
the same open steps and returning where a generator would draw. Term marks
itself closing, waits for zero holders, closes the source and clears `open`;
a call that arrives meanwhile waits for the close. The model takes term as one
step, which is its safety; the closing flag keeps a stream of generator calls
from starving term.

Draws outside the four connect-token generators
(`rocketnet_connect_token.cpp`) take no hold: `rocketnet_random_bytes`,
`rocketnet_random_uint64`, and every draw the transports, protocols and token
tables make. The model assumes they run between `rocketnet_init` and
`rocketnet_term`, when the source is open and no term can close it, and does
not model them.

| name | meaning |
|---|---|
| `AtMostOneSource` | the source holds at most one descriptor |
| `OpenMeansSource` | `open` set means the source is open |
| `HeldDrawFromSource` | a counted draw finds the source open until it releases |

| config | expected |
|---|---|
| `MCRNCryptoSource.cfg` | pass (3 threads and init, 2 calls each, 2 terms) |
| `MCRNCryptoSourceBrokenNoLock.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoHold.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoClear.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoStir.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenInitNoLock.cfg` | `AtMostOneSource` |

`NoLock` is the code before the guard: two calls draw at once, both read the
flag clear and both open. `NoHold` opens the source but does not count the
call a holder: a term closes the source after the call let the mutex go, and
its draw opens it unlocked beside another call's draw. `NoClear` is a term
that leaves `open` set: the next calls skip the open and their draws open the
source unlocked. `NoStir` is an open that runs `sodium_init` alone, which
opens nothing once sodium is initialized. `InitNoLock` is `rocketnet_init`
opening the source without the mutex beside a generator that holds it: both
read `open` clear, both stir, and both open a descriptor.
