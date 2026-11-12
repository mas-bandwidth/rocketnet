#!/bin/sh
# macbook: the new head (format, rename, i40e guard): Release and Debug builds
# with warnings counted, unit, functional, and the i40e rule selftest
J=$HOME/rowan-working/xdp-vlan-mac-gates
L=$J/m2
mkdir -p $L
cd $J/rocketnet || exit 1
git log --oneline -1
git status --short | head
nova-reserve run -- make JOBS=4 > $L/rel-build.log 2>&1; echo release-build=$?
echo release-warnings=$(grep -c "warning:" $L/rel-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/test > $L/rel-unit.log 2>&1; echo release-unit=$?; tail -1 $L/rel-unit.log
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./bin/functional > $L/rel-func.log 2>&1; echo release-functional=$?; tail -1 $L/rel-func.log
bash scripts/live-xdp-fleet-remote i40e-guard-selftest ./bin/server > $L/i40e-guard.log 2>&1; echo i40e-guard-selftest=$?; tail -1 $L/i40e-guard.log
nova-reserve run -- cmake --build build-debug --parallel 4 > $L/dbg-build.log 2>&1; echo debug-build=$?
echo debug-warnings=$(grep -c "warning:" $L/dbg-build.log)
ROCKETNET_TEST_JOBS=4 nova-reserve run -- ./build-debug/bin/test > $L/dbg-unit.log 2>&1; echo debug-unit=$?; tail -1 $L/dbg-unit.log
echo MAC-G2-DONE
