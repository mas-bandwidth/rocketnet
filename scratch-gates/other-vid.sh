#!/bin/sh
# The other-VID and untagged guest case: green on the fix, red with the
# filter's VLAN ID match removed, plus the gates the commit touches.
# Run from the scratch dir: sh other-vid.sh <commit> <logdir>
S=$(cd "$(dirname "$0")" && pwd)
C=$1; L=$2
J=$HOME/bench/xdp-vlan-gates
mkdir -p $L
cd $J || exit 1
nova-secrets exec --only GH_GATE_TOKEN -- git -c credential.helper= -c credential.helper='!f() { test "$1" = get && echo username=x-access-token && echo "password=$GH_GATE_TOKEN"; }; f' fetch -q origin fix/v1.1.9/xdp-vlan; echo fetch=$?
git checkout -q -f --detach $C || exit 1
echo "tree=$(git log --oneline -1)"
cells() {
  tag=$1
  ROCKETNET_NIGHTLY_NO_TIMEOUT=1 nova-reserve run -- scripts/transport-cells-repeat --runs 1 --cell xdp/portable > $L/cells-$tag.log 2>&1; echo cells-$tag=$?
  grep -a -E '^CELL|TRANSPORT-CELLS-REPEAT|DID-NOT-BOOT' $L/cells-$tag.log
  d=$(grep -a -o 'logs=[^ ]*' $L/cells-$tag.log | tail -1 | cut -d= -f2)
  [ -n "$d" ] && grep -a -r -h -E 'test_xdp_transport_vlan_other_vid_and_untagged_ipv4: (phase|far end)|other_vid.*(FAIL|check)' $d | head -12
}
nova-reserve run -- make JOBS=4 > $L/rel-build.log 2>&1; echo release-build=$?
echo release-warnings=$(grep -c "warning:" $L/rel-build.log)
cells fixed
git apply $S/filter-any-vid.patch; echo "mutant-applied=$? $(git diff --stat | tail -1)"
nova-reserve run -- make JOBS=4 > $L/mutant-build.log 2>&1; echo mutant-build=$?
cells mutant
git checkout -q -- sdk/xdp/server_xdp.c; echo "reverted=$(git status --short | grep -c '^ M')"
nova-reserve run -- make JOBS=4 > $L/restore-build.log 2>&1; echo restore-build=$?
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/test > $L/rel-unit.log 2>&1; echo "release-unit=$? :: $(tail -1 $L/rel-unit.log)"
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/functional > $L/rel-func.log 2>&1; echo "release-functional=$? :: $(tail -1 $L/rel-func.log)"
make format-check > $L/format.log 2>&1; echo "format=$? :: $(tail -1 $L/format.log)"
python3 tools/check_client_server_words.py > $L/words.log 2>&1; echo "words=$? :: $(tail -1 $L/words.log)"
rm -rf b-clang
cmake -B b-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-clang/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-clang/bin > $L/clang-config.log 2>&1; echo clang-config=$?
nova-reserve run -- cmake --build b-clang --target functional --parallel 4 > $L/clang-build.log 2>&1; echo clang-functional-build=$? warnings=$(grep -c "warning:" $L/clang-build.log)
CPPCHECK=/usr/bin/cppcheck nova-reserve run -- python3 tools/run_tidy.py --jobs 4 > $L/tidy.log 2>&1; echo tidy=$?
grep -E '^(clang-tidy|cppcheck):|tidy gate' $L/tidy.log
sh $S/text-checks-show.sh $C $HOME/bench/xdp-vlan-text $L/text > $L/text.out 2>&1
grep -E 'executed=|rc=[1-9]|words-after' $L/text.out
echo OTHER-VID-DONE
