#!/bin/sh
# Replay ci.yml's lint job and the per-commit Linux build-and-test rows at one
# commit, each row in its own worktree, in parallel under nova-reserve.
# Usage: sh replay-rows.sh <commit> <logdir>
C=$1; L=$2
G=$HOME/bench/xdp-vlan-gates
W=$HOME/bench/xdp-vlan-replay
mkdir -p $L $W
cd $G || exit 1
nova-secrets exec --only GH_GATE_TOKEN -- git -c credential.helper= -c credential.helper='!f() { test "$1" = get && echo username=x-access-token && echo "password=$GH_GATE_TOKEN"; }; f' fetch -q origin fix/v1.1.9/xdp-vlan; echo fetch=$?
git cat-file -e $C^{commit} || { echo "no commit $C"; exit 1; }
row() {
  name=$1; shift
  git -C $G worktree remove --force $W/$name 2>/dev/null
  git -C $G worktree add -q --detach $W/$name $C || { echo "ROW $name worktree failed"; return; }
  ( cd $W/$name && nova-reserve run --why "replay $name" -- python3 tools/replay_ci_steps.py .github/workflows/ci.yml "$@" --logs $L/$name > $L/$name.out 2>&1; echo "ROW $name rc=$?" >> $L/rows.txt ) &
}
git -C $G worktree prune
: > $L/rows.txt
row lint lint --set inputs.full=false
row rel-checks build-and-test --set matrix.os=linux --set matrix.build_type=Release --set matrix.leg=checks --set matrix.dev=OFF --set inputs.full=false
row rel-unit-0 build-and-test --set matrix.os=linux --set matrix.build_type=Release --set matrix.leg=unit --set matrix.shard=0/2 --set matrix.dev=OFF --set inputs.full=false
row rel-unit-1 build-and-test --set matrix.os=linux --set matrix.build_type=Release --set matrix.leg=unit --set matrix.shard=1/2 --set matrix.with_builds=true --set matrix.dev=OFF --set inputs.full=false
row dbg-unit build-and-test --set matrix.os=linux --set matrix.build_type=Debug --set matrix.leg=unit --set matrix.dev=ON --set inputs.full=false
row dbg-func build-and-test --set matrix.os=linux --set matrix.build_type=Debug --set matrix.leg=functional --set matrix.dev=ON --set inputs.full=false
row rel-func build-and-test --set matrix.os=linux --set matrix.build_type=Release --set matrix.leg=functional --set matrix.dev=OFF --set inputs.full=false
row dbg-matrix build-and-test --set matrix.os=linux --set matrix.build_type=Debug --set matrix.leg=transport_matrix --set matrix.dev=ON --set inputs.full=false
row rel-matrix build-and-test --set matrix.os=linux --set matrix.build_type=Release --set matrix.leg=transport_matrix --set matrix.dev=OFF --set inputs.full=false
wait
cat $L/rows.txt
for f in $L/*.out; do echo "== $f"; grep -E 'FAIL|failed|exit [1-9]|STEP.*rc=[1-9]|^REPLAY' $f | head -8; tail -2 $f; done
echo REPLAY-ROWS-DONE
