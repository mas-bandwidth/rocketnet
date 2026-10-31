// perf_trend records one night of the performance trend
// (.github/workflows/nightly-perf-trend.yml). It measures nothing itself: it
// reads what the two existing measurements print, appends one JSON record to
// the trend history and prints the night beside the night before.
//
//   - tools/check_instructions.py's report, one line per region:
//     instructions region=<r> ir=<n> per_unit=<n> baseline=<n> delta_pct=<x>% verdict=<V>
//     The instruction gate is ci.yml's per-commit instruction-gate job; the
//     trend records the counts and the checker's verdict and gates nothing.
//   - benchmarks/run_all.sh's run.txt, one summary line per matrix row:
//     <row>.txt: pps median=<n> min=<n> max=<n>
//     (benchmarks/check_windows.py; the honesty_* plant lines are not rows).
//
// Wall-clock pps is report-only. Each row is compared with the previous
// night's the way benchmarks/PROTOCOL.md compares two sittings: a median
// inside the other sitting's min/max spread is tied; outside it the row is
// above or below. A campaign that refused (run_all.sh's REFUSED line, or no
// "campaign complete" line) is recorded as incomplete with the rows it
// finished; the campaign step has already failed the night.
//
//	go run ./tools/perf_trend --os linux --host H --ref R --commit C --date D \
//	    --bench run.txt [--instructions report.txt] [--history in.jsonl] \
//	    --out out.jsonl --record-out record.json
//
// Prints the Markdown summary. Exits 0 when recorded, 1 on malformed input,
// 2 on bad usage.
package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

var instructionLine = regexp.MustCompile(`^instructions region=(\S+) ir=(\d+) .*delta_pct=(\S+)% verdict=(PASS|FAIL)\b`)
var benchLine = regexp.MustCompile(`^(\S+)\.txt: pps median=(\S+) min=(\S+) max=(\S+)$`)

type Region struct {
	IR       int64   `json:"ir"`
	DeltaPct float64 `json:"delta_pct"`
	Verdict  string  `json:"verdict"`
}

type Instructions struct {
	Regions map[string]Region `json:"regions"`
	Note    string            `json:"note,omitempty"`
}

type Pps struct {
	Median float64 `json:"median"`
	Min    float64 `json:"min"`
	Max    float64 `json:"max"`
}

type Campaign struct {
	Rows     map[string]Pps `json:"rows"`
	Host     string         `json:"host"`
	Cores    string         `json:"cores"`
	Complete bool           `json:"complete"`
	Refused  string         `json:"refused,omitempty"`
}

type Record struct {
	Schema       string        `json:"schema"`
	OS           string        `json:"os"`
	Host         string        `json:"host"`
	Ref          string        `json:"ref"`
	Commit       string        `json:"commit"`
	Date         string        `json:"date"`
	Instructions *Instructions `json:"instructions,omitempty"`
	Campaign     Campaign      `json:"campaign"`
}

// ParseInstructions reads check_instructions.py's report. A report that
// says the toolchain is not comparable has no regions and says so in Note.
func ParseInstructions(text string) (*Instructions, error) {
	out := &Instructions{Regions: map[string]Region{}}
	for _, raw := range strings.Split(text, "\n") {
		line := strings.TrimSpace(raw)
		if strings.HasPrefix(line, "not comparable:") || strings.HasPrefix(line, "toolchain mismatch:") {
			out.Note = line
			continue
		}
		m := instructionLine.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		ir, _ := strconv.ParseInt(m[2], 10, 64)
		delta, err := strconv.ParseFloat(m[3], 64)
		if err != nil {
			return nil, fmt.Errorf("bad delta_pct in %q", line)
		}
		out.Regions[m[1]] = Region{IR: ir, DeltaPct: delta, Verdict: m[4]}
	}
	if out.Note == "" && len(out.Regions) == 0 {
		return nil, errors.New("no 'instructions region=' lines in the instruction report")
	}
	return out, nil
}

// ParseCampaign reads run_all.sh's run.txt.
func ParseCampaign(text string) (Campaign, error) {
	c := Campaign{Rows: map[string]Pps{}}
	for _, raw := range strings.Split(text, "\n") {
		line := strings.TrimSpace(raw)
		switch {
		case strings.HasPrefix(line, "REFUSED:") && c.Refused == "":
			c.Refused = line
		case strings.HasPrefix(line, "host "):
			c.Host = strings.TrimSpace(strings.TrimPrefix(line, "host "))
		case strings.HasPrefix(line, "cores "):
			c.Cores = strings.TrimSpace(strings.TrimPrefix(line, "cores "))
		case strings.HasPrefix(line, "campaign complete"):
			c.Complete = true
		}
		m := benchLine.FindStringSubmatch(line)
		if m == nil || strings.HasPrefix(m[1], "honesty_") {
			continue
		}
		var v [3]float64
		for i := range v {
			f, err := strconv.ParseFloat(m[i+2], 64)
			if err != nil {
				return c, fmt.Errorf("bad pps summary %q", line)
			}
			v[i] = f
		}
		if !(0 < v[1] && v[1] <= v[0] && v[0] <= v[2]) {
			return c, fmt.Errorf("inconsistent pps summary %q", line)
		}
		c.Rows[m[1]] = Pps{Median: v[0], Min: v[1], Max: v[2]}
	}
	if c.Complete && len(c.Rows) == 0 {
		return c, errors.New("the bench campaign completed with no pps summary rows")
	}
	if !c.Complete && c.Refused == "" {
		c.Refused = "the campaign did not complete (no 'campaign complete' line)"
	}
	return c, nil
}

// Compare says how today's row sits against the previous night's, per
// benchmarks/PROTOCOL.md: a median inside the other's spread is tied.
func Compare(today Pps, before *Pps) (string, float64) {
	if before == nil {
		return "no previous night", 0
	}
	delta := (today.Median - before.Median) / before.Median * 100
	if (before.Min <= today.Median && today.Median <= before.Max) || (today.Min <= before.Median && before.Median <= today.Max) {
		return "tied", delta
	}
	if today.Median < before.Min {
		return "below the previous spread", delta
	}
	return "above the previous spread", delta
}

// ReadHistory returns every record in a JSON-lines history; a missing file is
// an empty history (the first night).
func ReadHistory(path string) ([]Record, error) {
	if path == "" {
		return nil, nil
	}
	file, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	defer file.Close()
	var out []Record
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 1<<20), 1<<24)
	for n := 1; scanner.Scan(); n++ {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		var r Record
		if err := json.Unmarshal([]byte(line), &r); err != nil {
			return nil, fmt.Errorf("%s:%d: %v", path, n, err)
		}
		out = append(out, r)
	}
	return out, scanner.Err()
}

// previous is the newest earlier record for the same OS and ref.
func previous(history []Record, r Record) *Record {
	for i := len(history) - 1; i >= 0; i-- {
		if history[i].OS == r.OS && history[i].Ref == r.Ref {
			return &history[i]
		}
	}
	return nil
}

func Summary(r Record, before *Record) string {
	var b strings.Builder
	fmt.Fprintf(&b, "### Performance trend, %s `%s` at `%s` (%s)\n\n", r.OS, r.Ref, r.Commit, r.Date)
	if before != nil {
		fmt.Fprintf(&b, "Compared with the previous night, `%s` (%s).\n\n", before.Commit, before.Date)
	} else {
		b.WriteString("No previous night in the history: this is the first record.\n\n")
	}
	if r.Instructions != nil {
		b.WriteString("| region | Ir | vs baseline | checker | vs previous night |\n| --- | --- | --- | --- | --- |\n")
		names := make([]string, 0, len(r.Instructions.Regions))
		for name := range r.Instructions.Regions {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			region := r.Instructions.Regions[name]
			prev := "-"
			if before != nil && before.Instructions != nil {
				if p, ok := before.Instructions.Regions[name]; ok && p.IR > 0 {
					prev = fmt.Sprintf("%+.2f%%", float64(region.IR-p.IR)/float64(p.IR)*100)
				}
			}
			fmt.Fprintf(&b, "| %s | %d | %+.2f%% | %s | %s |\n", name, region.IR, region.DeltaPct, region.Verdict, prev)
		}
		if r.Instructions.Note != "" {
			fmt.Fprintf(&b, "\nInstruction counts not recorded: %s\n", r.Instructions.Note)
		}
		b.WriteString("\n")
	}
	if len(r.Campaign.Rows) > 0 {
		b.WriteString("| bench row | pps median | min | max | vs previous night |\n| --- | --- | --- | --- | --- |\n")
		names := make([]string, 0, len(r.Campaign.Rows))
		for name := range r.Campaign.Rows {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			row := r.Campaign.Rows[name]
			var prev *Pps
			if before != nil {
				if p, ok := before.Campaign.Rows[name]; ok {
					prev = &p
				}
			}
			status, delta := Compare(row, prev)
			if prev != nil {
				status = fmt.Sprintf("%s (%+.1f%%)", status, delta)
			}
			fmt.Fprintf(&b, "| %s | %g | %g | %g | %s |\n", name, row.Median, row.Min, row.Max, status)
		}
		b.WriteString("\n")
	}
	if r.Campaign.Refused != "" {
		fmt.Fprintf(&b, "Bench campaign incomplete: %s\n\n", r.Campaign.Refused)
	}
	b.WriteString("Wall-clock pps is report-only; the per-commit instruction-gate job gates instruction counts.\n")
	return b.String()
}

func run(args []string, stdout, stderr io.Writer) int {
	flags := flag.NewFlagSet("perf_trend", flag.ContinueOnError)
	flags.SetOutput(stderr)
	osName := flags.String("os", "", "linux or macos")
	host := flags.String("host", "", "the runner")
	ref := flags.String("ref", "", "the ref measured")
	commit := flags.String("commit", "", "the commit measured")
	date := flags.String("date", "", "UTC time of the night")
	bench := flags.String("bench", "", "run_all.sh's run.txt")
	instructions := flags.String("instructions", "", "check_instructions.py's report (Linux)")
	historyPath := flags.String("history", "", "the previous trend history (JSON lines); missing is the first night")
	out := flags.String("out", "", "where to write the history with this night appended")
	recordOut := flags.String("record-out", "", "where to write this night's record")
	if err := flags.Parse(args); err != nil {
		return 2
	}
	if *osName == "" || *ref == "" || *commit == "" || *bench == "" || *out == "" || *recordOut == "" || flags.NArg() != 0 {
		fmt.Fprintln(stderr, "usage: perf_trend --os OS --host H --ref R --commit C --date D --bench run.txt [--instructions report.txt] [--history in.jsonl] --out out.jsonl --record-out record.json")
		return 2
	}
	fail := func(err error) int {
		fmt.Fprintln(stderr, "perf_trend:", err)
		return 1
	}
	record := Record{Schema: "perf_trend_v1", OS: *osName, Host: *host, Ref: *ref, Commit: *commit, Date: *date}
	if *instructions != "" {
		text, err := os.ReadFile(*instructions)
		if err != nil {
			return fail(err)
		}
		if record.Instructions, err = ParseInstructions(string(text)); err != nil {
			return fail(err)
		}
	}
	text, err := os.ReadFile(*bench)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return fail(err)
	}
	if record.Campaign, err = ParseCampaign(string(text)); err != nil {
		return fail(err)
	}
	history, err := ReadHistory(*historyPath)
	if err != nil {
		return fail(err)
	}
	before := previous(history, record)
	line, err := json.Marshal(record)
	if err != nil {
		return fail(err)
	}
	var all strings.Builder
	for _, h := range history {
		l, err := json.Marshal(h)
		if err != nil {
			return fail(err)
		}
		all.Write(l)
		all.WriteString("\n")
	}
	all.Write(line)
	all.WriteString("\n")
	if err := os.WriteFile(*out, []byte(all.String()), 0o644); err != nil {
		return fail(err)
	}
	pretty, _ := json.MarshalIndent(record, "", "  ")
	if err := os.WriteFile(*recordOut, append(pretty, '\n'), 0o644); err != nil {
		return fail(err)
	}
	io.WriteString(stdout, Summary(record, before))
	return 0
}

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}
