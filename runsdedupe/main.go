// runsdedupe: rewrite a RUNS.tsv keeping one record per config: the one with the latest
// started_utc (column 9). The first occurrence's position is kept. Prints each dropped row.
// Usage: go run . <RUNS.tsv>
package main

import (
	"fmt"
	"os"
	"strings"
)

func main() {
	path := os.Args[1]
	data, err := os.ReadFile(path)
	if err != nil {
		panic(err)
	}
	rows := strings.Split(strings.TrimRight(string(data), "\n"), "\n")
	best := map[string]int{}
	for i, r := range rows {
		if i == 0 {
			continue
		}
		f := strings.Split(r, "\t")
		if len(f) < 9 {
			fmt.Println("short row:", r)
			os.Exit(2)
		}
		if j, ok := best[f[0]]; ok {
			g := strings.Split(rows[j], "\t")
			if f[8] > g[8] {
				best[f[0]] = i
			}
		} else {
			best[f[0]] = i
		}
	}
	var out []string
	seen := map[string]bool{}
	dropped := 0
	for i, r := range rows {
		if i == 0 {
			out = append(out, r)
			continue
		}
		c := strings.Split(r, "\t")[0]
		if seen[c] {
			continue
		}
		seen[c] = true
		out = append(out, rows[best[c]])
	}
	for i, r := range rows {
		if i == 0 {
			continue
		}
		c := strings.Split(r, "\t")[0]
		if best[c] != i {
			f := strings.Split(r, "\t")
			fmt.Printf("DROP %s started=%s input=%s (kept started=%s)\n", c, f[8], f[2][:12], strings.Split(rows[best[c]], "\t")[8])
			dropped++
		}
	}
	if err := os.WriteFile(path, []byte(strings.Join(out, "\n")+"\n"), 0o644); err != nil {
		panic(err)
	}
	fmt.Printf("rows %d -> %d, dropped %d\n", len(rows)-1, len(out)-1, dropped)
}
