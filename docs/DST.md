# Deterministic simulation testing

RocketNet's deterministic simulation tier drives clients and a server with a
seeded in-process wire. The seed controls loss, duplication, delay, jitter and
reordering; mocked monotonic time controls endpoint updates. A failure must
print the seed and a command that reproduces the same schedule.

The harness checks properties that also appear in the TLA+ models:

- accepted payload bytes equal the sent message bytes;
- reliable messages arrive exactly once and ordered streams arrive in order;
- unreliable messages arrive at most once;
- stale packets from an earlier connection cannot enter a reused slot;
- acknowledgements name authenticated opened packets;
- buffers, queues, reassembly slots and receipt rings remain within their
  configured bounds;
- an endpoint (the server or a client) that can make fair progress does not remain wedged.

Every simulated endpoint has explicit state: connection generation, local clock,
next frame time, restart state, sent-message identities and received-message
identities. Fault decisions are derived from the run seed and stable event
keys, so adding an unrelated client does not perturb another endpoint's schedule.
Trace hashes cover the seed, ordered fault decisions and observable endpoint
events.

Short deterministic cases belong in the ordinary functional tier. Larger seed
sweeps and multi-hour campaigns are planned hardening gates. They must use the
same invariants and replay format; a larger run does not weaken a failed short
case.
