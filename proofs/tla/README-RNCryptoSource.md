# The random source opened before and after init

`RNCryptoSource.tla` models how the key and token generators, `rocketnet_init`
and `rocketnet_term` share the bundled sodium's random source
(`sdk/rocketnet.cpp`: `rocketnet_crypto_ready`, `rocketnet_crypto_close`).

Sodium opens its source in two unlocked steps: it reads its flag and, when the
flag is clear, opens a descriptor and sets it. A draw does that on its own
whenever the flag is clear: before `sodium_init`, and on macOS after
`rocketnet_term` has closed the source. The generators run before
`rocketnet_init` on any thread, so each runs `rocketnet_crypto_ready` first:
one load of a published flag, then the lock, a second load, `sodium_init` and
`randombytes_stir`, and the publish. Term closes the source and clears the flag
under the same lock, with no generator call in flight.

| name | meaning |
|---|---|
| `AtMostOneSource` | the source holds at most one descriptor |
| `OpenMeansSource` | a published flag means the source is open |
| `DrawnFromSource` | every completed draw found the source open |

| config | expected |
|---|---|
| `MCRNCryptoSource.cfg` | pass (3 threads, 2 terms) |
| `MCRNCryptoSourceBrokenNoLock.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoClear.cfg` | `AtMostOneSource` |
| `MCRNCryptoSourceBrokenNoStir.cfg` | `AtMostOneSource` |

`NoLock` is the code before the guard: two calls draw at once, both read the
flag clear and both open. `NoClear` is a term that leaves the flag set: the
next cycle's calls skip the lock and open in their draws. `NoStir` is a guard
that runs `sodium_init` alone, which opens nothing once sodium is initialized,
so after a term the draws open the source unlocked.
