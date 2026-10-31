// real_load_report summarises the real-load nightly: one row per server
// transport.
//
// It reads the run directories scripts/lan-proof leaves behind (server.log,
// merge.log and the lan-proof.console the workflow tees beside them) and
// writes, per cell, the server call times (p99, p999 upper bounds and maximum,
// in microseconds, from split_server_call), connects, disconnects, messages
// sent and received each way and the merger's unreliable_lost. The rows go to
// DIR/real-load.tsv, DIR/real-load.json and DIR/real-load.md (the job
// summary), and one real_load: line per cell to stdout.
//
// A cell fails when its run directory has no server receipt, when fewer than
// every client connected, on any disconnect (a timeout or a lost connection),
// when the merger's proof cell is not PASS, or when the server call timing is
// missing. The call times are reported, never gated.
//
//	go run ./tools/real_load_report --out DIR [--head SHA] [--date UTC] CELL=RUN_DIR...
//
// Exits 0 when every cell passes, 1 when any cell fails, 2 on bad usage.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

var calls = []string{"server_update", "server_receive_messages", "server_start_message",
	"server_finish_message", "server_send_packets"}

var columns = []string{"cell", "status", "clients", "connected", "disconnects", "unexpected_disconnects",
	"c2s_sent", "c2s_received", "s2c_sent", "s2c_received", "unreliable_lost",
	"sequenced_skipped", "update_p99_us", "update_p999_us", "update_max_us",
	"send_p99_us", "send_p999_us", "send_max_us", "reason"}

// Row is one cell's result. Fields holds every column; Calls holds each
// server call's timing fields for the JSON record.
type Row struct {
	Fields map[string]string
	Calls  map[string]map[string]string
}

func (r Row) MarshalJSON() ([]byte, error) {
	out := map[string]any{}
	for k, v := range r.Fields {
		out[k] = v
	}
	out["calls"] = r.Calls
	return json.Marshal(out)
}

// fields returns the key=value tokens of a log line after its prefix.
func fields(line, prefix string) map[string]string {
	out := map[string]string{}
	for _, token := range strings.Fields(strings.TrimPrefix(line, prefix)) {
		if k, v, ok := strings.Cut(token, "="); ok {
			out[k] = v
		}
	}
	return out
}

// lastLine returns the fields of the last line with prefix that contains
// extra, or nil when there is none.
func lastLine(text, prefix, extra string) map[string]string {
	var found *string
	for _, line := range strings.Split(text, "\n") {
		line := strings.TrimRight(line, "\r")
		if strings.HasPrefix(line, prefix) && strings.Contains(line, extra) {
			found = &line
		}
	}
	if found == nil {
		return nil
	}
	return fields(*found, prefix)
}

func read(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(data)
}

func isInt(value string) (int64, bool) {
	n, err := strconv.ParseInt(value, 10, 64)
	return n, err == nil
}

// Summarise builds one cell's row from its run directory.
func Summarise(cell, runDir string) Row {
	row := Row{Fields: map[string]string{}, Calls: map[string]map[string]string{}}
	for _, c := range columns {
		row.Fields[c] = ""
	}
	row.Fields["cell"] = cell
	server := read(filepath.Join(runDir, "server.log"))
	merge := read(filepath.Join(runDir, "merge.log"))
	console := read(filepath.Join(runDir, "lan-proof.console"))
	var reasons []string

	receipt := lastLine(server, "split_receipt: ", "role=server")
	proof := lastLine(server, "split_proof: ", "role=server")
	proofCell := lastLine(merge, "split_proof_cell: ", "")
	callFields := map[string]map[string]string{}
	for _, line := range strings.Split(server, "\n") {
		if strings.HasPrefix(line, "split_server_call: ") {
			f := fields(strings.TrimRight(line, "\r"), "split_server_call: ")
			callFields[f["call"]] = f
		}
	}

	if receipt == nil {
		refusal := ""
		for _, line := range strings.Split(console, "\n") {
			if strings.HasPrefix(line, "lan-proof: ") {
				refusal = strings.TrimRight(strings.TrimPrefix(line, "lan-proof: "), "\r")
			}
		}
		if refusal != "" {
			reasons = append(reasons, "no server receipt: "+refusal)
		} else {
			reasons = append(reasons, "no server receipt")
		}
	} else {
		row.Fields["clients"] = receipt["total"]
		row.Fields["connected"] = receipt["connected"]
		row.Fields["unexpected_disconnects"] = receipt["unexpected_disconnects"]
		row.Fields["c2s_sent"] = receipt["c2s_accepted"]
		row.Fields["c2s_received"] = receipt["c2s_received"]
		row.Fields["s2c_sent"] = receipt["s2c_accepted"]
		row.Fields["s2c_received"] = receipt["s2c_received"]
		total, totalOK := isInt(receipt["total"])
		connected, connectedOK := isInt(receipt["connected"])
		if !totalOK || !connectedOK || connected != total {
			reasons = append(reasons, fmt.Sprintf("connected %s of %s", receipt["connected"], receipt["total"]))
		}
		if n, ok := isInt(receipt["unexpected_disconnects"]); !ok || n != 0 {
			reasons = append(reasons, "unexpected_disconnects="+receipt["unexpected_disconnects"])
		}
	}
	if proof != nil {
		row.Fields["disconnects"] = proof["disconnects"]
		if n, ok := isInt(proof["disconnects"]); !ok || n != 0 {
			reasons = append(reasons, "disconnects="+proof["disconnects"])
		}
	} else if receipt != nil {
		reasons = append(reasons, "no server proof line")
	}
	if proofCell != nil {
		row.Fields["unreliable_lost"] = proofCell["unreliable_lost"]
		row.Fields["sequenced_skipped"] = proofCell["sequenced_skipped"]
		if proofCell["verdict"] != "PASS" {
			reason := proofCell["reason"]
			if reason == "" {
				reason = "see merge.log"
			}
			reasons = append(reasons, fmt.Sprintf("proof cell %s: %s", proofCell["verdict"], reason))
		}
	} else if receipt != nil {
		reasons = append(reasons, "no merger proof cell")
	}

	timed := 0
	for _, pair := range [][2]string{{"server_update", "update"}, {"server_send_packets", "send"}} {
		f, ok := callFields[pair[0]]
		if !ok {
			continue
		}
		row.Fields[pair[1]+"_p99_us"] = f["p99_upper_us"]
		row.Fields[pair[1]+"_p999_us"] = f["p999_upper_us"]
		row.Fields[pair[1]+"_max_us"] = f["max_us"]
		_, p99OK := isInt(f["p99_upper_us"])
		_, p999OK := isInt(f["p999_upper_us"])
		if p99OK && p999OK {
			timed++
		}
	}
	if receipt != nil && timed != 2 {
		reasons = append(reasons, "server call timing missing")
	}
	if len(reasons) > 0 {
		row.Fields["status"] = "FAIL"
		row.Fields["reason"] = strings.Join(reasons, "; ")
	} else {
		row.Fields["status"] = "PASS"
		row.Fields["reason"] = "-"
	}
	for _, call := range calls {
		f, ok := callFields[call]
		if !ok {
			continue
		}
		kept := map[string]string{}
		for _, k := range []string{"count", "p50_upper_us", "p99_upper_us", "p999_upper_us", "max_us", "overbudget_count"} {
			kept[k] = f[k]
		}
		row.Calls[call] = kept
	}
	return row
}

func or(value string) string {
	if value == "" {
		return "-"
	}
	return value
}

// Markdown renders the job-summary table.
func Markdown(rows []Row, head string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "Real load, 1000 clients over the LAN, head `%s`. Server call times are upper bounds in microseconds.\n\n", head)
	b.WriteString("| cell | status | connected | disconnects | c2s sent/received | s2c sent/received | unreliable lost | update p99 / p999 / max | send p99 / p999 / max | reason |\n")
	b.WriteString("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
	for _, r := range rows {
		f := r.Fields
		fmt.Fprintf(&b, "| %s | %s | %s/%s | %s | %s/%s | %s/%s | %s | %s / %s / %s | %s / %s / %s | %s |\n",
			f["cell"], f["status"], or(f["connected"]), or(f["clients"]), or(f["disconnects"]),
			or(f["c2s_sent"]), or(f["c2s_received"]), or(f["s2c_sent"]), or(f["s2c_received"]),
			or(f["unreliable_lost"]), or(f["update_p99_us"]), or(f["update_p999_us"]), or(f["update_max_us"]),
			or(f["send_p99_us"]), or(f["send_p999_us"]), or(f["send_max_us"]), f["reason"])
	}
	return b.String()
}

func run(args []string, stdout, stderr io.Writer) int {
	flags := flag.NewFlagSet("real_load_report", flag.ContinueOnError)
	flags.SetOutput(stderr)
	out := flags.String("out", "", "directory for real-load.tsv, .json and .md")
	head := flags.String("head", "unknown", "the commit the cells ran")
	date := flags.String("date", "unknown", "the UTC time of the night")
	if err := flags.Parse(args); err != nil {
		return 2
	}
	if *out == "" || flags.NArg() == 0 {
		fmt.Fprintln(stderr, "usage: real_load_report --out DIR [--head SHA] [--date UTC] CELL=RUN_DIR...")
		return 2
	}
	var rows []Row
	for _, item := range flags.Args() {
		cell, dir, ok := strings.Cut(item, "=")
		if !ok || cell == "" {
			fmt.Fprintf(stderr, "real_load_report: %q is not CELL=RUN_DIR\n", item)
			return 2
		}
		rows = append(rows, Summarise(cell, dir))
	}
	if err := os.MkdirAll(*out, 0o755); err != nil {
		fmt.Fprintln(stderr, "real_load_report:", err)
		return 2
	}
	var tsv strings.Builder
	tsv.WriteString("date\thead\t" + strings.Join(columns, "\t") + "\n")
	for _, r := range rows {
		values := make([]string, len(columns))
		for i, c := range columns {
			values[i] = r.Fields[c]
		}
		tsv.WriteString(*date + "\t" + *head + "\t" + strings.Join(values, "\t") + "\n")
	}
	record, err := json.MarshalIndent(map[string]any{"schema": "real_load_v1", "date": *date, "head": *head, "cells": rows}, "", "  ")
	if err != nil {
		fmt.Fprintln(stderr, "real_load_report:", err)
		return 2
	}
	text := Markdown(rows, *head)
	for name, body := range map[string]string{"real-load.tsv": tsv.String(), "real-load.json": string(record) + "\n", "real-load.md": text} {
		if err := os.WriteFile(filepath.Join(*out, name), []byte(body), 0o644); err != nil {
			fmt.Fprintln(stderr, "real_load_report:", err)
			return 2
		}
	}
	io.WriteString(stdout, text)
	failed := false
	for _, r := range rows {
		f := r.Fields
		na := func(v string) string {
			if v == "" {
				return "na"
			}
			return v
		}
		fmt.Fprintf(stdout, "real_load: cell=%s status=%s connected=%s disconnects=%s update_p99_us=%s update_p999_us=%s send_p99_us=%s send_p999_us=%s reason=%s\n",
			f["cell"], f["status"], na(f["connected"]), na(f["disconnects"]), na(f["update_p99_us"]),
			na(f["update_p999_us"]), na(f["send_p99_us"]), na(f["send_p999_us"]), f["reason"])
		if f["status"] != "PASS" {
			failed = true
		}
	}
	if failed {
		return 1
	}
	return 0
}

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}
