#!/bin/sh
# xdp-vlan macOS gates on the macbook: Release build, unit, functional, DST (+ schedules, outage all),
# the RSS-enforced nightly tier (unit and functional), production functional, happy_path with boot-volume Go caches;
# Debug build, unit, functional; ASan+UBSan and TSan Debug builds of nightly-macos-sanitizers' targets.
# usage: mac.sh <logdir>
J=$HOME/rowan-working/xdp-vlan-mac-gates
L=$J/$1
mkdir -p $L
cd $J/rocketnet || exit 1
git log --oneline -1
git status --short | head
uptime
nova-reserve run -- make JOBS=4 > $L/rel-build.log 2>&1; echo release-build=$?
echo release-warnings=$(grep -c "warning:" $L/rel-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/test > $L/rel-unit.log 2>&1; echo release-unit=$?; tail -1 $L/rel-unit.log
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/functional > $L/rel-func.log 2>&1; echo release-functional=$?; tail -1 $L/rel-func.log
nova-reserve run -- ./bin/dst --seeds 1-50 > $L/dst.log 2>&1; echo dst=$?; tail -1 $L/dst.log
for s in outage outage-past send-fail; do
  nova-reserve run -- ./bin/dst --seeds 1-50 --schedule $s > $L/dst-sched-$s.log 2>&1; echo dst-schedule-$s=$?; tail -1 $L/dst-sched-$s.log
done
nova-reserve run -- ./bin/dst --outage all --seeds 1-50 > $L/dst-outage.log 2>&1; echo dst-outage=$?; tail -1 $L/dst-outage.log
uptime
ROCKETNET_TEST_NIGHTLY=1 ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/test --enforce-rss > $L/nightly-unit-rss.log 2>&1; echo nightly-unit-rss=$?; tail -1 $L/nightly-unit-rss.log
ROCKETNET_TEST_NIGHTLY=1 ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/functional --enforce-rss > $L/nightly-func-rss.log 2>&1; echo nightly-func-rss=$?; tail -1 $L/nightly-func-rss.log
# happy_path as the CI step runs it, with the Go caches the CI fix sets
mkdir -p $HOME/Library/Caches/rocketnet-ci/go-build $HOME/Library/Caches/rocketnet-ci/go-mod
(cd tools/happy_path && GOCACHE=$HOME/Library/Caches/rocketnet-ci/go-build GOMODCACHE=$HOME/Library/Caches/rocketnet-ci/go-mod ROCKETNET_NIGHTLY_NO_TIMEOUT=1 nova-reserve run -- go test -count=1 -timeout=0 happy_path.go happy_path_test.go) > $L/happy-test.log 2>&1; echo happy-go-test=$?; tail -1 $L/happy-test.log
(cd tools/happy_path && GOCACHE=$HOME/Library/Caches/rocketnet-ci/go-build GOMODCACHE=$HOME/Library/Caches/rocketnet-ci/go-mod ROCKETNET_NIGHTLY_NO_TIMEOUT=1 nova-reserve run -- go run happy_path.go) > $L/happy-run.log 2>&1; echo happy-go-run=$?; tail -1 $L/happy-run.log
cmake -S . -B b-prod -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/rocketnet/b-prod/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/rocketnet/b-prod/bin > $L/prod-config.log 2>&1; echo prod-config=$?
nova-reserve run -- cmake --build b-prod --target functional_production test --parallel 4 > $L/prod-build.log 2>&1; echo prod-build=$?
echo prod-warnings=$(grep -c "warning:" $L/prod-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./b-prod/bin/test > $L/prod-unit.log 2>&1; echo prod-unit=$?; tail -1 $L/prod-unit.log
ROCKETNET_TEST_NIGHTLY=1 ROCKETNET_TEST_JOBS=4 ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable nova-reserve run -- ./b-prod/bin/functional_production > $L/prod-func.log 2>&1; echo prod-functional=$?; tail -1 $L/prod-func.log
uptime
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DROCKETNET_DEV=ON -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/rocketnet/build-debug/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/rocketnet/build-debug/bin > $L/dbg-config.log 2>&1; echo debug-config=$?
nova-reserve run -- cmake --build build-debug --parallel 4 > $L/dbg-build.log 2>&1; echo debug-build=$?
echo debug-warnings=$(grep -c "warning:" $L/dbg-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./build-debug/bin/test > $L/dbg-unit.log 2>&1; echo debug-unit=$?; tail -1 $L/dbg-unit.log
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./build-debug/bin/functional > $L/dbg-func.log 2>&1; echo debug-functional=$?; tail -1 $L/dbg-func.log
for leg in asan tsan; do
  if [ $leg = asan ]; then flags="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"; else flags="-fsanitize=thread -fno-omit-frame-pointer -O1"; fi
  cmake -S . -B b-$leg -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$J/rocketnet/b-$leg/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$J/rocketnet/b-$leg/bin > $L/$leg-config.log 2>&1; echo $leg-config=$?
  nova-reserve run -- cmake --build b-$leg --target test functional test_serialize test_transport_control test_msgx --parallel 4 > $L/$leg-build.log 2>&1; echo $leg-build=$?
  echo $leg-errors=$(grep -c "error:" $L/$leg-build.log) $leg-warnings=$(grep -c "warning:" $L/$leg-build.log)
done
nova-reserve run -- bash scripts/vm-boot-guard-selftest > $L/vm-boot-guard-selftest.log 2>&1; echo vm-boot-guard-selftest=$?; tail -2 $L/vm-boot-guard-selftest.log
sh tools/docs/check_agents_md.sh all > $L/agents-md.log 2>&1; echo check-agents-md=$?
clang++ -g -std=c++17 -ffp-contract=off -DROCKETNET_DEV=1 -Isodium -Isdk -Itests -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion -Werror -fsanitize=thread -fno-omit-frame-pointer -O1 -c tests/fuzz/rocketnet_fuzz_xdp_filter.cpp -o $L/fuzz_xdp_filter.o > $L/fuzz-xdp-filter-tsan.log 2>&1; echo fuzz-xdp-filter-tsan-flags=$?
uptime
echo MAC-DONE
