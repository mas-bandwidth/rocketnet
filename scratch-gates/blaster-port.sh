#!/bin/sh
# The blaster reply-port fix: shown red on the old read, green on the fix,
# plus the gates its file touches. Run from the scratch checkout:
# sh blaster-port.sh <commit> <logdir>
S=$(cd "$(dirname "$0")" && pwd)
C=$1; L=$2
J=$HOME/bench/xdp-vlan-gates
mkdir -p $L
cd $J || exit 1
nova-secrets exec --only GH_GATE_TOKEN -- git -c credential.helper= -c credential.helper='!f() { test "$1" = get && echo username=x-access-token && echo "password=$GH_GATE_TOKEN"; }; f' fetch -q origin fix/v1.1.9/xdp-vlan; echo fetch=$?
git checkout -q -f --detach $C || exit 1
echo "tree=$(git log --oneline -1)"
nova-reserve run -- make JOBS=4 > $L/rel-build.log 2>&1; echo release-build=$?
echo release-warnings=$(grep -c "warning:" $L/rel-build.log)
./bin/xdp_packet_blaster --self-test > $L/selftest-fixed.log 2>&1; echo "selftest-fixed=$? :: $(tail -1 $L/selftest-fixed.log)"
nova-reserve run -- cmake --build build --target test_xdp_packet_blaster > $L/blaster.log 2>&1; echo "blaster-test=$? :: $(tail -1 $L/blaster.log)"
git apply $S/blaster-old-port.patch; echo "mutant-applied=$? $(git diff --stat | tail -1)"
nova-reserve run -- cmake --build build --target xdp_packet_blaster > $L/mutant-build.log 2>&1; echo mutant-build=$?
./bin/xdp_packet_blaster --self-test > $L/selftest-mutant.log 2>&1; echo "selftest-mutant=$? (must be nonzero)"
grep 'self-test failed' $L/selftest-mutant.log
git checkout -q -- tools/xdp_packet_blaster.c; echo "reverted=$(git status --short | grep -c '^ M')"
nova-reserve run -- cmake --build build --target xdp_packet_blaster > $L/restore-build.log 2>&1; echo restore-build=$?
./bin/xdp_packet_blaster --self-test > $L/selftest-restored.log 2>&1; echo "selftest-restored=$? :: $(tail -1 $L/selftest-restored.log)"
make format-check > $L/format.log 2>&1; echo "format=$? :: $(tail -1 $L/format.log)"
python3 tools/check_client_server_words.py > $L/words.log 2>&1; echo "words=$? :: $(tail -1 $L/words.log)"
rm -rf b-clang
cmake -B b-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-clang/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-clang/bin > $L/clang-config.log 2>&1; echo clang-config=$?
nova-reserve run -- cmake --build b-clang --target xdp_packet_blaster --parallel 4 > $L/clang-build.log 2>&1; echo clang-blaster-build=$? warnings=$(grep -c "warning:" $L/clang-build.log)
./b-clang/bin/xdp_packet_blaster --self-test > $L/clang-selftest.log 2>&1; echo "clang-selftest=$? :: $(tail -1 $L/clang-selftest.log)"
CPPCHECK=/usr/bin/cppcheck nova-reserve run -- python3 tools/run_tidy.py --jobs 4 > $L/tidy.log 2>&1; echo tidy=$?
grep -E '^(clang-tidy|cppcheck):|tidy gate' $L/tidy.log
echo BLASTER-PORT-DONE
