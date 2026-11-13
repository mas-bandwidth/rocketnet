// percommit: quick checks of each commit in a list, in parallel, each in its own worktree
// under nova-reserve: Debug DEV=ON build of everything, the light profile cases (their
// actual= lines), check_counter_contract, check_rn_standard, check_tla_records.
//
// Usage: go run . <repo> <outdir> <sha> [sha ...]
// Writes <outdir>/<sha>.log and <outdir>/summary.tsv ("sha<TAB>step<TAB>rc"), DONE last.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
)

const script = `set -u
rc_all=0
step() { name="$1"; shift; echo "=== STEP $name"; "$@"; rc=$?; echo "=== RC $name $rc"; [ $rc -ne 0 ] && rc_all=1; return 0; }
step configure cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DROCKETNET_DEV=ON
step build cmake --build build-dbg --parallel 16
step light_server ./bin/test test_light_profile_server_1000_required_memory
step light_client ./bin/test test_light_profile_client_required_memory
step light_configs ./bin/test test_light_profile_configs_create
step counter_contract python3 tools/check_counter_contract.py
step rn_standard python3 tools/conformance/check_rn_standard.py
step tla_records python3 tools/check_tla_records.py
exit $rc_all
`

func main() {
	repo, out := os.Args[1], os.Args[2]
	shas := os.Args[3:]
	if err := os.MkdirAll(out, 0o755); err != nil {
		panic(err)
	}
	sum, err := os.OpenFile(filepath.Join(out, "summary.tsv"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		panic(err)
	}
	var addLock, sumLock sync.Mutex
	var wg sync.WaitGroup
	for _, sha := range shas {
		wg.Add(1)
		go func(sha string) {
			defer wg.Done()
			dir := filepath.Join(out, "wt-"+sha)
			addLock.Lock()
			c := exec.Command("git", "worktree", "add", "--detach", dir, sha)
			c.Dir = repo
			werr := c.Run()
			addLock.Unlock()
			rc := 0
			log, lerr := os.Create(filepath.Join(out, sha+".log"))
			if werr != nil || lerr != nil {
				rc = 99
			} else {
				r := exec.Command("nova-reserve", "run", "--why", "v1.1.55 assembler per-merge quick check "+sha, "--", "bash", "-c", script)
				r.Dir = dir
				r.Stdout, r.Stderr = log, log
				if e := r.Run(); e != nil {
					rc = 1
					if x, ok := e.(*exec.ExitError); ok {
						rc = x.ExitCode()
					}
				}
				log.Close()
			}
			sumLock.Lock()
			fmt.Fprintf(sum, "%s\t%d\n", sha, rc)
			sumLock.Unlock()
		}(sha)
	}
	wg.Wait()
	fmt.Fprintln(sum, "DONE")
	sum.Close()
}
