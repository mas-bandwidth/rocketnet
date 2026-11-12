#!/bin/sh
# hetzner2, in the gate clone after g2's release build: the i40e guard
# selftest green, then red with each half of the rule removed, then restored
cd ~/bench/xdp-vlan-gates || exit 1
git log --oneline -1
bash scripts/live-xdp-fleet-remote i40e-guard-selftest ./bin/server; echo "I0 rc=$?"
# I1: the script's rule allows every release
sed -i 's/^    \[\[ "\$release" =~/    return 1; [[ "$release" =~/' scripts/live-xdp-fleet-remote
echo "I1 applied $(git diff --stat | tail -1)"
bash scripts/live-xdp-fleet-remote i40e-guard-selftest ./bin/server; echo "I1 rc=$?"
git checkout -- scripts/live-xdp-fleet-remote
# I2: the server's rule allows every release
sed -i 's/^    const int soak\[4\] = { 7, 0, 0, 43 };/    const int soak[4] = { 0, 0, 0, 0 };/' tests/client_server/server.c
echo "I2 applied $(git diff --stat | tail -1)"
cmake --build build --target server > build/mutate-i40e.log 2>&1; echo "I2 build rc=$?"
bash scripts/live-xdp-fleet-remote i40e-guard-selftest ./bin/server; echo "I2 rc=$?"
git checkout -- tests/client_server/server.c
cmake --build build --target server > build/mutate-i40e.log 2>&1; echo "restore build rc=$?"
bash scripts/live-xdp-fleet-remote i40e-guard-selftest ./bin/server; echo "I3 restored rc=$?"
echo MUTATE-I40E-DONE
