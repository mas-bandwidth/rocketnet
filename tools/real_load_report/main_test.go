package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type logOpts struct {
	connected, unexpected, disconnects int
	updateP99                          int
	dropCall                           string
	noP999                             bool
}

func serverLog(o logOpts) string {
	if o.connected == 0 {
		o.connected = 1000
	}
	if o.updateP99 == 0 {
		o.updateP99 = 40
	}
	lines := []string{"split_ready: schema=split_v1 role=server run_id=0123456789abcdef total=1000"}
	for _, call := range calls {
		if call == o.dropCall {
			continue
		}
		p99 := 40
		if call == "server_update" {
			p99 = o.updateP99
		}
		p999 := " p999_upper_us=90"
		if o.noP999 {
			p999 = ""
		}
		lines = append(lines, fmt.Sprintf("split_server_call: run_id=0123456789abcdef total=1000 transport=gso "+
			"scope=traffic_plus_drain load=proof call=%s count=100 p50_upper_us=3 p99_upper_us=%d%s "+
			"max_us=310.500 overbudget_count=2 budget_us=100", call, p99, p999))
	}
	verdict := "PASS"
	if o.disconnects != 0 {
		verdict = "FAIL"
	}
	lines = append(lines, fmt.Sprintf("split_proof: role=server run_id=0123456789abcdef transport=gso disconnects=%d verdict=%s", o.disconnects, verdict))
	lines = append(lines, fmt.Sprintf("split_receipt: schema=split_v1 role=server run_id=0123456789abcdef total=1000 first=0 "+
		"count=1000 connected=%d c2s_accepted=12000 c2s_received=11990 s2c_accepted=12000 s2c_received=12000 "+
		"unexpected_disconnects=%d verdict=FAIL", o.connected, o.unexpected))
	return strings.Join(lines, "\n") + "\n"
}

const mergePass = "split_proof_cell: verdict=PASS transport=gso total=1000 streams=26000 " +
	"messages_sent=24000 messages_delivered=23990 unreliable_lost=10 sequenced_skipped=0 timing_verdict=FAIL\n"

func cellDir(t *testing.T, server, merge *string, console string) string {
	t.Helper()
	dir := t.TempDir()
	write := func(name, body string) {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if server != nil {
		write("server.log", *server)
	}
	if merge != nil {
		write("merge.log", *merge)
	}
	write("lan-proof.console", console)
	return dir
}

func ptr(s string) *string { return &s }

func TestCleanCellPassesAndReportsQuantiles(t *testing.T) {
	row := Summarise("gso", cellDir(t, ptr(serverLog(logOpts{})), ptr(mergePass), ""))
	if row.Fields["status"] != "PASS" {
		t.Fatalf("status %s: %s", row.Fields["status"], row.Fields["reason"])
	}
	if got := row.Fields["update_p99_us"] + "/" + row.Fields["update_p999_us"] + "/" + row.Fields["update_max_us"]; got != "40/90/310.500" {
		t.Fatalf("update quantiles %s", got)
	}
	if row.Fields["unreliable_lost"] != "10" {
		t.Fatalf("unreliable_lost %s", row.Fields["unreliable_lost"])
	}
}

func TestCallTimesAreReportedNeverGated(t *testing.T) {
	row := Summarise("gso", cellDir(t, ptr(serverLog(logOpts{updateP99: 150})), ptr(mergePass), ""))
	if row.Fields["status"] != "PASS" || row.Fields["update_p99_us"] != "150" {
		t.Fatalf("status %s p99 %s", row.Fields["status"], row.Fields["update_p99_us"])
	}
}

func expectFail(t *testing.T, row Row, reason string) {
	t.Helper()
	if row.Fields["status"] != "FAIL" || !strings.Contains(row.Fields["reason"], reason) {
		t.Fatalf("want FAIL with %q, got %s: %s", reason, row.Fields["status"], row.Fields["reason"])
	}
}

func TestShortConnectFails(t *testing.T) {
	expectFail(t, Summarise("gso", cellDir(t, ptr(serverLog(logOpts{connected: 998})), ptr(mergePass), "")), "connected 998 of 1000")
}

func TestDisconnectFails(t *testing.T) {
	expectFail(t, Summarise("gso", cellDir(t, ptr(serverLog(logOpts{disconnects: 3, unexpected: 3})), ptr(mergePass), "")), "disconnects=3")
}

func TestRefusedProofCellFails(t *testing.T) {
	merge := "split_proof_cell: verdict=FAIL reason=stream_mismatch\n"
	expectFail(t, Summarise("gso", cellDir(t, ptr(serverLog(logOpts{})), &merge, "")), "stream_mismatch")
}

func TestMissingServerLogNamesTheRefusal(t *testing.T) {
	console := "cell gso\nlan-proof: FAIL: a host is hot: load 9 on 16 cores\n"
	expectFail(t, Summarise("gso", cellDir(t, nil, nil, console)), "a host is hot")
}

func TestMissingTimingFails(t *testing.T) {
	expectFail(t, Summarise("gso", cellDir(t, ptr(serverLog(logOpts{dropCall: "server_send_packets"})), ptr(mergePass), "")), "server call timing missing")
}

// a server built before p999_upper_us is not a reportable night
func TestMissingP999Fails(t *testing.T) {
	expectFail(t, Summarise("gso", cellDir(t, ptr(serverLog(logOpts{noP999: true})), ptr(mergePass), "")), "server call timing missing")
}

func TestRunWritesArtifactsAndExitStatus(t *testing.T) {
	good := cellDir(t, ptr(serverLog(logOpts{})), ptr(mergePass), "")
	out := filepath.Join(t.TempDir(), "out")
	var stdout, stderr bytes.Buffer
	if code := run([]string{"--out", out, "--head", "abc", "--date", "2026-10-31", "gso=" + good}, &stdout, &stderr); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	if !strings.Contains(stdout.String(), "real_load: cell=gso status=PASS") {
		t.Fatalf("stdout %s", stdout.String())
	}
	data, err := os.ReadFile(filepath.Join(out, "real-load.json"))
	if err != nil {
		t.Fatal(err)
	}
	var record struct {
		Cells []struct {
			Calls map[string]map[string]string `json:"calls"`
		} `json:"cells"`
	}
	if err := json.Unmarshal(data, &record); err != nil {
		t.Fatal(err)
	}
	if got := record.Cells[0].Calls["server_update"]["p999_upper_us"]; got != "90" {
		t.Fatalf("json p999 %q", got)
	}
	tsv, _ := os.ReadFile(filepath.Join(out, "real-load.tsv"))
	if n := len(strings.Split(strings.TrimSpace(string(tsv)), "\n")); n != 2 {
		t.Fatalf("tsv lines %d", n)
	}
	bad := cellDir(t, nil, nil, "")
	if code := run([]string{"--out", out, "gso=" + good, "rio=" + bad}, &stdout, &stderr); code != 1 {
		t.Fatalf("a failed cell exited %d", code)
	}
}

func TestBadUsage(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := run([]string{"--out", t.TempDir(), "gso"}, &stdout, &stderr); code != 2 {
		t.Fatalf("exit %d", code)
	}
	if code := run([]string{"gso=x"}, &stdout, &stderr); code != 2 {
		t.Fatalf("no --out exit %d", code)
	}
}
