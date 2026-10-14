# BSD legs: the supply-chain read

The nightly BSD rows stay unbuilt. No workflow file is added.

Issue #124 keeps FreeBSD, OpenBSD and NetBSD on the vmactions family only if the supply-chain evaluation noted on #81 passes on its own terms. A third-party action that boots a VM and runs the build inside it is a large amount of trust to hand a repository. #81 states the terms as: pin the action by commit, read the source at that pin, and accept that the action runs inside the runner and sees the checkout, but that it is given no secret it is not handed. The same bar covers all three systems. If the read fails, the rows stay unbuilt. This file is that read. It does not boot a guest and it does not run the suite.

## Criteria

An option passes only when every item below is true of the program a nightly job would execute.

1. **The runner program is pinned.** Every action is a full commit SHA. A floating tag does not count.
2. **The pin covers what is downloaded.** Every file the runner executes or boots, beyond that pinned tree and the Ubuntu archive packages the existing ubuntu legs already install, is either inside the pinned tree or fetched from a URL the pinned tree writes in full (org, repository, tag, filename) and checked against a digest the pinned tree also fixes. A digest fetched from the same party that serves the bytes does not count. A GitHub release whose API object reports `immutable` false does not count: the asset at that URL is not a fixed pin. A release that reports `immutable` true counts only together with a URL the pinned tree writes in full, not a search of a release list.
3. **The guest is not given a credential.** The job sets `permissions: contents: read`, sets no secrets, and the code that runs does not forward `GITHUB_TOKEN` or a `GITHUB_*` pattern that carries the workflow token into the guest. Source that appends `SendEnv` of `GITHUB_*` fails this. The action sees the checkout. That is the access #81 already accepts. A bearer token is more than the checkout. A caller emptying the variable first is not a property of the action.
4. **The job needs no write and no identity token** to find its image or to finish.
5. **The suite runs on that BSD kernel.** `./bin/test` inside the guest is the row. qemu-user of a Linux binary is not that row.

## Decision, one line each

| option | decision |
|---|---|
| `vmactions/freebsd-vm` at `c46abacb49f09938ca4e1702d15d836285d694cc` (tag `v1.5.9`) | does not pass (criteria 2 and 3) |
| `vmactions/openbsd-vm` at `5f7b2c933b8846a138361d6843c52af4bef5d945` (tag `v1.4.8`) | does not pass (criteria 2 and 3) |
| `vmactions/netbsd-vm` at `c8a0d7ddb84619a7a8cc4e652efa7950b3fd9f92` (tag `v1.5.2`) | does not pass (criteria 2 and 3) |
| `cross-platform-actions/action` at `e0b9770014ba65d5e0815f15b74031c3a635f641` (annotated tag `v1.6.0` is `48a7982e8230b812b030848825c2cb55a66daa4e`) | does not pass (criterion 3; criterion 2 holds only by the immutable-release bit) |
| the in-repository qemu leg (`ci.yml`, the big-endian job) | does not pass (criterion 5; it is qemu-user, not a BSD kernel) |

## Sources

Dates below are the release or commit timestamps the API and git report. The read is of these commits, not of floating branch tips.

Maintainer records from the repository API:

- `vmactions/freebsd-vm`, organization `vmactions`, created 2020-10-01, MIT, default branch `main`. 91 releases from `v0.0.1` (2020-10-01) through `v1.5.9` (2026-10-03).
- `vmactions/openbsd-vm`, same organization, created 2020-10-02. 61 releases from `v0.0.1` (2022-07-07) through `v1.4.8` (2026-09-26).
- `vmactions/netbsd-vm`, same organization, created 2020-12-25. 63 releases from `v0.0.1` (2022-07-11) through `v1.5.2` (2026-09-26).
- `cross-platform-actions/action`, organization `cross-platform-actions`, created 2021-04-16, MIT, default branch `master`. 44 releases from `v0.0.2` (2021-06-22) through `v1.6.0` (2026-09-12). The `v1.6.0` release author login is `github-actions[bot]`.
- `anyvm-org/anyvm`, organization `anyvm-org`, created 2025-11-22. The vmactions readme says the images are built and booted by that project.

The three vmactions tags are commit objects. The cross-platform-actions tag is an annotated tag; the commit it points at is the pin that matters.

## vmactions, all three

The three actions are one generator. Each commit message is "Update from base-vm". `action.yml` runs `node24` on `index.js`. Each can be pinned by the full SHA above, so criterion 1 holds, and the usual `@v1` tag is the floating form this file refuses. Criterion 4 holds in the narrow sense that the action does not ask for a write token. The rest does not.

**What the pin does not cover.** At the freebsd pin, `conf/14.4.conf` sets `BUILDER_VERSION=2.2.8` and `ANYVM_VERSION=0.7.2`. `conf/default.release.conf` sets `DEFAULT_RELEASE=15.1`. A search of that commit's `conf/` shows the same two version lines on the other release files. The openbsd pin's `conf/7.9.conf` sets `BUILDER_VERSION=2.1.0` and `ANYVM_VERSION=0.7.2`. The netbsd pin's `conf/11.0.conf` sets `BUILDER_VERSION=2.2.7` and `ANYVM_VERSION=0.7.2`. The `v1.4.8` release notes list further openbsd builder tags by release (`0.9.9`, `1.1.2`, `1.1.4`, `2.1.0`). None of those conf files carries a digest.

`index.js` then downloads the runtime. The freebsd pin does it at the lines that build `https://github.com/anyvm-org/anyvm/releases/download/v${anyvmVersion}/anyvm.py` and call `downloadFile` with no digest check. `downloadFile` follows redirects and checks only the HTTP status. The openbsd and netbsd pins have the same URL string (line 1017 in each) and the same `downloadFile` body. The downloaded file is what the action executes (`python3` on that path).

The release API for `anyvm-org/anyvm` tag `v0.7.2` (published 2026-09-25) reports `immutable` false. Its `anyvm.py` asset reports `digest` `sha256:40a16e846bc07030759936d792343e770acdeb14d4c9991af7409648661a650d`. The bytes fetched from that URL during this read hash to the same value. That digest is a snapshot of a release the API does not freeze. The action does not check it. A later upload to the same tag name changes what the pinned action executes.

**The disk image.** The fetched `anyvm.py` (those bytes) resolves the image by walking the builder release asset list and matching a filename suffix (`freebsd-14.4.qcow2.zst` and the arch form). The download call on that URL does not hash the file. Digest checks in that file cover a pinned aarch64 firmware blob and an `nfsd.py` sidecar whose expected digest is fetched from the same release as the file. They do not cover the disk. The builder releases the conf files name also report `immutable` false:

- `anyvm-org/freebsd-builder` `v2.2.8` (2026-09-15), including `freebsd-14.4.qcow2.zst` and `freebsd-14.4-aarch64.qcow2.zst`
- `anyvm-org/openbsd-builder` `v2.1.0` (2026-08-22), including `openbsd-7.9.qcow2.zst` and `openbsd-7.9-aarch64.qcow2.zst`
- `anyvm-org/netbsd-builder` `v2.2.7` (2026-09-26), including `netbsd-11.0.qcow2.zst` and `netbsd-11.0-aarch64.qcow2.zst`

The freebsd readme at the pin says the images start from the FreeBSD project's own VM images, then the builder boots them, enables ssh, pre-installs packages, and exports a new qcow2. The guest is that rebuilt image, not the upstream file. The same release publishes a host private key beside the image (`*-host.id_rsa` is a public asset). The guest login is not a secret of the image.

`release` input text says a leading version part selects the newest matching release. A fully spelled release still resolves to whatever asset currently wears that name.

Criterion 2 fails on all three.

**The token.** Each `action.yml` defaults the `token` input to `${{ github.token }}`, so a workflow that leaves the input unset still supplies the workflow token. The action passes that value to the runtime as `GITHUB_TOKEN` when the environment does not already hold one. It also appends `SendEnv GITHUB_*` and `SendEnv CI` to the runner ssh config, and it sets `StrictHostKeyChecking=no`. The freebsd readme at the pin states that all `GITHUB_*` variables and `CI=true` are passed into the VM, and that an `envs` input forwards named variables, which is how a secret would be handed across on purpose. This read does not boot a guest; the source and that readme are the evidence. The workflow token is a credential. Forwarding it fails criterion 3 on all three, on a private repository as much as on a public one. `permissions: contents: read` limits what the token can do. It does not stop the token being given to the guest.

## cross-platform-actions/action

The organization has published the action since 2021. `v1.6.0` is cut by `github-actions[bot]`. The action metadata names one author; this file does not repeat a personal name. `action.yml` runs `node24` on `dist/index.js` with a post step `dist/post.mjs`. The pin above is a full commit, so criterion 1 holds. The action has no token input and does not default one to `github.token`, so criterion 4 holds for the lookup itself.

**Images and the qemu archive.** `src/version.ts`, and the same table compiled into `dist/index.js`, fixes the builder tags: FreeBSD `v0.17.0`, OpenBSD `v0.14.0`, NetBSD `v1.1.0`, resources `v2.0.0`. The image URL is composed as `https://github.com/cross-platform-actions/<name>-builder/releases/download/<tag>/<name>-<os version>-<arch>.<ext>`. For the versions the #81 record names, the release API lists these assets, each on a release that reports `immutable` true:

- `freebsd-14.4-x86-64.qcow2`, digest `sha256:adb5ee006eb4661a59f836670d81c9e1baa6d325fadd9115757fdf7241407cab`
- `freebsd-15.1-x86-64.qcow2`, digest `sha256:34e4cd936a2484fd7d873a61dbd0b2b6bb31387897e4c7e63a8fdd5b9a72f805`
- `openbsd-7.9-x86-64.qcow2`, digest `sha256:ad05bb9cdf8fb8edf0514ff4c30b124bf0067d20be6d14c49ebf89bbbc55a9c0`
- `netbsd-11.0-x86-64.tar.zst`, digest `sha256:8562637319486eded26eb5eec17736999a473513cd2a56b12815a425711e3727`

The hypervisor URL is `https://github.com/cross-platform-actions/resources/releases/download/v2.0.0/qemu-system-x86_64-linux.tar` on a Linux runner (resources `v2.0.0` also reports `immutable` true). The action downloads a qemu build from that project, not the Ubuntu archive.

`downloadDiskImage` calls `downloadTool` with the URL and no digest. The action never computes the digest the release API reports. Criterion 2 therefore holds only while GitHub keeps the immutable bit on those four releases and only when the workflow does not set `image_url`. That input replaces the pinned URL with a caller-supplied one and skips any digest. A workflow that omits it stays on the pinned URL.

**The token.** `dist/index.js` writes ssh config that contains the line `SendEnv CI GITHUB_*` (`src/action/action.ts` is the same line). Host key checking is `accept-new`, which is stricter than `no` and still trusts the first key. NetBSD's `requiresSshKey` is false: those images accept an ssh login with no credential. Criterion 3 fails for the same reason as vmactions. The closer pin on the image does not repair a guest that is handed the workflow token.

## The in-repository qemu leg

`ci.yml` has one qemu job, the big-endian leg. It is not a self-hosted runner and it is not `qemu-system`. It cross-compiles for `s390x` and runs `qemu-s390x-static` from `qemu-user-static`, installed by apt from the Ubuntu archive, the same archive the job's own comment names. That comment says the leg downloads no third-party image and no tarball, and that it does not run `./bin/test`: a socket suite under qemu-user reports on the emulator rather than on the platform. There is no BSD kernel in that job. Criterion 5 fails.

There is no `qemu-system` workflow in this repository. A job that boots FreeBSD, OpenBSD or NetBSD downloads a disk image. That is a different supply chain from the big-endian leg, and this commit does not add one. An official project image whose digest is copied into the workflow from the checksum that project publishes, with no token forwarded into the guest, is the shape that meets criteria 1 through 4 without a third-party action. It is not adopted here. This read does not fetch those images, does not check those checksums, and does not boot a guest. A nightly file with no suite run does not meet criterion 5, and a green claim of a run this file does not have is not a result.

## What `origin/bsd-portability` still is

`origin/bsd-portability` is `c57d598ec30c1dec20a2ea9b0a2df5892b300339`, committed 2026-08-29, message "OpenBSD has no dual-stack sockets: the client binds the server's family (#81)". That commit is an ancestor of sprint tip `2507f11c383580f73241563fded80b35e2d2d4e6`. The count of commits on the sprint tip that are not in the branch is 357. The count the other way is 0. Merging the branch changes nothing. It is not a base to continue from, and this change does not merge it.

The portability work at the tip of that branch is already the code in this tree. It still applies, as current code rather than as a patch to replay:

- `sdk/rocketnet.h` defines `ROCKETNET_PLATFORM_BSD` for `__FreeBSD__`, `__OpenBSD__` and `__NetBSD__`. Linux-only netlink and wireless headers sit behind `__linux__` in `sdk/rocketnet_platform.cpp`.
- `Makefile` drives the inner build with `$(MAKE)`.
- `rocketnet_platform_mutex_create` sets `PTHREAD_MUTEX_NORMAL` by name.
- `rocketnet_platform_set_socket_buffer_size` walks the request down when the kernel refuses it.
- `rocketnet_platform_client_dual_stack` returns false on OpenBSD, the ipv6 socket does not clear `IPV6_V6ONLY` there, and `sdk/rocketnet_client_transport.cpp` binds the server's family when dual stack is off.
- `sdk/rocketnet_frame_sync.cpp` uses `nanosleep` on OpenBSD, where `clock_nanosleep` is absent, and `CLOCK_MONOTONIC` for that clock. `rocketnet_platform.cpp` maps a missing `CLOCK_MONOTONIC_RAW` to `CLOCK_MONOTONIC`.

What the branch does not contain, and what this tip still does not contain, is a CI leg. The supply-chain question the #81 record left open is this file.

## Recommendation

Do not add `.github/workflows/nightly-bsd.yml`. The three vmactions pins execute an unhashed runtime from a release the API reports `immutable` false, and that runtime boots an unhashed disk from a builder release with the same flag, while all three actions and `cross-platform-actions/action` at `e0b9770014ba65d5e0815f15b74031c3a635f641` configure ssh to forward `GITHUB_*` into the guest. The cross-platform-actions image URLs are the closest pin, because those builder releases report `immutable` true and the tags are written into the committed bundle, and the token forward still fails the term #81 sets. The existing qemu leg does not boot a BSD kernel. The rows stay unbuilt until a later change either removes the token forward and puts every downloaded byte under a digest fixed by the pin, or boots an official image whose digest lives in a workflow in this repository and shows a run of `./bin/test` inside that guest. Neither is this commit.

## Links

- https://github.com/vmactions/freebsd-vm/blob/c46abacb49f09938ca4e1702d15d836285d694cc/action.yml
- https://github.com/vmactions/freebsd-vm/blob/c46abacb49f09938ca4e1702d15d836285d694cc/index.js
- https://github.com/vmactions/freebsd-vm/blob/c46abacb49f09938ca4e1702d15d836285d694cc/conf/14.4.conf
- https://github.com/vmactions/freebsd-vm/releases/tag/v1.5.9
- https://github.com/vmactions/openbsd-vm/blob/5f7b2c933b8846a138361d6843c52af4bef5d945/action.yml
- https://github.com/vmactions/openbsd-vm/blob/5f7b2c933b8846a138361d6843c52af4bef5d945/conf/7.9.conf
- https://github.com/vmactions/openbsd-vm/releases/tag/v1.4.8
- https://github.com/vmactions/netbsd-vm/blob/c8a0d7ddb84619a7a8cc4e652efa7950b3fd9f92/conf/11.0.conf
- https://github.com/vmactions/netbsd-vm/releases/tag/v1.5.2
- https://github.com/anyvm-org/anyvm/releases/tag/v0.7.2
- https://github.com/anyvm-org/freebsd-builder/releases/tag/v2.2.8
- https://github.com/anyvm-org/openbsd-builder/releases/tag/v2.1.0
- https://github.com/anyvm-org/netbsd-builder/releases/tag/v2.2.7
- https://github.com/cross-platform-actions/action/blob/e0b9770014ba65d5e0815f15b74031c3a635f641/action.yml
- https://github.com/cross-platform-actions/action/blob/e0b9770014ba65d5e0815f15b74031c3a635f641/src/version.ts
- https://github.com/cross-platform-actions/action/releases/tag/v1.6.0
- https://github.com/cross-platform-actions/freebsd-builder/releases/tag/v0.17.0
- https://github.com/cross-platform-actions/openbsd-builder/releases/tag/v0.14.0
- https://github.com/cross-platform-actions/netbsd-builder/releases/tag/v1.1.0
- https://github.com/cross-platform-actions/resources/releases/tag/v2.0.0
