// replayall: replay every per-commit Linux CI row of one commit on this bench.
//
// Usage: go run main.go <repo> <sha> <outdir> [name-substring ...]
//
// It makes <outdir>/base (a detached worktree of <sha>), asks
// tools/replay_ci_steps.py --rows for ci.yml's rows, keeps the rows whose
// name names neither macos nor windows, adds the static.yml jobs and the
// transport-cells-repeat --list checks, and runs each row in its own fresh
// worktree under nova-reserve. Each row's output goes to <outdir>/<row>.log;
// <outdir>/summary.tsv gets "name<TAB>rc<TAB>seconds" per row, and DONE last.
// With name substrings, only rows whose name contains one of them run.
package main

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

type row struct{ name, cmd string }

func main() {
	if len(os.Args) < 4 {
		fmt.Fprintln(os.Stderr, "usage: replayall <repo> <sha> <outdir> [name-substring ...]")
		os.Exit(2)
	}
	repo, sha, out := os.Args[1], os.Args[2], os.Args[3]
	only := os.Args[4:]
	must(os.MkdirAll(out, 0o755))
	base := filepath.Join(out, "base")
	must(run(repo, "git", "worktree", "add", "--detach", base, sha))

	listing, err := exec.Command("bash", "-c", "cd "+base+" && python3 tools/replay_ci_steps.py --rows .github/workflows/ci.yml").Output()
	must(err)
	var rows []row
	var name string
	sc := bufio.NewScanner(bytes.NewReader(listing))
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "ROW ") {
			name = strings.TrimPrefix(line, "ROW ")
			continue
		}
		t := strings.TrimSpace(line)
		if name != "" && strings.HasPrefix(t, "python3 ") {
			low := strings.ToLower(name)
			if !strings.Contains(low, "macos") && !strings.Contains(low, "windows") {
				rows = append(rows, row{name, t})
			}
			name = ""
		}
	}
	rows = append(rows,
		row{"static gcc -fanalyzer", "python3 tools/replay_ci_steps.py .github/workflows/static.yml static"},
		row{"static clang-tidy cppcheck", "python3 tools/replay_ci_steps.py .github/workflows/static.yml tidy"},
		row{"transport-cells list linux", "bash scripts/transport-cells-repeat --list --os linux"},
		row{"transport-cells list macos", "bash scripts/transport-cells-repeat --list --os macos"},
		row{"transport-cells list windows", "bash scripts/transport-cells-repeat --list --os windows"},
	)
	if len(only) > 0 {
		var kept []row
		for _, r := range rows {
			for _, o := range only {
				if strings.Contains(r.name, o) {
					kept = append(kept, r)
					break
				}
			}
		}
		rows = kept
	}

	clean := regexp.MustCompile(`[^A-Za-z0-9]+`)
	summary, err := os.OpenFile(filepath.Join(out, "summary.tsv"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	must(err)
	var addLock, sumLock sync.Mutex
	var wg sync.WaitGroup
	for _, r := range rows {
		wg.Add(1)
		go func(r row) {
			defer wg.Done()
			dir := filepath.Join(out, strings.Trim(clean.ReplaceAllString(r.name, "-"), "-"))
			addLock.Lock()
			err := run(repo, "git", "worktree", "add", "--detach", dir, sha)
			addLock.Unlock()
			rc := 0
			start := time.Now()
			log, lerr := os.Create(dir + ".log")
			if err != nil || lerr != nil {
				rc = 99
			} else {
				fmt.Fprintf(log, "ROW %s\nCMD %s\n", r.name, r.cmd)
				c := exec.Command("nova-reserve", "run", "--why", "cutter v1.1.54 replay: "+r.name, "--", "bash", "-c", r.cmd)
				c.Dir = dir
				c.Stdout, c.Stderr = log, log
				if e := c.Run(); e != nil {
					rc = 1
					if x, ok := e.(*exec.ExitError); ok {
						rc = x.ExitCode()
					}
				}
				log.Close()
			}
			sumLock.Lock()
			fmt.Fprintf(summary, "%s\t%d\t%.1f\n", r.name, rc, time.Since(start).Seconds())
			sumLock.Unlock()
		}(r)
	}
	wg.Wait()
	fmt.Fprintln(summary, "DONE")
	summary.Close()
}

func run(dir string, name string, args ...string) error {
	c := exec.Command(name, args...)
	c.Dir = dir
	c.Stdout, c.Stderr = os.Stderr, os.Stderr
	return c.Run()
}

func must(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
