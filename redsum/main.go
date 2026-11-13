// redsum: for each red row in <outdir>/summary.tsv, print the red steps of its row log and,
// from each red step's FULL step log, the lines that name a failure (and the last lines).
// Usage: go run . <outdir> [max-lines-per-step]
package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

var clean = regexp.MustCompile(`[^A-Za-z0-9]+`)
var exitRe = regexp.MustCompile(`^\s+exit (\d+) after [0-9.]+ s \((\S+)\)`)
var failRe = regexp.MustCompile(`(?i)(check failed|error[: ]|fail|traceback|assert|refused|mismatch|expected|not found|warning:|duplicate|stale|uncited|over the cap|timed out)`)

func lines(path string) []string {
	f, err := os.Open(path)
	if err != nil {
		return []string{"(cannot read " + path + ": " + err.Error() + ")"}
	}
	defer f.Close()
	var out []string
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	for sc.Scan() {
		out = append(out, sc.Text())
	}
	return out
}

func main() {
	out := os.Args[1]
	max := 25
	if len(os.Args) > 2 {
		max, _ = strconv.Atoi(os.Args[2])
	}
	for _, row := range lines(filepath.Join(out, "summary.tsv")) {
		p := strings.Split(row, "\t")
		if len(p) < 2 || p[1] == "0" {
			continue
		}
		name := p[0]
		log := filepath.Join(out, strings.Trim(clean.ReplaceAllString(name, "-"), "-")+".log")
		fmt.Printf("##### ROW %s rc=%s\n", name, p[1])
		rl := lines(log)
		found := false
		for i, l := range rl {
			m := exitRe.FindStringSubmatch(l)
			if m == nil || m[1] == "0" {
				continue
			}
			found = true
			step := ""
			if i > 0 {
				step = rl[i-1]
			}
			fmt.Printf("  STEP %s -> exit %s (%s)\n", strings.TrimSpace(step), m[1], m[2])
			sl := lines(m[2])
			n := 0
			for _, s := range sl {
				if failRe.MatchString(s) {
					fmt.Printf("    | %s\n", s)
					n++
					if n >= max {
						break
					}
				}
			}
			start := len(sl) - 6
			if start < 0 {
				start = 0
			}
			fmt.Printf("    -- last lines:\n")
			for _, s := range sl[start:] {
				fmt.Printf("    . %s\n", s)
			}
		}
		if !found {
			start := len(rl) - 15
			if start < 0 {
				start = 0
			}
			fmt.Printf("  (no red step line) last row-log lines:\n")
			for _, s := range rl[start:] {
				fmt.Printf("    . %s\n", s)
			}
		}
	}
}
