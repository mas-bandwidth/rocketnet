# The random source shared by the generators, init and term

`RNCryptoSource.tla` models how the key and token generators, `rocketnet_init`
and `rocketnet_term` share the bundled sodium's random source
(`sdk/rocketnet.cpp`: `rocketnet_crypto_acquire`, `rocketnet_crypto_release`,
`rocketnet_crypto_open_source`, `rocketnet_crypto_close`).

Sodium opens its source in two unlocked steps: it reads its flag and, when the
flag is clear, opens a descriptor and sets it. A draw does that on its own
whenever the flag is clear: before `sodium_init`, and on macOS after
`rocketnet_term` has closed the source. The generators run before
`rocketnet_init`, beside `rocketnet_term`, on any thread. Each holds
`rocketnet_crypto_mutex` shared across its draws; when the source is not open
it opens it with the mutex exclusive (`sodium_init`, then `randombytes_stir`)
and starts again. Init opens it the same way. Term closes it with the mutex
exclusive, so it waits for every draw in flight.

| name | meaning |
|---|---|
| `AtMostOneSource` | the source holds at most one descriptor |
| `OpenMeansSource` | `open` set means the source is open |
| `HeldDrawFromSource` | a draw holding the mutex finds the source open until it releases |

| config | expected |
|---|---|
| `MCRNCryptoSource.cfg` | pass (3 threads, 2 calls each, 2 terms) |
| `MCRNCryptoSourceBrokenNoLock.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoShared.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoClear.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoStir.cfg` | `AtMostOneSource` |

`NoLock` is the code before the guard: two calls draw at once, both read the
flag clear and both open. `NoShared` opens the source but does not hold the
mutex across the draw: a term closes the source after a call has read `open`,
and that call's draw opens it unlocked beside another call's exclusive open.
`NoClear` is a term that leaves `open` set: the next calls skip the open and
their draws open the source unlocked. `NoStir` is an open that runs
`sodium_init` alone, which opens nothing once sodium is initialized.
