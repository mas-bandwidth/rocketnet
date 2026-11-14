# RNClientTransportOrder

`RNClientTransportOrder.tla` models the client transport order at create
(`rocketnet_client_transport_create_impl` in
`sdk/rocketnet_client_transport.cpp`, each socket opened and connected by
`rocketnet_client_transport_open_connected`): which sockets the client
opens, in what order, and how the create ends. It says nothing about the
handshake that follows or the family race that runs after an IPv6 grant
(`ServerConnection.tla` RaceFamilies, `RNClientRace.tla`).

## Abstraction

- **The token.** `token` is the set of families the connect token names:
  IPv4, IPv6 or both. The host is one with IPv6, so a token naming IPv6
  selects its IPv6 server first (`rocketnet_client_protocol_pick_server_address`).
  A host without IPv6 selects the IPv4 server and is a one-family transport order.
- **The outcomes.** `outcome[<<r, f>>]` is what the socket for family `f`
  on candidate `r` does: `"ok"` (opened and connected), `"refused"` (an open
  refused, or a connect the kernel refused: no route, a link-local address
  with no interface) or `"oom"` (the open failed for want of memory). The
  environment chooses every outcome once, at Init, so the checker covers
  every combination.
- **The budget.** `budget[r]` is candidate `r`'s memory plan against the
  operator budget (`rocketnet_client_transport_resolve`): `"fits"`,
  `"refused"` (the plan exceeds a non-zero budget), `"unknown"` (a built-in
  transport with no plan; neither refused nor counted as fitting) or
  `"custom"` (a row from `ROCKETNET_TRANSPORT_REGISTRY_HEADER`, which has no
  SDK plan: not refused, and still tried). The environment chooses it once,
  at Init. When some candidate is refused and none fits or is custom, the
  create is refused before the first allocation (`NO_TRANSPORT_FOUND`, result
  `"nobudget"`; `rocketnet_transport_budget_count` and
  `rocketnet_transport_budget_refuses` in `sdk/rocketnet_driver.h`, which the
  server resolve takes too).
- **TryPrimary.** A candidate the budget refused is skipped, with no
  socket, and the transport order falls to the next candidate (the
  `budget_refused` check at the top of the transport order loop). Otherwise
  the socket for the server the token selected, on the current candidate. Granted on `"ok"`; the create ends on `"oom"`. On a
  refusal of the IPv6 socket of a token naming both families, the same
  candidate is tried for the IPv4 server next; any other refusal falls to
  the next candidate.
- **TryFallback.** The IPv4 server's socket on the same candidate. Granted
  on `"ok"`, the create ends on `"oom"`, and a refusal falls to the next
  candidate.
- **The end.** The last candidate's refusal exhausts the list
  (`NO_TRANSPORT_FOUND`). A list of one candidate is a pin: the model
  at `NumCandidates = 1` is the pinned transport.
- **Done.** The create has ended; the model stutters.

## Properties

- `TypeOK` holds the ranges.
- `TriedInToken`: every socket dials a server the token names.
- `TransportOrder`: the sockets are tried candidate by candidate over the
  candidates the budget did not refuse, the selected family first and the
  IPv4 fallback next, no such candidate skipped.
- `NoSocketForRefusedPlan`: no socket is opened for a candidate the budget
  refused.
- `NoBudgetOnlyWhenNoneTryable`: the budget refuses the create only when
  every candidate was refused or is a built-in with no plan; never while a
  candidate fits or a custom transport is listed.
- `NoExhaustWhileReachable`: the transport order exhausts only when no socket the
  token names, on any candidate the budget did not refuse, would have been
  granted.
- `NoTryAfterOom`: a refusal for want of memory is the last socket tried, on
  either family.
- `OomOnlyFromOom`: the create ends out of memory only on such a refusal.
- `GrantIsLast`: the grant is the last socket tried, and it opened and
  connected.
- `FallbackOnlyAfterRefusal`: the IPv4 server of a token naming both families
  is granted only after the IPv6 socket on the same candidate was refused.
- `Terminates` (weak fairness on the two steps): every create ends.

## Cases

- `proofs/tla/MCRNClientTransportOrder.cfg` passes: three candidates, every token
  shape, every outcome and every budget plan, all invariants and
  `Terminates`.
- `proofs/tla/MCRNClientTransportOrderPin.cfg` passes: the same at one candidate, a
  pinned transport.
- `proofs/tla/MCRNClientTransportOrderBrokenBudgetOpens.cfg` violates
  `NoSocketForRefusedPlan`. `Broken = {"budgetopens"}` opens a socket for a
  candidate the budget refused: candidate 1 refused, candidate 2 fitting,
  and the client tries candidate 1's socket first. In the code the
  transport order loop's `budget_refused[candidate]` check `continue`s
  before any socket is opened; without it the candidate's allocations
  would run past the budget the plan loop refused.
- `proofs/tla/MCRNClientTransportOrderBrokenNoFallback.cfg` violates
  `NoExhaustWhileReachable`. `Broken = {"nofallback"}` falls to the next
  candidate on an IPv6 refusal. TLC's trace: a token naming both families,
  the IPv6 socket refused and the IPv4 socket granted on both of two
  candidates; the client tries IPv6 on candidate 1, then IPv6 on candidate 2,
  and the transport order exhausts with a reachable IPv4 server never dialed. This is
  the transport order that dials only the selected server on every candidate, the
  shape `test_client_family_fallback_ipv6_connect_refused_auto` reproduces
  on the fake client driver.
- `proofs/tla/MCRNClientTransportOrderBrokenOomFalls.cfg` violates `NoTryAfterOom`.
  `Broken = {"oomfalls"}` treats an IPv6 refusal for want of memory as an
  ordinary refusal. TLC's trace: a token naming both families, the IPv6
  socket on candidate 1 refused for memory; the client tries the IPv4 server
  on candidate 1 and is granted, so a create that ran out of memory carries
  on. `test_client_family_fallback_out_of_memory_is_fatal` is the unit case.
- `proofs/tla/MCRNClientTransportOrderBrokenIgnoresToken.cfg` violates `TriedInToken`.
  `Broken = {"ignorestoken"}` falls back to IPv4 for any refused IPv6 socket.
  TLC's trace: a token naming IPv6 alone, its socket on candidate 1 refused;
  the client tries an IPv4 server the token does not name.
  `test_client_family_fallback_both_refused_exhausts` checks that such a
  token exhausts instead.

Each trace was checked by hand against
`rocketnet_client_transport_create_impl`: the fallback runs only when
`both_families` holds and the refusal's status is not `OUT_OF_MEMORY`, on the
candidate the transport order is on, and the race opens its second socket only when
the grant is the IPv6 server.

The model bounds the list to three candidates; the code's lists are short
(the platform's transports), and every candidate is handled by the same step.
