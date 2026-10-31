package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const instructionsReport = `isolation mode=per_region
instructions region=client_frame ir=787000 per_unit=12296.88 baseline=787980 delta_pct=-0.12% verdict=PASS
instructions region=server_tick ir=115000000 per_unit=1796875.00 baseline=114119350 delta_pct=+0.77% verdict=PASS
`

func campaign(row string, complete bool) string {
	text := "rocketnet transport bench campaign\nhost            Linux 6.8 amd-2\ncores           recv=15 send=14\n" +
		"honesty_bench_portable_100000.txt: pps median=100000 min=99000 max=101000\n" +
		"bench_portable.txt: pps " + row + "\n" +
		"bench_gso.txt: pps median=3e+06 min=2.9e+06 max=3.1e+06\n"
	if complete {
		text += "campaign complete -- results\n"
	}
	return text
}

func TestParseInstructions(t *testing.T) {
	got, err := ParseInstructions(instructionsReport)
	if err != nil {
		t.Fatal(err)
	}
	if r := got.Regions["server_tick"]; r.IR != 115000000 || r.DeltaPct != 0.77 || r.Verdict != "PASS" {
		t.Fatalf("server_tick %+v", r)
	}
	if _, err := ParseInstructions("nothing here\n"); err == nil {
		t.Fatal("a report with no regions was accepted")
	}
	got, err = ParseInstructions("not comparable: GNU-14.2.0/valgrind-3.22.0 vs GNU-15.2.0/valgrind-3.26.0\n")
	if err != nil || len(got.Regions) != 0 || !strings.HasPrefix(got.Note, "not comparable") {
		t.Fatalf("not comparable report: %+v %v", got, err)
	}
}

func TestParseCampaignSkipsHonestyRows(t *testing.T) {
	c, err := ParseCampaign(campaign("median=1e+06 min=900000 max=1.1e+06", true))
	if err != nil {
		t.Fatal(err)
	}
	if !c.Complete || len(c.Rows) != 2 || c.Rows["bench_portable"].Median != 1e6 {
		t.Fatalf("campaign %+v", c)
	}
	if c.Cores != "recv=15 send=14" {
		t.Fatalf("cores %q", c.Cores)
	}
}

func TestParseCampaignRefusals(t *testing.T) {
	c, err := ParseCampaign(campaign("median=1e+06 min=900000 max=1.1e+06", false) + "REFUSED: bench_uring matrix process failed; see x\n")
	if err != nil || c.Complete || !strings.HasPrefix(c.Refused, "REFUSED: bench_uring") || len(c.Rows) != 2 {
		t.Fatalf("refused campaign %+v %v", c, err)
	}
	c, err = ParseCampaign("")
	if err != nil || c.Refused == "" {
		t.Fatalf("an empty run.txt is incomplete: %+v %v", c, err)
	}
	if _, err := ParseCampaign("bench_gso.txt: pps median=5 min=6 max=7\n"); err == nil {
		t.Fatal("a median below its min was accepted")
	}
	if _, err := ParseCampaign("campaign complete\n"); err == nil {
		t.Fatal("a complete campaign with no rows was accepted")
	}
}

func TestCompareFollowsProtocol(t *testing.T) {
	before := Pps{Median: 1e6, Min: 9e5, Max: 1.1e6}
	for _, c := range []struct {
		today Pps
		want  string
	}{
		{Pps{Median: 1.05e6, Min: 1e6, Max: 1.2e6}, "tied"},
		{Pps{Median: 8e5, Min: 7.9e5, Max: 8.1e5}, "below the previous spread"},
		{Pps{Median: 1.3e6, Min: 1.25e6, Max: 1.35e6}, "above the previous spread"},
		{Pps{Median: 8.5e5, Min: 8e5, Max: 1.05e6}, "tied"},
	} {
		if got, _ := Compare(c.today, &before); got != c.want {
			t.Fatalf("%+v: got %q want %q", c.today, got, c.want)
		}
	}
	if got, _ := Compare(before, nil); got != "no previous night" {
		t.Fatalf("no previous: %q", got)
	}
}

func night(t *testing.T, dir, row, history, commit string) (string, string) {
	t.Helper()
	bench := filepath.Join(dir, commit+"-run.txt")
	instr := filepath.Join(dir, commit+"-instr.txt")
	os.WriteFile(bench, []byte(campaign(row, true)), 0o644)
	os.WriteFile(instr, []byte(instructionsReport), 0o644)
	out := filepath.Join(dir, commit+".jsonl")
	var stdout, stderr bytes.Buffer
	args := []string{"--os", "linux", "--host", "amd-2", "--ref", "main", "--commit", commit, "--date", "2026-10-31",
		"--bench", bench, "--instructions", instr, "--out", out, "--record-out", filepath.Join(dir, commit+".json")}
	if history != "" {
		args = append(args, "--history", history)
	}
	if code := run(args, &stdout, &stderr); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	return out, stdout.String()
}

func TestRunCarriesTheHistoryForward(t *testing.T) {
	dir := t.TempDir()
	first, text := night(t, dir, "median=1e+06 min=900000 max=1.1e+06", filepath.Join(dir, "missing.jsonl"), "aaa")
	if !strings.Contains(text, "first record") {
		t.Fatalf("first night summary: %s", text)
	}
	second, text := night(t, dir, "median=950000 min=940000 max=980000", first, "bbb")
	if !strings.Contains(text, "| bench_portable | 950000 | 940000 | 980000 | tied (-5.0%)") {
		t.Fatalf("second night summary: %s", text)
	}
	third, text := night(t, dir, "median=500000 min=490000 max=510000", second, "ccc")
	if !strings.Contains(text, "below the previous spread") || !strings.Contains(text, "`bbb`") {
		t.Fatalf("third night summary: %s", text)
	}
	records, err := ReadHistory(third)
	if err != nil || len(records) != 3 || records[0].Commit != "aaa" || records[2].Commit != "ccc" {
		t.Fatalf("history %+v %v", records, err)
	}
	data, _ := os.ReadFile(filepath.Join(dir, "ccc.json"))
	var r Record
	if err := json.Unmarshal(data, &r); err != nil || r.Instructions.Regions["client_frame"].IR != 787000 {
		t.Fatalf("record %s %v", data, err)
	}
}

func TestBadInput(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.jsonl")
	os.WriteFile(bad, []byte("{not json\n"), 0o644)
	bench := filepath.Join(dir, "run.txt")
	os.WriteFile(bench, []byte(campaign("median=1e+06 min=900000 max=1.1e+06", true)), 0o644)
	var stdout, stderr bytes.Buffer
	base := []string{"--os", "linux", "--ref", "main", "--commit", "c", "--bench", bench, "--out", filepath.Join(dir, "o"), "--record-out", filepath.Join(dir, "r")}
	if code := run(append(base, "--history", bad), &stdout, &stderr); code != 1 {
		t.Fatalf("a corrupt history exited %d", code)
	}
	if code := run([]string{"--os", "linux"}, &stdout, &stderr); code != 2 {
		t.Fatalf("missing arguments exited %d", code)
	}
}
