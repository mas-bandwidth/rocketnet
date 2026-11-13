// Unit tests of the cross-version check's grading and plan: no sockets, no
// processes, no clock. Run with `go test xver.go xver_test.go` from this
// directory (xver.c shares it, so the files are named).
package main

import (
	"os"
	"strings"
	"testing"
)

// The fixture lines below are xver.c's own formats, filled in; the formats
// they copy are checked against xver.c by TestFixturesAreXverFormats, so a
// fixture cannot drift from what the harness prints.
const (
	fixtureStarted         = "SRV started 127.0.0.1:1 transport=%s max_message_bytes=262144"
	fixtureVerify          = "SRV verify session=1 ok receipts=complete m5=300+0/300 m6=300/300 m7=300/300 rx m0=300/300 m1=300/300 m2=300/300 m3=300/300 m4=300/300(last) m5=300/300 m6=300/300 m7=300/300  stats sent=10 received=10 rtt=0.10 loss=0.000"
	fixtureVerifyFail      = "SRV FAIL verify session=1 receipts=complete fin_sent=1 m5=300+0/300 m6=300/300 m7=300/300 rx m0=300/300 m1=300/300 m2=299/300 m3=300/300 m4=300/300(last) m5=300/300 m6=300/300 m7=300/300 "
	fixtureReceiptsOpen    = "SRV FAIL verify session=1 receipts=open fin_sent=1 m5=300+0/300 m6=0/300 m7=300/300 rx m0=300/300 m1=300/300 m2=300/300 m3=300/300 m4=300/300(last) m5=300/300 m6=300/300 m7=300/300 "
	fixtureEnd             = "SRV end slot=0 session=1 reason=%s(1) verdict_sent=1 t=1.000"
	fixtureExit            = "SRV exit fails=0"
	fixtureClientConnect   = "CLI connected session=1 attempts=1 after=0.010s transport=%s"
	fixtureClientVerify    = "CLI verify basic ok in 1.00s refused_starts=0 receipts m5=300+0/300 m6=300/300 m7=300/300 rx m0=300/300  stats sent=10 received=10 resent=0 rtt=0.10 loss=0.000"
	fixtureClientFail      = "CLI FAIL basic verify server_ok=0 client_bad=1 rx m0=300/300 "
	fixtureClientExit      = "CLI exit fails=0"
	fixtureServerTransport = "portable"
)

func fill(format, value string) string {
	return strings.Replace(format, "%s", value, 1)
}

func passingServerOn(transport string, ends []string, verifies int) []string {
	lines := []string{"SRV key 00", fill(fixtureStarted, transport)}
	for i := 0; i < verifies; i++ {
		lines = append(lines, fixtureVerify)
	}
	for _, e := range ends {
		lines = append(lines, fill(fixtureEnd, e))
	}
	return append(lines, fixtureExit)
}

func passingServer(ends []string, verifies int) []string {
	return passingServerOn(fixtureServerTransport, ends, verifies)
}

func passingClient() []string {
	return []string{fill(fixtureClientConnect, "portable"), fixtureClientVerify, fixtureClientExit}
}

// pick takes alternative n (or the last) of every expected entry
func pick(want []string, n int) []string {
	var out []string
	for _, w := range want {
		alternatives := strings.Split(w, "|")
		out = append(out, alternatives[min(n, len(alternatives)-1)])
	}
	return out
}

// the printf format in xver.c that each fixture line above was filled from,
// up to the first conversion the fixture fills differently
func TestFixturesAreXverFormats(t *testing.T) {
	source, err := os.ReadFile("xver.c")
	if err != nil {
		t.Fatal(err)
	}
	text := string(source)
	for _, format := range []string{
		`"SRV key "`,
		`"\nSRV started %s transport=%s max_message_bytes=%d\n"`,
		`"SRV verify session=%x ok receipts=complete m5=%u+%u/%u m6=%u/%u m7=%u/%u rx %s stats sent=%llu received=%llu rtt=%.2f loss=%.3f\n"`,
		`"SRV FAIL verify session=%x receipts=%s fin_sent=%d m5=%u+%u/%u m6=%u/%u m7=%u/%u rx %s\n"`,
		`"SRV end slot=%d session=%x reason=%s(%d) verdict_sent=%d t=%.3f\n"`,
		`"SRV exit fails=%llu\n"`,
		`"CLI connected session=%x attempts=%d after=%.3fs transport=%s\n"`,
		`"CLI verify %s ok in %.2fs refused_starts=%llu receipts m5=%u+%u/%u m6=%u/%u m7=%u/%u rx %s stats sent=%llu received=%llu resent=%llu rtt=%.2f loss=%.3f\n"`,
		`"CLI FAIL %s verify server_ok=%u client_bad=%u rx %s\n"`,
		`"CLI exit fails=%llu\n"`,
		`"CLI ready-to-die\n"`,
	} {
		if !strings.Contains(text, format) {
			t.Errorf("xver.c no longer prints %s: update the grader and these fixtures together", format)
		}
	}
}

func TestSilentClientEndAllowsReliableTimedOutForV10xServersOnly(t *testing.T) {
	cases := map[string]string{
		"v1.0.0":  "RELIABLE_TIMED_OUT|TIMED_OUT",
		"v1.0.9":  "RELIABLE_TIMED_OUT|TIMED_OUT",
		"v1.1.0":  "TIMED_OUT",
		"v1.1.3":  "TIMED_OUT",
		"main":    "TIMED_OUT",
		"newest":  "TIMED_OUT",
		"v1.10.0": "TIMED_OUT",
	}
	for label, want := range cases {
		if got := silentClientEnd(label); got != want {
			t.Errorf("silentClientEnd(%q) = %s, want %s", label, got, want)
		}
	}
}

func TestGradePassesEveryAllowedName(t *testing.T) {
	for _, c := range allCases {
		for _, server := range []string{"v1.0.0", "main"} {
			for n := 0; n < 2; n++ {
				lines := passingServer(pick(expectedServerEnds(c, server), n), expectedVerifies(c))
				if reasons := grade(c, server, fixtureServerTransport, 0, 0, passingClient(), lines); len(reasons) != 0 {
					t.Errorf("case %s server %s alternative %d: %v", c, server, n, reasons)
				}
			}
		}
	}
}

func TestGradePassesEveryTransportItWasGranted(t *testing.T) {
	for _, transport := range []string{"portable", "gso", "io_uring"} {
		lines := passingServerOn(transport, []string{"CLIENT_DISCONNECTED"}, 1)
		if reasons := grade("basic", "main", transport, 0, 0, passingClient(), lines); len(reasons) != 0 {
			t.Errorf("transport %s: %v", transport, reasons)
		}
	}
}

// a pin the build cannot honour falls back to auto with only a warning: the
// cell must not pass under the label it pinned
func TestGradeRefusesAnotherServerTransport(t *testing.T) {
	for _, c := range []struct{ pinned, granted string }{
		{"io_uring", "gso"},
		{"io_uring", "portable"},
		{"gso", "portable"},
		{"portable", "gso"},
	} {
		lines := passingServerOn(c.granted, []string{"CLIENT_DISCONNECTED"}, 1)
		reasons := grade("basic", "main", c.pinned, 0, 0, passingClient(), lines)
		if len(reasons) != 1 || !strings.Contains(reasons[0], "server was granted transport ["+c.granted+"]") {
			t.Errorf("pinned %s granted %s: reasons = %v", c.pinned, c.granted, reasons)
		}
	}
}

func TestGradeRefusesAServerThatNamesNoTransport(t *testing.T) {
	var lines []string
	for _, line := range passingServer([]string{"CLIENT_DISCONNECTED"}, 1) {
		if !strings.HasPrefix(line, "SRV started ") {
			lines = append(lines, line)
		}
	}
	reasons := grade("basic", "main", "gso", 0, 0, passingClient(), lines)
	if len(reasons) != 1 || !strings.Contains(reasons[0], "server was granted transport []") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestGradeRefusesAClientNotOnPortable(t *testing.T) {
	client := []string{fill(fixtureClientConnect, "portable"), fill(fixtureClientConnect, "gso"), fixtureClientExit}
	reasons := grade("basic", "main", "portable", 0, 0, client, passingServer([]string{"CLIENT_DISCONNECTED"}, 1))
	if len(reasons) != 1 || !strings.Contains(reasons[0], `client was granted transport "gso"`) {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestGradeRefusesReliableTimedOutFromNewerServer(t *testing.T) {
	for _, server := range []string{"v1.1.0", "main", "newest"} {
		lines := passingServer([]string{"RELIABLE_TIMED_OUT", "CLIENT_DISCONNECTED"}, 1)
		reasons := grade("die", server, fixtureServerTransport, 0, 0, passingClient(), lines)
		if len(reasons) != 1 || !strings.Contains(reasons[0], "server end reasons") {
			t.Fatalf("server %s: reasons = %v", server, reasons)
		}
	}
}

func TestGradeRefusesOtherEndsFromV10xServer(t *testing.T) {
	for _, ends := range [][]string{
		{"MESSAGE_NOT_RECEIVED", "CLIENT_DISCONNECTED"},
		{"RELIABLE_TIMED_OUT"},
		{"RELIABLE_TIMED_OUT", "RELIABLE_TIMED_OUT"},
	} {
		lines := passingServer(ends, 1)
		if reasons := grade("die", "v1.0.0", fixtureServerTransport, 0, 0, passingClient(), lines); len(reasons) != 1 {
			t.Errorf("ends %v: reasons = %v", ends, reasons)
		}
	}
	// lifecycle D is the server's own silence: TIMED_OUT on every version
	lines := passingServer([]string{"CLIENT_DISCONNECTED", "SERVER_DISCONNECTED", "TIMED_OUT", "RELIABLE_TIMED_OUT", "CLIENT_DISCONNECTED"}, 5)
	if reasons := grade("lifecycle", "v1.0.0", fixtureServerTransport, 0, 0, passingClient(), lines); len(reasons) != 1 {
		t.Errorf("lifecycle D: reasons = %v", reasons)
	}
}

// the real lines xver.c prints on a failure, each named in the reasons
func TestGradeNamesEveryFault(t *testing.T) {
	lines := []string{"SRV key 00", fill(fixtureStarted, fixtureServerTransport), fixtureVerifyFail, fill(fixtureEnd, "MESSAGE_NOT_RECEIVED"), "SRV exit fails=1"}
	client := []string{fill(fixtureClientConnect, "portable"), fixtureClientFail, "CLI exit fails=1"}
	reasons := grade("basic", "main", fixtureServerTransport, 1, 1, client, lines)
	want := []string{"client exited 1", "server exited 1", "client: CLI FAIL basic verify", "server: SRV FAIL verify", "server end reasons", "verified 0 sessions, want 1", "SRV exit fails=0"}
	joined := strings.Join(reasons, "\n")
	for _, w := range want {
		if !strings.Contains(joined, w) {
			t.Errorf("no reason mentions %q in %v", w, reasons)
		}
	}
	if len(reasons) != len(want) {
		t.Errorf("%d reasons, want %d: %v", len(reasons), len(want), reasons)
	}
}

// the server's verdict covers its own sends' receipts: a server whose
// receipts never complete fails its verdict, and a verify line that does not
// say receipts=complete is not a verified session
func TestGradeRefusesServerReceiptsOpen(t *testing.T) {
	lines := []string{"SRV key 00", fill(fixtureStarted, fixtureServerTransport), fixtureReceiptsOpen, fill(fixtureEnd, "CLIENT_DISCONNECTED"), "SRV exit fails=1"}
	reasons := grade("basic", "main", fixtureServerTransport, 0, 1, passingClient(), lines)
	joined := strings.Join(reasons, "\n")
	for _, w := range []string{"server exited 1", "server: SRV FAIL verify session=1 receipts=open", "verified 0 sessions, want 1", "SRV exit fails=0"} {
		if !strings.Contains(joined, w) {
			t.Errorf("no reason mentions %q in %v", w, reasons)
		}
	}
	unreceipted := strings.Replace(fixtureVerify, " ok receipts=complete ", " ok ", 1)
	lines = []string{"SRV key 00", fill(fixtureStarted, fixtureServerTransport), unreceipted, fill(fixtureEnd, "CLIENT_DISCONNECTED"), fixtureExit}
	reasons = grade("basic", "main", fixtureServerTransport, 0, 0, passingClient(), lines)
	if len(reasons) != 1 || !strings.Contains(reasons[0], "without its own receipts complete") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestGradeCountsVerifiedSessions(t *testing.T) {
	lines := passingServer(expectedServerEnds("lifecycle", "main"), 4)
	reasons := grade("lifecycle", "main", fixtureServerTransport, 0, 0, passingClient(), lines)
	if len(reasons) != 1 || !strings.Contains(reasons[0], "verified 4 sessions, want 5") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestServerEndsReadsReasonNames(t *testing.T) {
	got := serverEnds([]string{fill(fixtureEnd, "TIMED_OUT"), "CLI end reason=X(1)", fill(fixtureEnd, "CLIENT_DISCONNECTED")})
	if strings.Join(got, ",") != "TIMED_OUT,CLIENT_DISCONNECTED" {
		t.Fatalf("got %v", got)
	}
}

// a run that would check nothing, or never send the largest message, is refused
func TestCheckRunInputsRefusesRunsThatCheckLess(t *testing.T) {
	good := [][2]any{{151, []string{"basic"}}, {300, allCases}, {8192, []string{"die"}}}
	for _, g := range good {
		if err := checkRunInputs(g[0].(int), g[1].([]string)); err != nil {
			t.Errorf("count %d cases %v: %v", g[0], g[1], err)
		}
	}
	bad := []struct {
		count int
		cases []string
		want  string
	}{
		{0, []string{"basic"}, "-count 0"},
		{150, []string{"basic"}, "-count 150"},
		{8193, []string{"basic"}, "-count 8193"},
		{300, nil, "names no case"},
		{300, splitList(" , "), "names no case"},
		{300, []string{"basic", "nope"}, `unknown case "nope"`},
	}
	for _, b := range bad {
		err := checkRunInputs(b.count, b.cases)
		if err == nil || !strings.Contains(err.Error(), b.want) {
			t.Errorf("count %d cases %v: err %v, want %q", b.count, b.cases, err, b.want)
		}
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
