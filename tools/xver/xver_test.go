// Unit tests of the cross-version check's grading and plan: no sockets, no
// processes, no clock. Run with `go test xver.go xver_test.go` from this
// directory (xver.c shares it, so the files are named).
package main

import (
	"strings"
	"testing"
)

func passingServer(ends []string, verifies int) []string {
	lines := []string{"SRV key 00", "SRV started addr=127.0.0.1:1 transport=portable"}
	for i := 0; i < verifies; i++ {
		lines = append(lines, "SRV verify slot=0 ok")
	}
	for _, e := range ends {
		lines = append(lines, "SRV end slot=0 reason="+e+"(1)")
	}
	return append(lines, "SRV exit fails=0")
}

func TestSilentClientEndIsReliableTimedOutForV100ServersOnly(t *testing.T) {
	cases := map[string]string{
		"v1.0.0":  "RELIABLE_TIMED_OUT",
		"v1.0.9":  "RELIABLE_TIMED_OUT",
		"v1.1.0":  "TIMED_OUT",
		"v1.1.3":  "TIMED_OUT",
		"main":    "TIMED_OUT",
		"v1.10.0": "TIMED_OUT",
	}
	for label, want := range cases {
		if got := silentClientEnd(label); got != want {
			t.Errorf("silentClientEnd(%q) = %s, want %s", label, got, want)
		}
	}
}

func TestGradePassesExactExpectation(t *testing.T) {
	for _, c := range allCases {
		for _, server := range []string{"v1.0.0", "main"} {
			lines := passingServer(expectedServerEnds(c, server), expectedVerifies(c))
			if reasons := grade(c, server, 0, 0, []string{"CLI done"}, lines); len(reasons) != 0 {
				t.Errorf("case %s server %s: %v", c, server, reasons)
			}
		}
	}
}

func TestGradeRefusesReliableTimedOutFromNewerServer(t *testing.T) {
	lines := passingServer([]string{"RELIABLE_TIMED_OUT", "CLIENT_DISCONNECTED"}, 1)
	reasons := grade("die", "main", 0, 0, nil, lines)
	if len(reasons) != 1 || !strings.Contains(reasons[0], "server end reasons") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestGradeRefusesTimedOutFromV100Server(t *testing.T) {
	lines := passingServer([]string{"TIMED_OUT", "CLIENT_DISCONNECTED"}, 1)
	if reasons := grade("die", "v1.0.0", 0, 0, nil, lines); len(reasons) != 1 {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestGradeNamesEveryFault(t *testing.T) {
	lines := []string{"SRV key 00", "SRV verify slot=0 FAIL bad payload", "SRV end slot=0 reason=MESSAGE_NOT_RECEIVED(9)", "SRV exit fails=1"}
	reasons := grade("basic", "main", 1, 1, []string{"CLI recv mode=2 FAIL count"}, lines)
	want := []string{"client exited 1", "server exited 1", "client: CLI recv", "server: SRV verify", "server end reasons", "SRV exit fails=0"}
	joined := strings.Join(reasons, "\n")
	for _, w := range want {
		if !strings.Contains(joined, w) {
			t.Errorf("no reason mentions %q in %v", w, reasons)
		}
	}
}

func TestGradeCountsVerifiedSessions(t *testing.T) {
	lines := passingServer(expectedServerEnds("lifecycle", "main"), 4)
	reasons := grade("lifecycle", "main", 0, 0, nil, lines)
	if len(reasons) != 1 || !strings.Contains(reasons[0], "verified 4 sessions, want 5") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestServerEndsReadsReasonNames(t *testing.T) {
	got := serverEnds([]string{"SRV end slot=0 reason=TIMED_OUT(3) after=10s", "CLI end reason=X(1)", "SRV end slot=1 reason=CLIENT_DISCONNECTED(2)"})
	if strings.Join(got, ",") != "TIMED_OUT,CLIENT_DISCONNECTED" {
		t.Fatalf("got %v", got)
	}
}

func TestPairingsRunBothDirectionsOnPortableAndEveryPairOnKernelTransports(t *testing.T) {
	list := pairings([]string{"main", "v1.0.0", "v1.1.3"}, "main", []string{"gso"}, []string{"basic"})
	seen := map[pairing]bool{}
	for _, p := range list {
		if seen[p] {
			t.Errorf("duplicate %v", p)
		}
		seen[p] = true
	}
	must := []pairing{
		{"main", "main", "portable", "basic"},
		{"v1.0.0", "main", "portable", "basic"},
		{"main", "v1.0.0", "portable", "basic"},
		{"v1.1.3", "main", "portable", "basic"},
		{"main", "v1.1.3", "portable", "basic"},
		{"v1.0.0", "v1.1.3", "gso", "basic"},
		{"v1.1.3", "v1.0.0", "gso", "basic"},
	}
	for _, p := range must {
		if !seen[p] {
			t.Errorf("missing %v", p)
		}
	}
	if len(list) != 5+9 {
		t.Errorf("%d runs, want 14", len(list))
	}
}
