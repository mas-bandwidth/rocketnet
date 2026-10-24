# NDEBUG fuzz campaign, 2026-10-24

The campaign is `tools/fuzz_ndebug.sh --seconds 600 --jobs 8` at `8781c5985368d309507481d805454560a1e73447`. The script is unchanged from that commit through `4c44c457ce1b147f33bd3974855ad2a3aa8558f2`. The library and the eleven harnesses were built with `-DNDEBUG`. Each target printed one clean `Done N runs` line. The crash directory had no files. No reproducer.

Corpus entries, features, and coverage edges are the `corp`, `ft`, and `cov` numbers on each target's final line.

| target | runs | seconds | corpus | features | coverage | reproducer |
|---|---:|---:|---:|---:|---:|---|
| rocketnet_fuzz | 145410892 | 601 | 346 | 1071 | 313 | none |
| rocketnet_fuzz_connect_token | 3659548 | 601 | 87 | 549 | 348 | none |
| rocketnet_fuzz_receive_structured | 389666 | 601 | 892 | 7029 | 1265 | none |
| rocketnet_fuzz_message | 101239047 | 601 | 171 | 1400 | 287 | none |
| rocketnet_fuzz_token_private | 732763 | 601 | 71 | 1046 | 673 | none |
| rocketnet_fuzz_round_trip | 57410667 | 601 | 144 | 651 | 410 | none |
| rocketnet_fuzz_config | 53265072 | 601 | 228 | 720 | 285 | none |
| rocketnet_fuzz_server_transport | 303914 | 601 | 642 | 11729 | 2866 | none |
| rocketnet_fuzz_client_transport | 248861 | 601 | 665 | 11889 | 2821 | none |
| rocketnet_fuzz_session | 23745 | 601 | 1319 | 20942 | 3969 | none |
| rocketnet_fuzz_differential | 259730 | 601 | 124 | 2296 | 956 | none |
