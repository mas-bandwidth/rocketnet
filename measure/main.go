// measure: the readings a measurement commit needs, at one sha, in its own worktree under
// nova-reserve: the light profile actual= lines (Debug and Release, DEV=ON), the loopback
// golden TRACE lines, the stale TLA+ groups (pinned tlacheck), and the record checks.
//
// Usage: go run . <repo> <outdir> <sha> <tlacheck>
// Writes <outdir>/measure-<sha>.log; prints DONE <rc> as its last line.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

const script = `set -u
rc_all=0
step() { name="$1"; shift; echo "=== STEP $name"; "$@"; rc=$?; echo "=== RC $name $rc"; [ $rc -ne 0 ] && rc_all=1; return 0; }
export ROCKETNET_TEST_VERBOSE=1
step stale "$TLACHECK" groups --root proofs --stale
step configure_dbg cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DROCKETNET_DEV=ON
step build_dbg cmake --build build-dbg --parallel 16
step dbg_light_server ./bin/test test_light_profile_server_1000_required_memory
step dbg_light_client ./bin/test test_light_profile_client_required_memory
step dbg_light_configs ./bin/test test_light_profile_configs_create
ROCKETNET_LOOPBACK_PRINT_TRACE=1 step dbg_loopback ./bin/test test_loopback_transport_patterns_off_same_trace
step counter_contract python3 tools/check_counter_contract.py
step rn_standard python3 tools/conformance/check_rn_standard.py
step tla_records python3 tools/check_tla_records.py
step configure_rel cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON
step build_rel cmake --build build-rel --parallel 16
step rel_light_server ./bin/test test_light_profile_server_1000_required_memory
step rel_light_client ./bin/test test_light_profile_client_required_memory
ROCKETNET_LOOPBACK_PRINT_TRACE=1 step rel_loopback ./bin/test test_loopback_transport_patterns_off_same_trace
exit $rc_all
`

func main() {
	repo, out, sha, tlacheck := os.Args[1], os.Args[2], os.Args[3], os.Args[4]
	if err := os.MkdirAll(out, 0o755); err != nil {
		panic(err)
	}
	dir := filepath.Join(out, "wt-measure-"+sha)
	c := exec.Command("git", "worktree", "add", "--detach", dir, sha)
	c.Dir = repo
	if b, err := c.CombinedOutput(); err != nil {
		fmt.Printf("worktree: %v %s\nDONE 99\n", err, b)
		return
	}
	log, err := os.Create(filepath.Join(out, "measure-"+sha+".log"))
	if err != nil {
		panic(err)
	}
	r := exec.Command("nova-reserve", "run", "--why", "v1.1.55 assembler measurement "+sha, "--", "bash", "-c", script)
	r.Dir = dir
	r.Env = append(os.Environ(), "TLACHECK="+tlacheck)
	r.Stdout, r.Stderr = log, log
	rc := 0
	if e := r.Run(); e != nil {
		rc = 1
		if x, ok := e.(*exec.ExitError); ok {
			rc = x.ExitCode()
		}
	}
	log.Close()
	fmt.Printf("DONE %d\n", rc)
}
