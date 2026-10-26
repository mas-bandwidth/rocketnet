# Workflow and shell lint, 2026-10-26

Card `rn-lint-existing-workflows-and-shell.w1@1`, epoch 1.
Staged head before this commit: `ef0229e87240cadf6e3b2376d2f72782d5fd7c97`.
Branch: `sprint/rn-lint-existing-workflows-and-shell.w1.g1.e1`.
Bench: hetzner2 (`/home/nova/johnny-jobs/rn-lint-existing-workflows-and-shell.w1~1`). Studio did not run the tools.

## Tools

- actionlint 1.7.12, linux/amd64, built with go1.26.1. Archive `actionlint_1.7.12_linux_amd64.tar.gz` sha256 `8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8`, matched `actionlint_1.7.12_checksums.txt` from the v1.7.12 release.
- ShellCheck 0.11.0, linux x86_64. Archive `shellcheck-v0.11.0.linux.x86_64.tar.xz` sha256 `8c3be12b05d5c177a04c29e3c78ce89ac86f1595681cab149b65b97c4e227198`, the GitHub release asset digest.
- Both binaries live in the task-local directory `.toolbin` of the job and were copied to hetzner2. Nothing was installed system-wide. `.github/workflows/static.yml` installs cmake, gcc, clang-tidy, cppcheck, libbpf, libxdp, and libsodium on a github-hosted runner only. It does not install these two tools.

## What was parsed

actionlint over all 20 `.github/workflows/*.yml`. Each file exited 1. ShellCheck over 31 shell scripts: every `*.sh` under `scripts/` and `tools/`, plus every other file there whose shebang is bash, sh, dash, ksh, or zsh. The first bench pass missed the `#!/usr/bin/env bash` scripts because it required `/bash`; the second pass, recorded here, includes all 31.

Raw logs: `/home/nova/johnny-jobs/lint-ef0229e8/` on hetzner2. This file is the per-finding record.

## Fixed in this commit

`scripts/live-xdp-fleet` assigned five locals and never read them. Removing the assignments does not change what the script runs. ShellCheck SC2034 on the edited file is gone.

- `public_port=40000` (was line 40)
- `server_shape`, `server_site`, `server_path`, `server_cap` (inits and the server-row stores)

`server_label`, `server_ssh`, `server_nic`, `server_address`, and `server_queues` stay. They are read.

## Not a defect: actionlint runner-label (72)

Every one of these is actionlint rule `runner-label`. The token is a custom self-hosted label (`fleet`, `long`, `libxdp-1-4`, `rocketnet-macos`, `rocketnet-quiet`, `rocketnet-windows`). actionlint's own text says to name custom labels in `actionlint.yaml`. That file is outside PATHS. `scripts/check-fleet-runners` (read, not a shell script) refuses GitHub-hosted labels (`ubuntu-*`, `macos-*`, `windows-*`) and allows the fleet labels. Renaming a label would change runner routing, which this card forbids. `self-hosted`, `Linux`, and `X64` were not reported. Counted from the run: fleet 40, rocketnet-windows 12, rocketnet-macos 10, long 7, rocketnet-quiet 2, libxdp-1-4 1.

- `.github/workflows/cbmc.yml:29:40`
- `.github/workflows/ci.yml:629:40`
- `.github/workflows/ci.yml:629:47`
- `.github/workflows/ci.yml:765:42`
- `.github/workflows/ci.yml:1086:40`
- `.github/workflows/ci.yml:1159:40`
- `.github/workflows/ci.yml:1274:40`
- `.github/workflows/ci.yml:1409:40`
- `.github/workflows/ci.yml:1573:40`
- `.github/workflows/ci.yml:1688:40`
- `.github/workflows/ci.yml:1759:40`
- `.github/workflows/ci.yml:1826:40`
- `.github/workflows/ci.yml:2056:40`
- `.github/workflows/ci.yml:2108:40`
- `.github/workflows/ci.yml:2165:40`
- `.github/workflows/docs.yml:31:40`
- `.github/workflows/fuzz-nightly.yml:42:40`
- `.github/workflows/fuzz-nightly.yml:191:40`
- `.github/workflows/fuzz-nightly.yml:276:42`
- `.github/workflows/fuzz-nightly.yml:425:42`
- `.github/workflows/fuzz-nightly.yml:585:40`
- `.github/workflows/fuzz-nightly.yml:766:40`
- `.github/workflows/fuzz-nightly.yml:797:42`
- `.github/workflows/msan.yml:33:40`
- `.github/workflows/nightly-bigendian-system.yml:44:40`
- `.github/workflows/nightly-bigendian-system.yml:44:47`
- `.github/workflows/nightly-channel-seeds.yml:22:46`
- `.github/workflows/nightly-channel-seeds.yml:22:55`
- `.github/workflows/nightly-channels-stress.yml:22:46`
- `.github/workflows/nightly-channels-stress.yml:44:48`
- `.github/workflows/nightly-channels-stress.yml:66:42`
- `.github/workflows/nightly-dst.yml:41:40`
- `.github/workflows/nightly-dst.yml:98:42`
- `.github/workflows/nightly-dst.yml:155:42`
- `.github/workflows/nightly-examples.yml:73:42`
- `.github/workflows/nightly-examples.yml:160:42`
- `.github/workflows/nightly-frame-budget.yml:36:40`
- `.github/workflows/nightly-frame-budget.yml:64:42`
- `.github/workflows/nightly-frame-budget.yml:120:42`
- `.github/workflows/nightly-leaks.yml:29:40`
- `.github/workflows/nightly-macos-sanitizers.yml:54:42`
- `.github/workflows/nightly-max-clients.yml:36:40`
- `.github/workflows/nightly-max-clients.yml:36:47`
- `.github/workflows/nightly-proofs.yml:22:40`
- `.github/workflows/nightly-proofs.yml:22:47`
- `.github/workflows/nightly-soak.yml:30:40`
- `.github/workflows/nightly-soak.yml:30:47`
- `.github/workflows/nightly-soak.yml:239:40`
- `.github/workflows/nightly-soak.yml:239:47`
- `.github/workflows/nightly-soak.yml:289:40`
- `.github/workflows/nightly-soak.yml:289:47`
- `.github/workflows/nightly-soak.yml:347:42`
- `.github/workflows/nightly-soak.yml:463:42`
- `.github/workflows/nightly-soak.yml:627:40`
- `.github/workflows/nightly-soak.yml:712:40`
- `.github/workflows/nightly.yml:90:40`
- `.github/workflows/nightly.yml:133:42`
- `.github/workflows/nightly.yml:168:40`
- `.github/workflows/nightly.yml:239:40`
- `.github/workflows/nightly.yml:287:42`
- `.github/workflows/nightly.yml:332:42`
- `.github/workflows/nightly.yml:385:42`
- `.github/workflows/nightly.yml:498:42`
- `.github/workflows/nightly.yml:532:42`
- `.github/workflows/nightly.yml:616:42`
- `.github/workflows/release-check.yml:69:40`
- `.github/workflows/release-check.yml:158:40`
- `.github/workflows/release-check.yml:246:40`
- `.github/workflows/release-check.yml:282:40`
- `.github/workflows/static.yml:55:40`
- `.github/workflows/static.yml:90:40`
- `.github/workflows/tlc.yml:25:40`

## Not a defect: actionlint if-cond (1)

`.github/workflows/nightly-examples.yml:159` is `if: false` on job `examples-windows`. The line above is `# DISABLED: no Windows fleet runner registered for this repository`. `scripts/check-fleet-runners` documents `DISABLED` as a job that carries `if: false` and a `# DISABLED:` comment. Deleting the `if:` would enable the job. That changes workflow meaning. Left in place.

- `.github/workflows/nightly-examples.yml:159:9: constant expression "false" in condition. remove the if: section [if-cond]`

## Not a defect: ShellCheck, suggested edit would change the script or misreads a trap

Each line below was left as it is. The rationale is the rule, checked against the source the line names.

SC2016: the single quotes are the payload (a remote `bash -c` script, a `printf` of a shell program, a `case` arm matching the literal `$(CC)` / `$(MAKE)`, or a command string echoed for a guest). Double quotes would expand it on this side.
SC2030 and SC2031: `scripts/rocketnet-provision` puts `PATH`, `VERIFY`, `INTERFACE`, `NUM_QUEUES`, and `UNMET` inside `( )` so the change stays in that probe. The loss shellcheck names is the isolation.
SC2329: `scripts/netem-pair-soak:186` `cleanup` is `trap cleanup EXIT` at line 215. `scripts/soak-multiday:123` `stop` is `trap stop INT TERM` at line 127. ShellCheck does not count the trap as a call.
SC2013 `scripts/channel-seed-sweep:54`: the `for` loop reads function names matched by `^[A-Za-z0-9_]+`, which do not contain spaces. A while-read rewrite is not a defect fix.
SC2012 `tools/fuzz_coverage.sh:71`: `ls` of three `sdk/rocketnet_*.cpp` globs. Those names are repository filenames. `find` is a different command, not a correction of a broken one.
SC2009 `tools/run_xdp_filter_guest.sh:273`: the `ps | grep` pipeline excludes `grep` and `$NAME`. `pgrep` would not apply that same filter. Left as written.
SC2001 `scripts/soak-multiday:158`: `sed 's/^0*//'` strips leading zeros from a hex string. The parameter-expansion form is a different spelling of the same strip, not a defect.

### SC2016

- `scripts/channel-seed-sweep:94:35: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof:332:45: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-remote:159:46: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:88:27: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:88:54: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:89:27: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:89:77: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:90:27: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:91:5: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:92:5: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:95:5: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:97:5: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:101:27: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:319:9: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:321:9: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/lan-proof-selftest:322:13: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/soak-per-transport:210:14: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/soak-per-transport:211:14: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/transport-cells-repeat:256:74: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/transport-cells-repeat:256:103: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `scripts/transport-cells-repeat:256:143: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `tools/check_fp_contract.sh:25:48: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `tools/check_fp_contract.sh:25:56: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `tools/check_fp_contract.sh:33:44: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `tools/check_fp_contract.sh:33:58: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `tools/check_make_jobs.sh:19:14: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`
- `tools/check_make_jobs.sh:19:26: note: Expressions don't expand in single quotes, use double quotes for that. [SC2016]`

### SC2030

- `scripts/rocketnet-provision:668:49: note: Modification of PATH is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:674:66: note: Modification of PATH is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:787:63: note: Modification of PATH is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:817:64: note: Modification of PATH is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:825:48: note: Modification of PATH is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:830:48: note: Modification of NUM_QUEUES is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:830:62: note: Modification of PATH is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:962:52: note: Modification of PATH is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:968:15: note: Modification of UNMET is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:968:24: note: Modification of VERIFY is local (to subshell caused by (..) group). [SC2030]`
- `scripts/rocketnet-provision:968:34: note: Modification of INTERFACE is local (to subshell caused by (..) group). [SC2030]`

### SC2031

- `scripts/rocketnet-provision:674:82: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:787:79: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:795:79: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:825:64: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:830:78: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:874:34: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:894:35: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:909:36: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:931:37: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:950:36: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:962:68: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:968:64: note: PATH was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1054:10: note: UNMET was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1060:10: note: VERIFY was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1123:35: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1129:25: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1130:33: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1132:24: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1134:25: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1134:64: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1137:21: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1138:7: note: VERIFY was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1171:38: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1173:66: note: NUM_QUEUES was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1174:55: note: NUM_QUEUES was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1180:75: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1182:22: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1186:30: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1193:14: note: VERIFY was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1229:28: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1230:27: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1235:12: note: VERIFY was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1248:12: note: VERIFY was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1268:18: note: VERIFY was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1285:6: note: VERIFY was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1286:10: note: UNMET was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1287:168: note: INTERFACE was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1287:179: note: NUM_QUEUES was modified in a subshell. That change might be lost. [SC2031]`
- `scripts/rocketnet-provision:1301:6: note: UNMET was modified in a subshell. That change might be lost. [SC2031]`

### SC2329

- `scripts/netem-pair-soak:186:1: note: This function is never invoked. Check usage (or ignored if invoked indirectly). [SC2329]`
- `scripts/soak-multiday:123:1: note: This function is never invoked. Check usage (or ignored if invoked indirectly). [SC2329]`

### SC2013

- `scripts/channel-seed-sweep:54:30: note: To read lines rather than words, pipe/redirect to a 'while read' loop. [SC2013]`

### SC2012

- `tools/fuzz_coverage.sh:71:28: note: Use find instead of ls to better handle non-alphanumeric filenames. [SC2012]`

### SC2009

- `tools/run_xdp_filter_guest.sh:273:24: note: Consider using pgrep instead of grepping ps output. [SC2009]`

### SC2001

- `scripts/soak-multiday:158:12: note: See if you can use ${variable//search/replace} instead. [SC2001]`

## Unresolved, so this card is HOLD

These warnings were not rewritten. The rewrite shellcheck offers would change shell behavior, and I do not have evidence that the warning is a false positive.

SC2088 (18): a quoted `~/...` is passed into `ssh` as an argument (`scripts/lan-proof` `ssh_run`, `scripts/live-xdp-fleet`). Tilde does not expand locally, and it does not expand on the far side once it is inside a variable. Replacing it with `$HOME` would bake in this machine's home. That is a behavior change. Not done.
SC2086 (17): the unquoted values are word lists or single tokens whose quoting would change the argument vector (`client_ifaces`, `client_sshs`, `shard_hosts`, `client_labels`, `$DEFINES`, `$LIBS`, `${NETCODE_OBJ:-}`, and the `rocketnet-provision` probes). Not quoted.
SC2015 (3): `A && B || C` in `scripts/lan-proof-remote:142` and `scripts/lan-proof-selftest:287` and `:321`. C can run when A is true. I did not prove that is impossible here, and rewriting it changes control flow.

### SC2088

- `scripts/lan-proof:255:14: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:256:13: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:291:46: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:294:50: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:485:55: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:544:53: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:561:50: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:574:47: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/lan-proof:578:45: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:287:14: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:288:17: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:289:16: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:296:11: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:328:14: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:456:14: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:463:70: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:473:18: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`
- `scripts/live-xdp-fleet:490:14: warning: Tilde does not expand in quotes. Use $HOME. [SC2088]`

### SC2086

- `scripts/lan-proof:421:12: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/lan-proof:519:12: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/lan-proof:521:34: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/lan-proof:595:65: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/lan-proof-selftest:267:46: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:334:26: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:404:16: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:1030:99: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:1130:33: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:1192:34: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:1233:17: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:1236:45: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:1246:17: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `scripts/rocketnet-provision:1249:42: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `tools/differential/run.sh:79:14: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `tools/docs/check_agents_md.sh:84:29: note: Double quote to prevent globbing and word splitting. [SC2086]`
- `tools/docs/check_agents_md.sh:84:66: note: Double quote to prevent globbing and word splitting. [SC2086]`

### SC2015

- `scripts/lan-proof-remote:142:27: note: Note that A && B || C is not if-then-else. C may run when A is true. [SC2015]`
- `scripts/lan-proof-selftest:287:70: note: Note that A && B || C is not if-then-else. C may run when A is true. [SC2015]`
- `scripts/lan-proof-selftest:321:70: note: Note that A && B || C is not if-then-else. C may run when A is true. [SC2015]`

No workflow job, permission, runner label, or release rule was edited. No blanket ShellCheck disable was added. Compiler and static-analyzer warnings were not part of this run.

Not done: the three unresolved classes above. No claim that actionlint or ShellCheck exits 0.
