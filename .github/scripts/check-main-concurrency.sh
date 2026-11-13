#!/bin/sh
# check-main-concurrency.sh: refuse a per-commit workflow whose concurrency group
# is one group per ref and does not separate main commits.
#
# GitHub keeps at most one running and one pending run per concurrency group and
# cancels the older pending run whenever a newer one queues, whatever
# cancel-in-progress says. A burst of landings on main therefore replaces the
# pending run of every commit but the last, leaving only the newest commit with
# a verdict. Fix: on refs/heads/main each commit gets its own group -- put
# github.sha in the group when the ref is refs/heads/main, and keep github.ref
# everywhere else, so pull requests and branches still cancel a superseded run.
#
# This refuses a per-commit workflow (one triggered by push or pull_request)
# whose top-level concurrency group references github.ref but does not select
# github.sha on main (the form `github.ref == 'refs/heads/main' && github.sha`, or
# a sha in no condition), i.e. one group per ref that would replace a main commit's
# pending run instead of queuing it. A sha token alone is not enough: the inverted
# `github.ref != 'refs/heads/main' && github.sha || github.ref` gives main one group.
# That includes a ref-only group such as "records-${{ github.ref }}":
# a group serializes a shared write only when the workflow writes, and none of
# these does.
#
# Second rule (2026-11-12, VERIFY-2 C1): one event kind never cancels or replaces
# another. In a workflow triggered by `schedule` or `workflow_call` (the nightlies,
# and ci.yml, which nightly-ci.yml calls on a schedule), EVERY concurrency group,
# top-level or job-level, must contain github.event_name or github.run_id. A
# group keyed by ref alone puts a release-gate dispatch and the scheduled night in
# one group, and cancel-in-progress (or GitHub's replacement of a pending run)
# then kills one with the other: nightly.yml's `linux / mutation` never finished
# on a schedule because each dispatch cancelled it. Same-kind runs may still
# cancel each other.
#
# usage: check-main-concurrency.sh [workflow files...]   (default: .github/workflows/*.yml *.yaml)
# exit 0 when every per-commit group separates main commits and every scheduled
# workflow's groups separate event kinds, 1 with one line per refusal otherwise.
# Self-test: check-main-concurrency.sh --self-test
set -u
self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

check() {
  awk '
    function flush() {
      if (percommit && badv != "") {
        printf "%s:%d: concurrency group %s does not separate main commits\n", file, badline, badv
        bad = 1
      }
      if (sched) {
        for (i = 1; i <= nkind; i++) {
          printf "%s:%d: concurrency group %s lets one event kind cancel another (needs github.event_name or github.run_id)\n", file, kindline[i], kindv[i]
          bad = 1
        }
      }
      percommit = 0; sched = 0; nkind = 0; badv = ""; inon = 0; incc = 0
    }
    FNR == 1 { if (NR > 1) flush(); file = FILENAME }
    # every group line, top-level or job-level, for the event-kind rule
    /^[ \t]*group:/ {
      g = $0; sub(/^[ \t]*group:[ \t]*/, "", g)
      if (g !~ /github[.]event_name/ && g !~ /github[.]run_id/) { nkind++; kindv[nkind] = g; kindline[nkind] = FNR }
    }
    /^on:/ {
      inon = 1; incc = 0
      if ($0 ~ /(push|pull_request)/) percommit = 1
      if ($0 ~ /(schedule|workflow_call)/) sched = 1
      next
    }
    inon && /^  (push|pull_request|pull_request_target):/ { percommit = 1 }
    inon && /^  (schedule|workflow_call):/ { sched = 1 }
    inon && /^[a-zA-Z_-]+:/ { inon = 0 }
    /^concurrency:/ { incc = 1; next }
    incc && /^[ \t]*#/ { next }
    incc && /^  group:/ {
      v = $0; sub(/^  group:[ \t]*/, "", v)
      # main must select the sha: the literal `github.ref == refs/heads/main && github.sha` form, or a
      # sha (or run id) in no condition at all; a github.sha token behind any other condition (the
      # inverted `github.ref != refs/heads/main && github.sha || github.ref`) gives main one group
      sel = (v ~ /github[.]ref[ \t]*==[ \t]*.refs\/heads\/main.[ \t]*&&[ \t]*github[.](sha|run_id)/)
      uncond = (v ~ /github[.](sha|run_id)/ && v !~ /(&&|[|][|]|!=|==)/)
      if (v ~ /github[.]ref/ && !sel && !uncond) { badv = v; badline = FNR }
      incc = 0
      next
    }
    incc && /^[ \t]*[a-zA-Z_-]+:/ { incc = 0 }
    END { flush(); exit bad }
  ' "$@"
}

if [ "${1:-}" = "--self-test" ]; then
  d=$(mktemp -d) || exit 1
  trap 'rm -rf "$d"' EXIT
  # failing fixture: one group per ref (github.workflow + github.ref, no github.sha)
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: foo-${{ github.workflow }}-${{ github.ref }}\n  cancel-in-progress: ${{ github.ref != '"'"'refs/heads/main'"'"' }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: a one-group-per-ref workflow was accepted"; exit 1; fi
  # failing fixture: a ref-only group (no github.workflow) on push to main, like tlc.yml had
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: records-${{ github.ref }}\n  cancel-in-progress: ${{ github.ref != '"'"'refs/heads/main'"'"' }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad-ref-only.yml"
  if check "$d/bad-ref-only.yml" >/dev/null; then echo "self-test: a ref-only group on push to main was accepted"; exit 1; fi
  # failing fixture: concurrency before on:, pull_request trigger
  printf 'name: x\nconcurrency:\n  group: pr-${{ github.ref }}\non:\n  pull_request:\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad-pr-first.yml"
  if check "$d/bad-pr-first.yml" >/dev/null; then echo "self-test: a ref-only group declared before on: was accepted"; exit 1; fi
  # failing fixture: the inverted form names github.sha but gives main one group (github.ref)
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: ci-${{ github.ref != '"'"'refs/heads/main'"'"' && github.sha || github.ref }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad-inverted.yml"
  if check "$d/bad-inverted.yml" >/dev/null; then echo "self-test: an inverted group (sha off main, ref on main) was accepted"; exit 1; fi
  # passing fixture: a sha in no condition separates every commit everywhere
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: x-${{ github.ref }}-${{ github.sha }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-sha.yml"
  check "$d/good-sha.yml" >/dev/null || { echo "self-test: an unconditional sha group was refused"; exit 1; }
  # passing fixtures: separated main group, a schedule-only ref group, and no concurrency
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: foo-${{ github.workflow }}-${{ github.ref == '"'"'refs/heads/main'"'"' && github.sha || github.ref }}\n  cancel-in-progress: ${{ github.ref != '"'"'refs/heads/main'"'"' }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-sep.yml"
  # failing fixtures (event-kind rule): a scheduled workflow whose top-level group, or a job-level group, is
  # keyed by ref only (nightly.yml's mutation job before 2026-11-12); a called workflow (ci.yml) the same
  printf 'name: x\non:\n  schedule:\n    - cron: "30 5 * * *"\n  workflow_dispatch:\nconcurrency:\n  group: nightly-${{ github.ref }}\n  cancel-in-progress: true\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad-nightly-top.yml"
  if check "$d/bad-nightly-top.yml" >/dev/null; then echo "self-test: a scheduled workflow grouped by ref only was accepted"; exit 1; fi
  printf 'name: x\non:\n  schedule:\n    - cron: "40 8 * * *"\n  workflow_dispatch:\njobs:\n  mutation:\n    concurrency:\n      group: nightly-mutation-${{ matrix.ref }}\n      cancel-in-progress: true\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad-nightly-job.yml"
  if check "$d/bad-nightly-job.yml" >/dev/null; then echo "self-test: a scheduled job grouped by ref only was accepted"; exit 1; fi
  printf 'name: x\non:\n  workflow_call:\n  push:\n    branches: [main]\nconcurrency:\n  group: c-${{ github.workflow }}-${{ github.ref == '"'"'refs/heads/main'"'"' && github.sha || github.ref }}\n  cancel-in-progress: false\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad-called.yml"
  if check "$d/bad-called.yml" >/dev/null; then echo "self-test: a called workflow grouped without the event kind was accepted"; exit 1; fi
  printf 'name: x\non:\n  schedule:\n    - cron: "0 6 * * 6"\n  workflow_dispatch:\nconcurrency:\n  group: debt\n  cancel-in-progress: false\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad-fixed-name.yml"
  if check "$d/bad-fixed-name.yml" >/dev/null; then echo "self-test: a scheduled workflow with one fixed group was accepted"; exit 1; fi
  # passing: groups keyed by event kind (or by run id) in a scheduled workflow
  printf 'name: x\non:\n  schedule:\n    - cron: "30 5 * * *"\n  workflow_dispatch:\nconcurrency:\n  group: nightly-${{ github.event_name }}-${{ github.ref }}\n  cancel-in-progress: true\njobs:\n  a:\n    concurrency:\n      group: nightly-a-${{ github.event_name }}-${{ matrix.ref }}\n      cancel-in-progress: true\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n  b:\n    concurrency:\n      group: nightly-b-${{ github.event_name == '"'"'workflow_dispatch'"'"' && github.run_id || github.event_name }}\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-nightly.yml"
  printf 'name: x\non:\n  push:\n    branches: [main]\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-none.yml"
  check "$d/good-sep.yml" "$d/good-nightly.yml" "$d/good-none.yml" >/dev/null || { echo "self-test: a separating, nightly or no-concurrency workflow was refused"; check "$d/good-sep.yml" "$d/good-nightly.yml" "$d/good-none.yml"; exit 1; }
  # zero inputs: run from a directory with no workflows; must be red, not "ok"
  mkdir "$d/empty"
  if (cd "$d/empty" && sh "$self") >/dev/null; then echo "self-test: zero workflow files was accepted"; exit 1; fi
  echo "check-main-concurrency self-test: ok"
  exit 0
fi

if [ $# -eq 0 ]; then
  for f in .github/workflows/*.yml .github/workflows/*.yaml; do
    if [ -e "$f" ]; then set -- "$@" "$f"; fi
  done
  [ $# -gt 0 ] || { echo "check-main-concurrency: no workflows (0 files): nothing was checked"; exit 1; }
fi
if check "$@"; then echo "check-main-concurrency: every per-commit concurrency group separates main commits and every scheduled workflow's groups separate event kinds ($# workflow files)"; exit 0; fi
echo "check-main-concurrency: a main commit's pending run would be replaced, or one event kind would cancel another"
exit 1
