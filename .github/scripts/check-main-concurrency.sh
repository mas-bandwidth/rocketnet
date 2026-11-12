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
# This refuses a per-commit workflow (one started by push, pull_request,
# pull_request_target, merge_group or workflow_run, the last chained off a
# per-commit run) with any concurrency group, top level or job level, string
# or block form, that names neither github.sha nor github.run_id. The rule is
# an allowlist (VERIFY-3 A-1): a constant group ("ci"), a workflow-only group,
# a ref_name or head_ref group and a ref-only group such as
# "records-${{ github.ref }}" each put every main commit in one group, and a
# group serializes a shared write only when the workflow writes, which none
# of these does. Schedule- and dispatch-only workflows (the nightlies) are not
# per-commit and are left alone. The spellings read here are the ones
# tools/check_workflow_shape.py holds every workflow to.
#
# usage: check-main-concurrency.sh [workflow files...]   (default: .github/workflows/*.yml *.yaml)
# exit 0 when every per-commit group separates main commits, 1 with one line per refusal otherwise.
# Self-test: check-main-concurrency.sh --self-test
set -u

check() {
  awk '
    function lead(line) { match(line, /[^ ]/); return RSTART - 1 }
    function consider(v, line) {
      sub(/[ \t]+#.*$/, "", v)
      if (v !~ /github[.](sha|run_id)/) { n++; badv[n] = v; badline[n] = line }
    }
    function flush(   i) {
      if (percommit) {
        for (i = 1; i <= n; i++) {
          printf "%s:%d: concurrency group %s does not separate main commits (it names neither github.sha nor github.run_id)\n", file, badline[i], badv[i]
          bad = 1
        }
      }
      percommit = 0; n = 0; inon = 0; incc = 0
    }
    FNR == 1 { if (NR > 1) flush(); file = FILENAME }
    /^[ \t]*#/ { next }
    /^on:/ {
      inon = 1; incc = 0
      if ($0 ~ /(push|pull_request|merge_group|workflow_run)/) percommit = 1
      next
    }
    inon && /^  (push|pull_request|pull_request_target|merge_group|workflow_run):/ { percommit = 1 }
    inon && /^  - (push|pull_request|pull_request_target|merge_group|workflow_run)[ \t]*$/ { percommit = 1 }
    inon && /^[a-zA-Z_-]+:/ { inon = 0 }
    incc && /[^ \t]/ && lead($0) <= ccind { incc = 0 }
    /^ *concurrency:/ {
      ccind = lead($0); v = $0; sub(/^ *concurrency:[ \t]*/, "", v); sub(/^#.*$/, "", v)
      if (v != "") consider(v, FNR); else incc = 1
      next
    }
    incc && /^ *group:/ {
      v = $0; sub(/^ *group:[ \t]*/, "", v)
      consider(v, FNR)
      incc = 0
      next
    }
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
  # failing fixtures, VERIFY-3 A-1: a constant group, a workflow-only group, a head_ref/event.ref group,
  # a string-form group, a job-level group, and a group on a workflow chained off a per-commit run
  head='name: x\non:\n  push:\n    branches: [main]\n'
  job='jobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n'
  printf "${head}"'concurrency:\n  group: planted\n  cancel-in-progress: false\n'"${job}" > "$d/bad-constant.yml"
  printf "${head}"'concurrency:\n  group: ${{ github.workflow }}\n'"${job}" > "$d/bad-workflow.yml"
  printf "${head}"'concurrency:\n  group: planted-${{ github.head_ref || github.event.ref }}\n'"${job}" > "$d/bad-ref-name.yml"
  printf "${head}"'concurrency: planted-${{ github.ref }}\n'"${job}" > "$d/bad-string.yml"
  printf "${head}"'jobs:\n  a:\n    runs-on: [self-hosted]\n    concurrency:\n      group: planted-${{ github.ref }}\n    steps:\n      - run: echo hi\n' > "$d/bad-job-level.yml"
  printf 'name: x\non:\n  workflow_run:\n    workflows: [CI]\n    types: [completed]\nconcurrency:\n  group: chained\n'"${job}" > "$d/bad-chained.yml"
  for f in bad-constant bad-workflow bad-ref-name bad-string bad-job-level bad-chained; do
    if check "$d/$f.yml" >/dev/null; then echo "self-test: $f.yml was accepted"; exit 1; fi
  done
  # a commented-out group line is not read as the group
  printf "${head}"'concurrency:\n  # group: x-${{ github.sha }}\n  group: planted\n'"${job}" > "$d/bad-commented.yml"
  if check "$d/bad-commented.yml" >/dev/null; then echo "self-test: a commented sha line hid a constant group"; exit 1; fi
  # passing fixtures: separated main group, a schedule-only ref group, and no concurrency
  printf 'name: x\non:\n  push:\n    branches: [main]\nconcurrency:\n  group: foo-${{ github.workflow }}-${{ github.ref == '"'"'refs/heads/main'"'"' && github.sha || github.ref }}\n  cancel-in-progress: ${{ github.ref != '"'"'refs/heads/main'"'"' }}\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-sep.yml"
  printf 'name: x\non:\n  schedule:\n    - cron: "30 5 * * *"\n  workflow_dispatch:\nconcurrency:\n  group: nightly-${{ github.ref }}\n  cancel-in-progress: true\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-nightly.yml"
  printf 'name: x\non:\n  push:\n    branches: [main]\njobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: echo hi\n' > "$d/good-none.yml"
  printf 'name: x\non:\n  pull_request:\njobs:\n  a:\n    runs-on: [self-hosted]\n    concurrency:\n      group: a-${{ github.run_id }}\n    steps:\n      - run: echo hi\n' > "$d/good-job-run-id.yml"
  check "$d/good-sep.yml" "$d/good-nightly.yml" "$d/good-none.yml" "$d/good-job-run-id.yml" >/dev/null || { echo "self-test: a separating, nightly or no-concurrency workflow was refused"; check "$d/good-sep.yml" "$d/good-nightly.yml" "$d/good-none.yml" "$d/good-job-run-id.yml"; exit 1; }
  # run from a directory with no .github/workflows: nothing to check is a failure, never a pass
  self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
  mkdir "$d/empty"
  if (cd "$d/empty" && sh "$self" >/dev/null); then echo "self-test: no workflows was accepted"; exit 1; fi
  echo "check-main-concurrency self-test: ok"
  exit 0
fi

if [ $# -eq 0 ]; then
  set -- $(ls .github/workflows/*.yml .github/workflows/*.yaml 2>/dev/null)
  [ $# -gt 0 ] || { echo "check-main-concurrency: no workflows found under .github/workflows: nothing was checked"; exit 1; }
fi
if check "$@"; then echo "check-main-concurrency: every per-commit concurrency group separates main commits ($# workflow files)"; exit 0; fi
echo "check-main-concurrency: a main commit's pending run would be replaced, not queued"
exit 1
