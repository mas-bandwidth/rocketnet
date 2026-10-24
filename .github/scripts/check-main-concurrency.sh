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
# This refuses a workflow whose top-level concurrency group references both
# github.workflow and github.ref (the per-commit gate pattern) but not
# github.sha, i.e. one group per ref that would replace a main commit's pending
# run instead of queuing it. A group with a fixed literal name (for example the
# TLC records gate's "tlc-records-${{ github.ref }}") serializes a shared write
# and is left alone: its runs must queue, not cancel.
#
# usage: check-main-concurrency.sh [workflow files...]   (default: .github/workflows/*.yml *.yaml)
# exit 0 when every per-commit group separates main commits, 1 with one line per refusal otherwise.
# Self-test: check-main-concurrency.sh --self-test
set -u

check() {
  awk '
    function report(v) {
      printf "%s:%d: concurrency group %s does not separate main commits\n", FILENAME, FNR, v
      bad = 1
    }
    /^concurrency:/ { incc = 1; next }
    incc && /^[ \t]*#/ { next }
    incc && /^  group:/ {
      v = $0; sub(/^  group:[ \t]*/, "", v)
      if (v !~ /github[.]sha/ && v ~ /github[.]workflow/ && v ~ /github[.]ref/) report(v)
      incc = 0
      next
    }
    incc && /^[ \t]*[a-zA-Z_-]+:/ { incc = 0 }
    END { exit bad }
  ' "$@"
}

if [ "${1:-}" = "--self-test" ]; then
  d=$(mktemp -d) || exit 1
  trap 'rm -rf "$d"' EXIT
  # failing fixture: one group per ref (github.workflow + github.ref, no github.sha)
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: foo-${{ github.workflow }}-${{ github.ref }}\n  cancel-in-progress: ${{ github.ref != '"'"'refs/heads/main'"'"' }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: a one-group-per-ref workflow was accepted"; exit 1; fi
  # passing fixtures: separated main group, fixed records group, and no concurrency
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: foo-${{ github.workflow }}-${{ github.ref == '"'"'refs/heads/main'"'"' && github.sha || github.ref }}\n  cancel-in-progress: ${{ github.ref != '"'"'refs/heads/main'"'"' }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-sep.yml"
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: records-${{ github.ref }}\n  cancel-in-progress: ${{ github.ref != '"'"'refs/heads/main'"'"' }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-records.yml"
  printf 'name: x\non:\n  push:\n    branches: [main]\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-none.yml"
  check "$d/good-sep.yml" "$d/good-records.yml" "$d/good-none.yml" >/dev/null || { echo "self-test: a separating, records or no-concurrency workflow was refused"; check "$d/good-sep.yml" "$d/good-records.yml" "$d/good-none.yml"; exit 1; }
  echo "check-main-concurrency self-test: ok"
  exit 0
fi

if [ $# -eq 0 ]; then
  set -- $(ls .github/workflows/*.yml .github/workflows/*.yaml 2>/dev/null)
  [ $# -gt 0 ] || { echo "check-main-concurrency: no workflows"; exit 0; }
fi
if check "$@"; then echo "check-main-concurrency: every per-commit concurrency group separates main commits ($# workflow files)"; exit 0; fi
echo "check-main-concurrency: a main commit's pending run would be replaced, not queued"
exit 1
