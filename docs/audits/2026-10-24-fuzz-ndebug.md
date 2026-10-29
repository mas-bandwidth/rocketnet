# Release-build fuzz campaign, 2026-10-24

Release-build fuzz campaign of `mas-bandwidth/rocketnet` at `6ac80bfb59eb3b9e3e6f748c1973c6b6c8f0b228`
(`main`). Scope: the library under `sdk/` and the eleven `tests/fuzz/` harnesses, all built with `-DNDEBUG`,
so `rocketnet_assert` compiles to nothing (`sdk/rocketnet_internal.h`) and every bound that only an
assert holds is fuzzed the way a release build runs it. Method: `tools/fuzz_ndebug.sh --seconds 600 --jobs 6` (the nightly's sanitizer flags, AddressSanitizer and UndefinedBehaviorSanitizer, on Linux),
each target for 600 seconds from its seed corpus; any crash, leak, timeout or sanitizer report would
have been minimised with `-minimize_crash=1`, deduplicated by its top three frames and replayed against
this build and the Debug fuzz build. Every target ended with a clean `Done N runs` line, the script
exited 0, the crash directory held no files and no log carried a sanitizer report. An earlier run of
the same campaign at `8781c5985368d309507481d805454560a1e73447` was also clean; the only library change
between that commit and the one audited is `sdk/rocketnet_base64.cpp` (two length checks), and the
whole campaign was run again at the audited commit, which is the table below. Library changes that
land on `main` after `6ac80bfb` are outside this campaign.

Corpus entries, features and coverage edges are the `corp`, `ft` and `cov` numbers on each target's
final line.

| target | runs | seconds | corpus | features | coverage | crashes |
|---|---:|---:|---:|---:|---:|---:|
| rocketnet_fuzz | 80379225 | 601 | 346 | 1087 | 313 | 0 |
| rocketnet_fuzz_connect_token | 2704686 | 601 | 80 | 531 | 333 | 0 |
| rocketnet_fuzz_receive_structured | 284437 | 601 | 820 | 6829 | 1255 | 0 |
| rocketnet_fuzz_message | 78974896 | 601 | 167 | 1430 | 292 | 0 |
| rocketnet_fuzz_token_private | 500644 | 601 | 70 | 1008 | 652 | 0 |
| rocketnet_fuzz_round_trip | 60020776 | 601 | 258 | 836 | 454 | 0 |
| rocketnet_fuzz_config | 38865216 | 601 | 224 | 732 | 285 | 0 |
| rocketnet_fuzz_server_transport | 212861 | 601 | 645 | 11636 | 2809 | 0 |
| rocketnet_fuzz_client_transport | 187012 | 601 | 614 | 11076 | 2742 | 0 |
| rocketnet_fuzz_session | 16763 | 601 | 1241 | 20607 | 3923 | 0 |
| rocketnet_fuzz_differential | 196198 | 601 | 118 | 2256 | 938 | 0 |

## Findings

None. No target produced a crash, leak, timeout or sanitizer report, so there is no reproducer to
minimise and nothing to replay against the Debug fuzz build.

## Counts

- bug: 0
- risk: 0
- quality: 0
- total: 0
