#!/bin/bash
# xdp-vlan gates on hetzner2: usage h2-gates.sh <commit> <logdir>
# INTEGRATE-v115's gate list (as integrate-v117/h1-gates.sh ran it), the XDP
# guest gates, the text checkers, tidy, format, xver, check_abi and the pins.
J=~/bench/xdp-vlan-gates
S=~/bench/xdp-vlan/scratch-gates
C=$1
L=~/bench/gates-logs/$2
mkdir -p $L
if [ ! -d $J ]; then
  git clone -q ~/bench/xdp-vlan $J || exit 1
  git -C $J remote set-url origin https://github.com/mas-bandwidth/rocketnet.git
fi
cd $J || exit 1
nova-secrets exec --only GH_GATE_TOKEN -- git -c credential.helper= -c credential.helper='!f() { test "$1" = get && echo username=x-access-token && echo "password=$GH_GATE_TOKEN"; }; f' fetch -q --tags origin; echo fetch=$?
git checkout -q -f --detach $C || exit 1
git log --oneline -1
git status --short | head
gcc --version | head -1; clang --version | head -1; uname -r
uptime
rm -rf build bin build-debug b-clang b-asan b-lsan b-prod b-tsan b-be
nova-reserve run -- make JOBS=4 > $L/rel-build.log 2>&1; echo release-build=$?
echo release-warnings=$(grep -c "warning:" $L/rel-build.log)
# ---- guest gates
nova-reserve run -- bash tools/run_xdp_filter_guest.sh --artifact-dir $L/xdp-filter-guest > $L/xdp-filter-guest.log 2>&1; echo xdp-filter-guest=$?; tail -1 $L/xdp-filter-guest.log
grep -a -h 'verifier: processed' $L/xdp-filter-guest/* 2>/dev/null | sort | uniq -c
llvm-objdump -h build/generated/server_xdp.o | grep ' server_xdp '
ROCKETNET_NIGHTLY_NO_TIMEOUT=1 nova-reserve run -- scripts/transport-cells-repeat --runs 1 --cell xdp/portable > $L/xdp-cells.log 2>&1; echo xdp-cells=$?
grep -a -E '^CELL|XDP-GUEST|TRANSPORT-CELLS-REPEAT|fail=[1-9]|DID-NOT-BOOT' $L/xdp-cells.log | head -20
nova-reserve run -- bash scripts/vm-boot-guard-selftest > $L/vm-boot-guard-selftest.log 2>&1; echo vm-boot-guard-selftest=$?
echo qemu-left=$(pgrep -u "$(id -un)" -c qemu-system)
# ---- per-commit Release
nova-reserve run -- cmake --build build --target test_xdp_packet_blaster > $L/blaster.log 2>&1; echo blaster-test=$?
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/test > $L/rel-unit.log 2>&1; echo release-unit=$?; tail -1 $L/rel-unit.log
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/functional > $L/rel-func.log 2>&1; echo release-functional=$?; tail -1 $L/rel-func.log
echo release-functional-skips=$(grep -c "SKIPPED" $L/rel-func.log)
ROCKETNET_TEST_VERBOSE=1 ./bin/test --run test_light_profile_server_1000_required_memory 2>&1 | grep actual
ROCKETNET_TEST_VERBOSE=1 ./bin/test --run test_light_profile_client_required_memory 2>&1 | grep actual
LOAD_PLAN_ONLY=1 ./bin/load 1000 60 > $L/load-plan.log 2>&1; echo load-plan=$?; grep -a -o 'server_public_required_bytes[^ ]*' $L/load-plan.log | head -2
nova-reserve run -- ./bin/dst --seeds 1-50 > $L/dst.log 2>&1; echo dst=$?; tail -1 $L/dst.log
for s in outage outage-past send-fail; do
  nova-reserve run -- ./bin/dst --seeds 1-50 --schedule $s > $L/dst-sched-$s.log 2>&1; echo dst-schedule-$s=$? fail-lines=$(grep -c "^FAIL" $L/dst-sched-$s.log); tail -1 $L/dst-sched-$s.log
done
nova-reserve run -- ./bin/dst --outage all --seeds 1-50 > $L/dst-outage.log 2>&1; echo dst-outage=$? fail-lines=$(grep -c "^FAIL" $L/dst-outage.log); tail -1 $L/dst-outage.log
ROCKETNET_TEST_NIGHTLY=1 ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/test --enforce-rss > $L/nightly-unit-rss.log 2>&1; echo nightly-unit-rss=$?; tail -1 $L/nightly-unit-rss.log
ROCKETNET_TEST_NIGHTLY=1 ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/functional --enforce-rss > $L/nightly-func-rss.log 2>&1; echo nightly-func-rss=$?; tail -1 $L/nightly-func-rss.log
./bin/source_audit > $L/audit.log 2>&1; echo source-audit=$?
python3 tools/check_memory.py ./bin/transport_memory > $L/memory.log 2>&1; echo memory=$?
make format-check > $L/format.log 2>&1; echo format=$?; tail -1 $L/format.log
python3 tools/check_xdp_guard_control.py > $L/xdpguard.log 2>&1; echo xdp-guard=$?
python3 tools/check_xdp_archive.py > $L/xdparchive.log 2>&1; echo xdp-archive=$?
python3 tools/check_byte_order.py > $L/byteorder.log 2>&1; echo byte-order=$?
bash tools/check_fp_contract.sh --self-test > $L/fp.log 2>&1 && bash tools/check_fp_contract.sh >> $L/fp.log 2>&1; echo fp-contract=$?
python3 tools/test_check_abi.py > $L/abi.log 2>&1 && python3 tools/check_abi.py >> $L/abi.log 2>&1; echo abi=$?; tail -1 $L/abi.log
python3 tools/test_check_exports.py > $L/exports.log 2>&1; echo exports=$?
python3 tools/test_check_hardening.py > $L/hardening.log 2>&1; echo hardening=$?
bash tools/check_unit_no_sockets.sh > $L/nosockets.log 2>&1; echo no-sockets=$?
bash scripts/rocketnet-provision selftest > $L/provision.log 2>&1; echo provision-selftest=$?
python3 tools/check_transport_matrix.py > $L/transport-matrix-check.log 2>&1; echo check-transport-matrix=$?
bash scripts/live-xdp-fleet-remote i40e-guard-selftest ./bin/server > $L/i40e-guard.log 2>&1; echo i40e-guard-selftest=$?; tail -1 $L/i40e-guard.log
# ---- text checkers (the integrate-v117 list), one per line
n=0; while IFS= read -r line; do
  case "$line" in ''|'#'*) continue ;; esac
  n=$((n + 1)); sh -c "$line" > $L/text-$n.log 2>&1; rc=$?
  if [ $rc != 0 ]; then echo "text-check-$n=$rc $line"; fi
done < $S/checks-text.txt
echo text-checks-run=$n
# ---- xver (nightly-xver), gso and io_uring
(cd tools/xver && go test -timeout=0 xver.go xver_test.go > $L/xver-test.log 2>&1); echo xver-go-test=$?
W=$J/xver-work; rm -rf $W; git worktree prune; mkdir -p $W
prev=$(git tag --merged HEAD --list 'v*' --sort=-v:refname | grep -v -- - | head -1)
echo xver-previous=$prev
(cd tools/xver && go run xver.go build -tree ../.. -build $W/build-newest > $L/xver-build.log 2>&1 && git worktree add -q --detach $W/tree-$prev $prev && go run xver.go build -tree $W/tree-$prev -build $W/build-$prev >> $L/xver-build.log 2>&1 && git worktree add -q --detach $W/tree-v1.0.0 v1.0.0 && go run xver.go build -tree $W/tree-v1.0.0 -build $W/build-v1.0.0 >> $L/xver-build.log 2>&1); echo xver-build=$?
(cd tools/xver && nova-reserve run -- go run xver.go run -bin newest=$W/build-newest/xver -bin $prev=$W/build-$prev/xver -bin v1.0.0=$W/build-v1.0.0/xver -new newest -transports gso,io_uring -parallel 4 -out $W/runs > $L/xver-run.log 2>&1); echo xver-run=$?; tail -2 $L/xver-run.log
rm -rf $W; git worktree prune
# ---- tidy (its own configure)
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > $L/tidy-config.log 2>&1; echo tidy-config=$?
/usr/bin/cppcheck --version; clang-tidy-18 --version | head -2
CPPCHECK=/usr/bin/cppcheck nova-reserve run -- python3 tools/run_tidy.py --jobs 4 > $L/tidy.log 2>&1; echo tidy=$?; tail -3 $L/tidy.log
# ---- production DEV=OFF
cmake -S . -B b-prod -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-prod/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-prod/bin > $L/prod-config.log 2>&1; echo prod-config=$?
nova-reserve run -- cmake --build b-prod --parallel 4 -- -k > $L/prod-build.log 2>&1; echo prod-build=$? errors=$(grep -c "error:" $L/prod-build.log) warnings=$(grep -c "warning:" $L/prod-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./b-prod/bin/test > $L/prod-unit.log 2>&1; echo prod-unit=$?; tail -1 $L/prod-unit.log
ROCKETNET_TEST_NIGHTLY=1 ROCKETNET_TEST_JOBS=4 ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable nova-reserve run -- ./b-prod/bin/functional_production > $L/prod-func.log 2>&1; echo prod-functional=$?; tail -1 $L/prod-func.log
# ---- Debug
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DROCKETNET_DEV=ON -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/build-debug/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/build-debug/bin > $L/dbg-config.log 2>&1; echo debug-config=$?
nova-reserve run -- cmake --build build-debug --parallel 4 > $L/dbg-build.log 2>&1; echo debug-build=$? warnings=$(grep -c "warning:" $L/dbg-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./build-debug/bin/test > $L/dbg-unit.log 2>&1; echo debug-unit=$?; tail -1 $L/dbg-unit.log
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./build-debug/bin/functional > $L/dbg-func.log 2>&1; echo debug-functional=$?; tail -1 $L/dbg-func.log
# ---- clang Debug
cmake -B b-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-clang/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-clang/bin > $L/clang-config.log 2>&1; echo clang-config=$?
nova-reserve run -- cmake --build b-clang --parallel 4 -- -k > $L/clang-build.log 2>&1; echo clang-build=$? errors=$(grep -c "error:" $L/clang-build.log) warnings=$(grep -c "warning:" $L/clang-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./b-clang/bin/test > $L/clang-unit.log 2>&1; echo clang-unit=$?; tail -1 $L/clang-unit.log
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./b-clang/bin/functional > $L/clang-func.log 2>&1; echo clang-functional=$?; tail -1 $L/clang-func.log
# ---- ASan Release
cmake -B b-asan -DCMAKE_BUILD_TYPE=Release -DROCKETNET_SANITIZE_ADDRESS=ON -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-asan/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-asan/bin > $L/asan-config.log 2>&1; echo asan-config=$?
nova-reserve run -- cmake --build b-asan --parallel 4 -- -k > $L/asan-build.log 2>&1; echo asan-build=$? errors=$(grep -c "error:" $L/asan-build.log) warnings=$(grep -c "warning:" $L/asan-build.log)
# ---- ASan+UBSan Debug (nightly-leaks)
cmake -B b-lsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-lsan/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-lsan/bin > $L/lsan-config.log 2>&1; echo lsan-config=$?
nova-reserve run -- cmake --build b-lsan --parallel 4 -- -k > $L/lsan-build.log 2>&1; echo lsan-build=$? errors=$(grep -c "error:" $L/lsan-build.log) warnings=$(grep -c "warning:" $L/lsan-build.log)
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 LSAN_OPTIONS=suppressions=tools/lsan.supp:print_suppressions=1 ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./b-lsan/bin/test > $L/lsan-unit.log 2>&1; echo lsan-unit=$?; tail -1 $L/lsan-unit.log
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 LSAN_OPTIONS=suppressions=tools/lsan.supp:print_suppressions=1 ROCKETNET_TEST_CASE_TIME_SCALE=2 ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./b-lsan/bin/functional > $L/lsan-func.log 2>&1; echo lsan-functional=$?; tail -1 $L/lsan-func.log
# ---- big-endian emulated
cmake -B b-be -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-ffp-contract=off "-DCMAKE_CXX_FLAGS=-ffp-contract=off -DROCKETNET_TEST_EMULATED=1" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-be/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-be/bin > $L/be-config.log 2>&1; echo be-config=$?
nova-reserve run -- cmake --build b-be --target test functional bigendian_interop --parallel 4 -- -k > $L/be-build.log 2>&1; echo be-build=$? errors=$(grep -c "error:" $L/be-build.log) warnings=$(grep -c "warning:" $L/be-build.log)
# ---- TSan as nightly.yml configures it
cmake -B b-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1" -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/b-tsan/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/b-tsan/bin > $L/tsan-config.log 2>&1; echo tsan-config=$?
nova-reserve run -- cmake --build b-tsan --parallel 4 -- -k > $L/tsan-build.log 2>&1; echo tsan-build-all=$? errors=$(grep -c "error:" $L/tsan-build.log) warnings=$(grep -c "warning:" $L/tsan-build.log)
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 ROCKETNET_TEST_CASE_TIME_SCALE=4 nova-reserve run -- setarch "$(uname -m)" -R ./b-tsan/bin/test > $L/tsan-unit.log 2>&1; echo tsan-unit=$?; tail -1 $L/tsan-unit.log
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 ROCKETNET_TEST_CASE_TIME_SCALE=4 nova-reserve run -- setarch "$(uname -m)" -R ./b-tsan/bin/functional > $L/tsan-func.log 2>&1; echo tsan-functional=$?; tail -1 $L/tsan-func.log
grep -an "WARNING: ThreadSanitizer" $L/tsan-unit.log $L/tsan-func.log | head
# ---- the changed fuzz harness under the nightly TSan flags, gcc and clang
Wf="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion -Werror"
for cxx in g++ clang++; do
  $cxx -g -std=c++17 -ffp-contract=off -DROCKETNET_DEV=1 -Isodium -Isdk -Itests $Wf -fsanitize=thread -fno-omit-frame-pointer -O1 -c tests/fuzz/rocketnet_fuzz_xdp_filter.cpp -o $L/fuzz-xdp-filter.$cxx.o > $L/fuzz-tsan-$cxx.log 2>&1; echo fuzz-tsan-flags-xdp-filter-$cxx=$?
done
git status --short | head
uptime
echo GATES-DONE
