#!/bin/sh
# check-runs-on.sh: refuse a GitHub-hosted runner in any workflow of this repository.
#
# Owner, 2026-10-18: "all CI should run on our fleet, not github." Every job's runs-on must select
# the fleet: its value names `self-hosted`, names no hosted image (ubuntu-*, macos-*, windows-*),
# and, when it is an expression, never falls through to a bare matrix or inputs value (which could
# carry a hosted label). ${{ fromJSON(matrix.runner) }} is accepted when every matrix include row
# ({ os: ... }) in the file names a self-hosted runner. The mapping form (`group:` and `labels:` under
# runs-on) is judged on its labels. Reusable-workflow calls (`uses:`) have no runs-on and are not checked.
#
# usage: check-runs-on.sh [workflow files...]   (default: .github/workflows/*.yml *.yaml)
# exit 0 when every runs-on selects the fleet, 1 with one line per refusal otherwise.
# Self-test: check-runs-on.sh --self-test
set -u

check() {
  # fromJSON(matrix.runner) selects the fleet only when every matrix include row ({ os: ... }) in the
  # file names a self-hosted runner; the files with a row that does not are listed here
  unrunnered=""
  for f in "$@"; do
    if grep -E '^[[:space:]]*-[[:space:]]*[{].*os:' "$f" | grep -vqE "runner:[[:space:]]*'[[][^]]*self-hosted"; then
      unrunnered="$unrunnered $f"
    fi
  done
  awk -v unrunnered="$unrunnered" '
    # `atfile` is the file the runs-on was read in: a block value still pending at the end of one
    # file is judged after awk has moved FILENAME to the next
    function report(v, why) { printf "%s:%d: runs-on %s: %s\n", atfile, at, why, v; bad = 1 }
    function judge(v,   t) {
      t = v
      gsub(/#.*/, "", t)
      if (t ~ /^[$][{][{] *fromJSON[(]matrix[.]runner[)] *[}][}][ \t]*$/) {
        if (index(" " unrunnered " ", " " atfile " ")) report(v, "a matrix include of this file names no self-hosted runner")
        return
      }
      if (t !~ /self-hosted/) { report(v, "does not select self-hosted (a GitHub-hosted runner)"); return }
      u = t
      gsub(/ubuntu-[0-9][0-9][0-9][0-9]([^.0-9A-Za-z-]|$)/, "", u)   # the fleet distro labels (ubuntu-2404), never a hosted image
      if (u ~ /(ubuntu|macos|windows)-[0-9a-z]/) { report(v, "names a GitHub-hosted image"); return }
      if (t ~ /\|\|[ \t]*(matrix|inputs)\./) { report(v, "falls through to a matrix/inputs value"); return }
    }
    FNR == 1 { if (pending) judge(val); pending = 0 }
    pending {
      if ($0 ~ /^[ \t]*-[ \t]/ && match($0, /[^ \t]/) > ind) { item = $0; sub(/^[ \t]*-[ \t]*/, "", item); val = val " " item; next }
      if ($0 ~ /^[ \t]*$/ || $0 ~ /^[ \t]*#/) next
      # the mapping form (`group:` / `labels:` under runs-on): the labels are what is judged, flow or
      # block list (block items are taken by the rule above); a group name is not a label
      if ($0 ~ /^[ \t]*labels:/ && match($0, /[^ \t]/) > ind) { item = $0; sub(/^[ \t]*labels:[ \t]*/, "", item); val = val " " item; next }
      if ($0 ~ /^[ \t]*group:/ && match($0, /[^ \t]/) > ind) next
      judge(val); pending = 0
    }
    /^[ \t]*runs-on:/ {
      at = FNR; atfile = FILENAME; ind = match($0, /r/); val = $0; sub(/^[ \t]*runs-on:[ \t]*/, "", val)
      if (val ~ /^[ \t]*(#.*)?$/) { val = ""; pending = 1; next }
      judge(val)
    }
    END { if (pending) judge(val); exit bad }
  ' "$@"
}

if [ "${1:-}" = "--self-test" ]; then
  d=$(mktemp -d) || exit 1
  trap 'rm -rf "$d"' EXIT
  printf 'jobs:\n  a:\n    runs-on: [self-hosted, Linux, X64, fleet]\n  b:\n    runs-on:\n      - self-hosted\n      - macOS\n      - ARM64\n      - fleet\n  c:\n    runs-on: ${{ startsWith(matrix.os, '"'"'ubuntu'"'"') && fromJSON('"'"'["self-hosted","Linux","X64","fleet"]'"'"') || fromJSON('"'"'["self-hosted","macOS","ARM64","fleet"]'"'"') }}\n  e:\n    runs-on: [self-hosted, Linux, X64, fleet, ubuntu-2404]\n' > "$d/good.yml"
  printf 'jobs:\n  d:\n    runs-on: ${{ fromJSON(matrix.runner) }}\n    strategy:\n      matrix:\n        include:\n          - { os: ubuntu-24.04, runner: '"'"'["self-hosted","Linux","X64","fleet"]'"'"' }\n' >> "$d/good.yml"
  check "$d/good.yml" >/dev/null || { echo "self-test: a fleet workflow was refused"; check "$d/good.yml"; exit 1; }
  printf 'jobs:\n  a:\n    runs-on: ${{ fromJSON(matrix.runner) }}\n    strategy:\n      matrix:\n        include:\n          - { os: ubuntu-24.04, runner: '"'"'["self-hosted","Linux","X64","fleet"]'"'"' }\n          - { os: macos-15 }\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: a matrix row without a fleet runner was accepted"; exit 1; fi
  for v in 'ubuntu-latest' '[ubuntu-24.04]' 'macos-15' 'windows-latest' '${{ matrix.os }}' \
           '${{ startsWith(matrix.os, '"'"'ubuntu'"'"') \&\& fromJSON('"'"'["self-hosted","Linux","X64","fleet"]'"'"') || matrix.os }}' \
           '[self-hosted, ubuntu-24.04]' '[self-hosted, windows-2025]' '[self-hosted, ubuntu-2404.1]'; do
    printf 'jobs:\n  a:\n    runs-on: %s\n' "$v" > "$d/bad.yml"
    if check "$d/bad.yml" >/dev/null; then echo "self-test: hosted runs-on was accepted: $v"; exit 1; fi
  done
  printf 'jobs:\n  a:\n    runs-on:\n      - ubuntu-latest\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: hosted block list was accepted"; exit 1; fi
  # a hosted block list at the end of one file is reported at that file, not at the next
  printf 'jobs:\n  b:\n    runs-on: [self-hosted, Linux, X64, fleet]\n' > "$d/next.yml"
  check "$d/bad.yml" "$d/next.yml" | grep -q "^$d/bad.yml:3: runs-on does not select" || { echo "self-test: a refusal at the end of a file was reported at another file"; check "$d/bad.yml" "$d/next.yml"; exit 1; }
  # the mapping form: judged on its labels, flow or block list; a group alone selects nothing
  printf 'jobs:\n  g:\n    runs-on:\n      group: fleet\n      labels: [self-hosted, Linux, X64, fleet]\n  h:\n    runs-on:\n      labels:\n        - self-hosted\n        - fleet\n' > "$d/group.yml"
  check "$d/group.yml" >/dev/null || { echo "self-test: a mapping runs-on whose labels select self-hosted was refused"; check "$d/group.yml"; exit 1; }
  printf 'jobs:\n  g:\n    runs-on:\n      group: larger\n      labels: [ubuntu-latest]\n' > "$d/bad.yml"
  check "$d/bad.yml" | grep -q "does not select self-hosted.*ubuntu-latest" || { echo "self-test: a mapping runs-on with hosted labels was not refused on its labels"; exit 1; }
  printf 'jobs:\n  g:\n    runs-on:\n      group: larger\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: a group-only runs-on was accepted"; exit 1; fi
  printf 'jobs:\n  g:\n    runs-on: group: larger\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: a one-line group runs-on was accepted unjudged"; exit 1; fi
  # zero inputs: a tree with no workflow files is red, never "nothing to refuse"
  self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
  mkdir "$d/empty"
  if (cd "$d/empty" && sh "$self" >/dev/null); then echo "self-test: zero workflow files was accepted"; exit 1; fi
  echo "check-runs-on self-test: ok"
  exit 0
fi

if [ $# -eq 0 ]; then
  for f in .github/workflows/*.yml .github/workflows/*.yaml; do
    if [ -e "$f" ]; then set -- "$@" "$f"; fi
  done
  [ $# -gt 0 ] || { echo "check-runs-on: 0 workflow files under .github/workflows: nothing was checked"; exit 1; }
fi
if check "$@"; then echo "check-runs-on: every runs-on selects the fleet ($# workflow files)"; exit 0; fi
echo "check-runs-on: GitHub-hosted runners are refused (owner, 2026-10-18: \"all CI should run on our fleet, not github\")"
exit 1
