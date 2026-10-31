# RNXdpReplyMacsLayout

The XDP server transport's reply MAC table with no gateway, at the layout of
`sdk/rocketnet_xdp_frame.h` and with the code's own arithmetic.
[RNXdpReplay](README-RNXdpReplay.md) models the table as a map from address to
learned and answered MAC; this model carries the open addressing that map is
made of: the index mask, the probe window, the slot states, the stamps and the
evictions.

## State

| Model | Code |
|---|---|
| `home[a]` | `rocketnet_xdp_reply_macs_slot(a)`, the FNV-1a hash masked by `ROCKETNET_XDP_REPLY_MAC_ENTRIES - 1` |
| `st[s]` | `entries[s].state`: `EMPTY`, `LIVE` or `ANSWERED` |
| `addr[s]`, `mac[s]`, `stamp[s]` | `entries[s].address`, `.mac`, `.learned` |
| `count` | `learn_count` |
| `N`, `P` | `ROCKETNET_XDP_REPLY_MAC_ENTRIES`, `ROCKETNET_XDP_REPLY_MAC_PROBES` |

`home` is chosen at `Init` from every map of addresses to slots, so every
collision pattern is checked. Probe `i` of address `a` is slot
`(home[a] + i) % N`, for `i < P`.

The table has no tombstone state: a forget turns a `LIVE` entry into an
`ANSWERED` one in place, and nothing returns a used slot to `EMPTY` but a reset
of the whole table. The forgotten `ANSWERED` entry is the slot a lookup for
another address must probe past, the role a tombstone has in other tables.

Only the order of the stamps matters to the code (learn's and answer's oldest
choice), and `learn_count` is above every stamp, so the `VIEW` keeps each used
slot's rank among the used slots' stamps and leaves `learn_count` out: two
states with the same ranks have the same behaviors. The ghost `abs[a]` is the
contract: what a lookup of `a` must return (its learned MAC, else its answered
MAC, else none).

## Actions and the code they mirror

- **`Learn(a, m)`**: `rocketnet_xdp_reply_macs_learn` of a usable MAC: the
  address's `LIVE` entry, else its `ANSWERED` entry
  (`rocketnet_xdp_reply_macs_find_state`), else the first slot of the window
  that is not `LIVE`, else the `LIVE` entry with the smallest stamp. The slot
  becomes `LIVE` with the MAC and the next stamp. A learn that takes another
  address's slot removes that address from the contract.
- **`Answer(a, m)`**: `rocketnet_xdp_reply_mac` with a usable answered MAC and
  no learned entry, which calls `rocketnet_xdp_reply_macs_answer`: the
  address's `ANSWERED` entry, else the first `EMPTY` slot of the window, else
  the `ANSWERED` entry with the smallest stamp, else nothing is kept.
- **`Forget(a)`**: `rocketnet_xdp_reply_macs_forget`: the `LIVE` entry
  becomes `ANSWERED` in place.
- **`Reset`**: `rocketnet_xdp_reply_macs_reset` with no gateway.

The lookups are `rocketnet_xdp_reply_macs_find_state`: from the home slot,
stop at the first `EMPTY` slot, return the first slot of the state sought
holding the address, at most `P` probes. They are read-only, so they are
operators (`Find`, `Lookup`), not actions.

## Invariants

- **`TableIsMap`**: every address's lookup returns the contract's value.
- **`OneEntryPerAddress`**: an address holds at most one used slot.
- **`NoEntryPastEmpty`**: every used slot lies within `P` probes of its
  address's home, with no `EMPTY` slot before it on the way.
- **`EvictOnlyOldest`**: a learn evicts a `LIVE` entry only from a window
  that is all `LIVE`, and only the one with the smallest stamp; an answer never
  takes a `LIVE` slot.

## The instance

`MCRNXdpReplyMacsLayout.cfg`: four entries, a window of two, three addresses
over every home map, two MACs. The window is half the table, so full windows,
probes that wrap past the last slot and evictions are all reached.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNXdpReplyMacsLayoutBrokenForgottenEndsProbe.cfg`**
   (`SkipOtherEntries = FALSE`): `rocketnet_xdp_reply_macs_find_state`
   returning `NULL` at a slot that is neither `LIVE` nor the state sought, as
   if a forgotten entry were empty (the tombstone treated as empty). Violates
   `TableIsMap`: `a1` and `a2` share home 0 and are learned into slots 0 and
   1; `a1` is forgotten, so slot 0 is `ANSWERED`; the lookup of `a2`'s learned
   MAC stops at slot 0 and returns nothing while `a2` is learned.
2. **`MCRNXdpReplyMacsLayoutBrokenEvictNewest.cfg`** (`EvictOldest = FALSE`):
   learn's eviction comparing `candidate->learned > oldest->learned`. Violates
   `EvictOnlyOldest`: `a1`, `a2` and `a3` share home 0; `a1` then `a2` fill
   the window; learning `a3` evicts `a2`, learned after `a1`.
3. **`MCRNXdpReplyMacsLayoutBrokenInsertWindowLong.cfg`**
   (`InsertWindowExact = FALSE`): learn's insert loop running
   `i <= ROCKETNET_XDP_REPLY_MAC_PROBES`. Violates `TableIsMap`: `a1` (home 0)
   takes slot 0, `a3` (home 1) slot 1; `a2` (home 0) finds both window slots
   `LIVE` and takes slot 2, offset 2; every lookup of `a2` stops after slots 0
   and 1 and returns nothing, and its next learn would insert a second entry.

## Runs

The records are in `RUNS.tsv` (group `rnxdpreplymacslayout`), made with the
pinned runner as `proofs/tla/README.md` describes.
