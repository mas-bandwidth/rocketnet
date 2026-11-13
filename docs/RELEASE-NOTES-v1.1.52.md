# RocketNet v1.1.52

Changes compared with the `v1.1.51` tag. A point release: no packet layout or
protocol version change, and v1.1.52 endpoints interoperate with every v1.x
endpoint. No counter row is added: the contract holds 290 counters and no
released id moved.

- The split server update's thread rules are checked. The window runs from
  `rocketnet_server_update_receive`'s return to
  `rocketnet_server_update_finish`'s entry. Inside it, `update_client` may run
  from any thread, along with the update calls; an update call finishes an
  open update first. Every other main thread call and every fill call in the
  window is refused and counted as `MAIN_THREAD_CALL_OVERLAP`, in every build.
  `update_finish` takes the main thread scope, and `update_receive` arms the
  check and takes its scope before it writes anything. The one-call
  `rocketnet_server_update` is unchanged.
- Development builds only: the refused-datagram sampler's index saturates
  instead of wrapping, so a flood of refused datagrams can no longer write
  past its array.
- Tests and tools:
  - the per-commit trace check's shards are sets of (layer, seed) pairs, so
    the heaviest layers' seeds are spread over shards. Every layer still runs
    every seed, and a pair in two shards or in none, or an empty shard, is
    red;
  - every per-commit CI job is held under the 120 s cap by more shards, and
    the compiler cache no longer depends on the checkout path;
  - `tools/replay_ci_steps.py` runs each step under the workflow's own shell,
    reports each step's seconds against the job's cap, and replays every row
    of a split job.

- Tag: `v1.1.52`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.52).
