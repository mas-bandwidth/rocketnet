#!/bin/sh
# check-runs-on.sh: refuse a GitHub-hosted runner in any workflow of this repository.
#
# Owner, 2026-10-18: "all CI should run on our fleet, not github." Every job's runs-on must select
# the fleet: its value names `self-hosted`, names no hosted image (ubuntu-*, macos-*, windows-*),
# and, when it is an expression, never falls through to a bare matrix or inputs value (which could
# carry a hosted label). ${{ fromJSON(matrix.runner) }} is accepted when every matrix include row
# ({ os: ... }) in the file names a self-hosted runner. Reusable-workflow calls (`uses:`) have no runs-on and are not checked.
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
    function report(v, why) { printf "%s:%d: runs-on %s: %s\n", FILENAME, at, why, v; bad = 1 }
    function judge(v,   t) {
      t = v
      gsub(/#.*/, "", t)
      if (t ~ /^[$][{][{] *fromJSON[(]matrix[.]runner[)] *[}][}][ \t]*$/) {
        if (index(" " unrunnered " ", " " FILENAME " ")) report(v, "a matrix include of this file names no self-hosted runner")
        return
      }
      if (t !~ /self-hosted/) { report(v, "does not select self-hosted (a GitHub-hosted runner)"); return }
      if (t ~ /(ubuntu|macos|windows)-[0-9a-z]/) { report(v, "names a GitHub-hosted image"); return }
      if (t ~ /\|\|[ \t]*(matrix|inputs)\./) { report(v, "falls through to a matrix/inputs value"); return }
    }
    FNR == 1 { if (pending) judge(val); pending = 0 }
    pending {
      if ($0 ~ /^[ \t]*-[ \t]/ && match($0, /[^ \t]/) > ind) { item = $0; sub(/^[ \t]*-[ \t]*/, "", item); val = val " " item; next }
      if ($0 ~ /^[ \t]*$/ || $0 ~ /^[ \t]*#/) next
      judge(val); pending = 0
    }
    /^[ \t]*runs-on:/ {
      at = FNR; ind = match($0, /r/); val = $0; sub(/^[ \t]*runs-on:[ \t]*/, "", val)
      if (val ~ /^[ \t]*(#.*)?$/) { val = ""; pending = 1; next }
      if (val ~ /^(group|labels):/ ) { val = ""; pending = 0; next }
      judge(val)
    }
    END { if (pending) judge(val); exit bad }
  ' "$@"
}

if [ "${1:-}" = "--self-test" ]; then
  d=$(mktemp -d) || exit 1
  trap 'rm -rf "$d"' EXIT
  printf 'jobs:\n  a:\n    runs-on: [self-hosted, Linux, X64, fleet]\n  b:\n    runs-on:\n      - self-hosted\n      - macOS\n      - ARM64\n      - fleet\n  c:\n    runs-on: ${{ startsWith(matrix.os, '"'"'ubuntu'"'"') && fromJSON('"'"'["self-hosted","Linux","X64","fleet"]'"'"') || fromJSON('"'"'["self-hosted","macOS","ARM64","fleet"]'"'"') }}\n' > "$d/good.yml"
  printf 'jobs:\n  d:\n    runs-on: ${{ fromJSON(matrix.runner) }}\n    strategy:\n      matrix:\n        include:\n          - { os: ubuntu-24.04, runner: '"'"'["self-hosted","Linux","X64","fleet"]'"'"' }\n' >> "$d/good.yml"
  check "$d/good.yml" >/dev/null || { echo "self-test: a fleet workflow was refused"; check "$d/good.yml"; exit 1; }
  printf 'jobs:\n  a:\n    runs-on: ${{ fromJSON(matrix.runner) }}\n    strategy:\n      matrix:\n        include:\n          - { os: ubuntu-24.04, runner: '"'"'["self-hosted","Linux","X64","fleet"]'"'"' }\n          - { os: macos-15 }\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: a matrix row without a fleet runner was accepted"; exit 1; fi
  for v in 'ubuntu-latest' '[ubuntu-24.04]' 'macos-15' 'windows-latest' '${{ matrix.os }}' \
           '${{ startsWith(matrix.os, '"'"'ubuntu'"'"') \&\& fromJSON('"'"'["self-hosted","Linux","X64","fleet"]'"'"') || matrix.os }}' \
           '[self-hosted, ubuntu-24.04]'; do
    printf 'jobs:\n  a:\n    runs-on: %s\n' "$v" > "$d/bad.yml"
    if check "$d/bad.yml" >/dev/null; then echo "self-test: hosted runs-on was accepted: $v"; exit 1; fi
  done
  printf 'jobs:\n  a:\n    runs-on:\n      - ubuntu-latest\n' > "$d/bad.yml"
  if check "$d/bad.yml" >/dev/null; then echo "self-test: hosted block list was accepted"; exit 1; fi
  echo "check-runs-on self-test: ok"
  exit 0
fi

if [ $# -eq 0 ]; then
  set -- $(ls .github/workflows/*.yml .github/workflows/*.yaml 2>/dev/null)
  [ $# -gt 0 ] || { echo "check-runs-on: no workflows"; exit 0; }
fi
if check "$@"; then echo "check-runs-on: every runs-on selects the fleet ($# workflow files)"; exit 0; fi
echo "check-runs-on: GitHub-hosted runners are refused (owner, 2026-10-18: \"all CI should run on our fleet, not github\")"
exit 1
