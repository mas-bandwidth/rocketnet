package main

import (
	"context"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/mas-bandwidth/nova-tools/pkg/tlc"
)

const tinyModel = `---- MODULE MCNightly ----
EXTENDS Naturals
VARIABLE x
Init == x = 0
Next == x' = 1 - x
Spec == Init /\ [][Next]_x
Safe == x \in {0, 1}
Broken == x = 0
EventuallyTwo == <>(x = 2)
====
`

func fixture(t *testing.T, expected, property, clause string) options {
	t.Helper()
	jar := os.Getenv("TLC_NIGHTLY_TEST_JAR")
	if jar == "" {
		t.Fatal("TLC_NIGHTLY_TEST_JAR must name the pinned real JAR")
	}
	root := t.TempDir()
	if evidence := os.Getenv("TLC_NIGHTLY_TEST_EVIDENCE"); evidence != "" {
		if err := os.MkdirAll(evidence, 0o755); err != nil {
			t.Fatal(err)
		}
		var err error
		root, err = os.MkdirTemp(evidence, "control-")
		if err != nil {
			t.Fatal(err)
		}
		t.Logf("control evidence: %s", root)
	}
	dir := filepath.Join(root, "tla")
	if err := os.Mkdir(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	write := func(name, body string) {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("MCNightly.tla", tinyModel)
	write("MCNightly.cfg", "SPECIFICATION Spec\n"+clause+"\n")
	write("CASES.tsv", "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"+
		"MCNightly.cfg\tMCNightly.tla\t"+expected+"\t"+property+"\tcheck\tnightly-control\trequired\t-\n")
	return options{root: root, out: filepath.Join(root, "result"), jar: jar,
		java: os.Getenv("TLC_NIGHTLY_TEST_JAVA"), adapter: strings.Repeat("a", 64),
		group: "nightly-control", shards: 1}
}

func TestRealTLCOutcomes(t *testing.T) {
	for _, test := range []struct {
		name, expected, property, clause string
		exit, workers                    int
	}{
		{"positive", "pass", "-", "INVARIANT Safe", 0, 2},
		{"invariant", "invariant", "Broken", "INVARIANT Broken", 12, 1},
		{"temporal", "temporal", "EventuallyTwo", "PROPERTY EventuallyTwo", 13, 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			o := fixture(t, test.expected, test.property, test.clause)
			rec, err := runCase(context.Background(), o, hooks{})
			if err != nil || rec.Result != "PASS" || rec.Exit != test.exit || rec.Workers != test.workers {
				raw, _ := os.ReadFile(filepath.Join(o.out, "tlc.log"))
				t.Fatalf("result=%+v err=%v\n%s", rec, err, raw)
			}
			if rec.Mode != "nightly-no-timeout" || rec.ElapsedCap != nil || !rec.InputsCurrent || rec.InputSHA == "" || len(rec.Inputs) == 0 {
				t.Fatalf("incomplete provenance: %+v", rec)
			}
			if _, err := os.Stat(filepath.Join(o.out, "RUNS.tsv")); !os.IsNotExist(err) {
				t.Fatal("nightly must never produce bounded RUNS.tsv")
			}
			t.Logf("exit=%d generated=%s distinct=%s workers=%d elapsed=%.3f", rec.Exit, rec.Generated, rec.Distinct, rec.Workers, rec.Seconds)
		})
	}
}

func TestRealTLCWrongOracleAndParseFailure(t *testing.T) {
	for _, brokenSource := range []bool{false, true} {
		o := fixture(t, "pass", "-", "INVARIANT Broken")
		if brokenSource {
			if err := os.WriteFile(filepath.Join(o.root, "tla", "MCNightly.tla"), []byte("---- MODULE MCNightly ----\ninvalid % syntax\n====\n"), 0o644); err != nil {
				t.Fatal(err)
			}
		}
		rec, err := runCase(context.Background(), o, hooks{})
		if err != nil || rec.Result != "FAIL" || rec.Exit == 0 {
			t.Fatalf("wrong oracle or parse failure accepted: %+v %v", rec, err)
		}
		t.Logf("parseFailure=%t exit=%d reason=%s", brokenSource, rec.Exit, rec.Reason)
	}
}

func TestNonProofExitsCannotPass(t *testing.T) {
	for _, code := range []int{1, 124, 127, 137} {
		o := fixture(t, "pass", "-", "INVARIANT Safe")
		rec, err := runCase(context.Background(), o, hooks{execute: func(ctx context.Context, r tlc.Run, log string) int {
			if _, deadline := ctx.Deadline(); deadline {
				t.Fatal("nightly context has a deadline")
			}
			for _, required := range []string{"-Xmx2g", "-XX:ActiveProcessorCount=2", "-XX:+UseParallelGC", "-lncheck", "final"} {
				if !slices.Contains(r.Args(), required) {
					t.Fatalf("lost engine argument %s", required)
				}
			}
			if err := os.WriteFile(log, []byte("Model checking completed. No error has been found.\n3 states generated, 2 distinct states found\n"), 0o644); err != nil {
				t.Fatal(err)
			}
			return code
		}})
		if err != nil || rec.Result != "FAIL" || rec.Exit != code {
			t.Fatalf("non-proof exit accepted: %+v %v", rec, err)
		}
	}
}

func TestSourceMismatchRefusesVerdict(t *testing.T) {
	for _, beforeCopy := range []bool{true, false} {
		o := fixture(t, "pass", "-", "INVARIANT Safe")
		mutate := func() {
			path := filepath.Join(o.root, "tla", "MCNightly.tla")
			if err := os.WriteFile(path, []byte(tinyModel+"\n\\* source changed\n"), 0o644); err != nil {
				t.Fatal(err)
			}
		}
		h := hooks{}
		if beforeCopy {
			h.beforeCopy = mutate
			h.execute = func(context.Context, tlc.Run, string) int {
				t.Fatal("changed copy must refuse before execution")
				return 0
			}
		} else {
			h.execute = func(ctx context.Context, r tlc.Run, log string) int {
				code := tlc.Execute(ctx, r, log)
				mutate()
				return code
			}
		}
		rec, err := runCase(context.Background(), o, h)
		if err == nil || rec.Result != "FAIL" || rec.InputsCurrent || !strings.Contains(err.Error(), "fingerprint differs") {
			t.Fatalf("source mismatch accepted: %+v %v", rec, err)
		}
	}
}

func TestCancellationIsIncomplete(t *testing.T) {
	o := fixture(t, "pass", "-", "INVARIANT Safe")
	ctx, cancel := context.WithCancel(context.Background())
	rec, err := runCase(ctx, o, hooks{execute: func(ctx context.Context, r tlc.Run, log string) int {
		cancel()
		return tlc.Execute(ctx, r, log)
	}})
	if err != nil || rec.Result != "FAIL" || !rec.Interrupted || rec.InputsCurrent != true || rec.Exit != 124 {
		t.Fatalf("cancellation accepted: %+v %v", rec, err)
	}
}

func TestEvidenceDirectoryCannotBeReused(t *testing.T) {
	o := fixture(t, "pass", "-", "INVARIANT Safe")
	if err := os.Mkdir(o.out, 0o755); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(o.out, "prior.log")
	if err := os.WriteFile(marker, []byte("keep"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := runCase(context.Background(), o, hooks{}); err == nil {
		t.Fatal("reused evidence directory accepted")
	}
	if raw, _ := os.ReadFile(marker); string(raw) != "keep" {
		t.Fatal("prior evidence changed")
	}
}
