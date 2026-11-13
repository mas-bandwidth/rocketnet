// xver is the cross-version check: it runs a client built against one
// rocketnet release against a server built against another, over real
// loopback UDP, and grades both ends. The ends are tools/xver/xver.c, built
// once per source tree by `xver build` (tools/xver/CMakeLists.txt adds the
// tree as a subdirectory, so each end is that release's library end to end).
//
//	go run xver.go build -tree <source tree> -build <build dir>
//	go run xver.go run -bin <label>=<xver binary> ... -new <label>
//	                   [-transports gso,io_uring] [-parallel 4] [-count 300] [-out <dir>]
//
// run takes one -bin per version and names the newest with -new. On the
// portable server transport it runs every case for new x new and, for each
// other version O, O client x new server and new client x O server. For each
// transport named by -transports (the Linux server transports gso and
// io_uring), every client version against every server version. The client
// always runs portable.
//
// The cases are in xver.c's header. A run passes only when both ends exit 0,
// the server says (SRV started) it was granted the transport the run pinned
// and the client says portable on every connect (a pin the build cannot
// honour falls back to the library's auto order with only a warning),
// neither prints a line holding " FAIL ", the server verified every session
// the case has, each with its own sends' receipts complete, and the server's
// end reasons are exactly the case's list (expectedServerEnds). Every failure
// prints one line
//
//	XVER FAIL case=<case> client=<label> server=<label> transport=<t>: <reason>
//
// and run exits 1 when any run did not pass, so a red nightly names each one.
// ROCKETNET_NIGHTLY_NO_TIMEOUT=1 removes the tool's own deadline on each run
// (default 15 minutes); a case's own waits are bounded in xver.c.
//
// Standard library only. A functional check over sockets and the wall clock,
// not a unit test; xver_test.go tests the grading, which uses neither.
package main

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
)

// the cases, in the order they run; xver.c implements each one
var allCases = []string{"basic", "weather", "outage-c2s", "outage-s2c", "outage-both", "die", "lifecycle"}

const defaultRunDeadline = 15 * time.Minute

// silentClientEnd is the reason a server of this version gives a slot whose
// client went silent (killed, or every client-to-server packet lost) past the
// connect token's timeout T. Alternatives are joined by "|".
//
// A v1.0.x server may say RELIABLE_TIMED_OUT instead of TIMED_OUT: its default
// reliable_message_timeout_ms is 10000, the same length as T, so two 10 s
// clocks race, the reliable clock of the oldest message the server sent and
// the silence clock of the last packet it received; whichever started first
// ends the slot. The v1.1.0 release notes changed that default to 0 and say
// "A v1.0.x end still ends a connection by its reliable timeout". Either name
// is that documented v1.0.x behaviour, so a v1.0.x server is allowed either,
// and no other server is: every server after v1.0.x must say TIMED_OUT.
// Measured 2026-11-11 over every pair with a v1.0.0 server: RELIABLE_TIMED_OUT
// in 21 of 24 silent-client ends, TIMED_OUT in 3, all 3 on the io_uring server.
func silentClientEnd(serverLabel string) string {
	if strings.HasPrefix(serverLabel, "v1.0.") {
		return "RELIABLE_TIMED_OUT|TIMED_OUT"
	}
	return "TIMED_OUT"
}

// endsMatch reports whether the reasons the server gave are the expected list,
// position by position, where an expected entry may name alternatives with "|".
func endsMatch(got, want []string) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range want {
		ok := false
		for _, alternative := range strings.Split(want[i], "|") {
			ok = ok || got[i] == alternative
		}
		if !ok {
			return false
		}
	}
	return true
}

// expectedServerEnds is the exact list of reasons, in order, the server must
// print for the case's connections.
func expectedServerEnds(caseName, serverLabel string) []string {
	switch caseName {
	case "lifecycle":
		// A client disconnects; B the server disconnects the client; C the
		// client goes silent; D the server goes silent, so the client times out
		// and then the server times out the slot; E client disconnects
		return []string{"CLIENT_DISCONNECTED", "SERVER_DISCONNECTED", silentClientEnd(serverLabel), "TIMED_OUT", "CLIENT_DISCONNECTED"}
	case "die":
		// the killed client, then the client that took its slot
		return []string{silentClientEnd(serverLabel), "CLIENT_DISCONNECTED"}
	default:
		return []string{"CLIENT_DISCONNECTED"}
	}
}

// expectedVerifies is how many sessions the server must verify in the case.
func expectedVerifies(caseName string) int {
	switch caseName {
	case "lifecycle":
		return 5
	case "die":
		// the killed client's traffic is cut short; only the second session ends
		return 1
	default:
		return 1
	}
}

func hasFailLine(line string) bool {
	return strings.Contains(line, " FAIL ") || strings.HasSuffix(line, " FAIL")
}

// serverEnds reads the reason names from the server's "SRV end" lines.
func serverEnds(lines []string) []string {
	var ends []string
	for _, line := range lines {
		if !strings.HasPrefix(line, "SRV end ") {
			continue
		}
		for _, field := range strings.Fields(line) {
			if reason, ok := strings.CutPrefix(field, "reason="); ok {
				if i := strings.Index(reason, "("); i >= 0 {
					reason = reason[:i]
				}
				ends = append(ends, reason)
			}
		}
	}
	return ends
}

func countPrefix(lines []string, prefix string) int {
	n := 0
	for _, line := range lines {
		if strings.HasPrefix(line, prefix) {
			n++
		}
	}
	return n
}

// fieldValue returns the value of the first "<name>=<value>" field of line,
// and whether there was one.
func fieldValue(line, name string) (string, bool) {
	for _, field := range strings.Fields(line) {
		if value, ok := strings.CutPrefix(field, name+"="); ok {
			return value, true
		}
	}
	return "", false
}

// grantedTransports returns the transport= value of every line that starts
// with prefix ("" for such a line without one). The library falls back to its
// own auto order, with only a warning, when the pinned transport is not
// available, so the pin is checked against what each end says it was granted.
func grantedTransports(lines []string, prefix string) []string {
	var out []string
	for _, line := range lines {
		if strings.HasPrefix(line, prefix) {
			value, _ := fieldValue(line, "transport")
			out = append(out, value)
		}
	}
	return out
}

// grade returns why a run did not pass, or nothing when it passed. transport
// is the server transport the run pinned; the client always pins portable.
func grade(caseName, serverLabel, transport string, clientExit, serverExit int, clientLines, serverLines []string) []string {
	var reasons []string
	if got := grantedTransports(serverLines, "SRV started "); len(got) != 1 || got[0] != transport {
		reasons = append(reasons, fmt.Sprintf("server was granted transport %v (SRV started), want [%s]", got, transport))
	}
	for _, got := range grantedTransports(clientLines, "CLI connected ") {
		if got != "portable" {
			reasons = append(reasons, fmt.Sprintf("client was granted transport %q (CLI connected), want portable", got))
		}
	}
	if clientExit != 0 {
		reasons = append(reasons, fmt.Sprintf("client exited %d", clientExit))
	}
	if serverExit != 0 {
		reasons = append(reasons, fmt.Sprintf("server exited %d", serverExit))
	}
	for _, line := range clientLines {
		if hasFailLine(line) {
			reasons = append(reasons, "client: "+line)
		}
	}
	for _, line := range serverLines {
		if hasFailLine(line) {
			reasons = append(reasons, "server: "+line)
		}
	}
	got := serverEnds(serverLines)
	want := expectedServerEnds(caseName, serverLabel)
	if !endsMatch(got, want) {
		reasons = append(reasons, fmt.Sprintf("server end reasons %v, want %v", got, want))
	}
	if n, w := countPrefix(serverLines, "SRV verify "), expectedVerifies(caseName); n != w {
		reasons = append(reasons, fmt.Sprintf("server verified %d sessions, want %d", n, w))
	}
	// the server's verdict covers its own sends' receipts, as the client's does
	for _, line := range serverLines {
		if strings.HasPrefix(line, "SRV verify ") && !strings.Contains(line, " ok receipts=complete ") {
			reasons = append(reasons, "server verified a session without its own receipts complete: "+line)
		}
	}
	if countPrefix(serverLines, "SRV exit fails=0") != 1 {
		reasons = append(reasons, "server did not print SRV exit fails=0")
	}
	return reasons
}

// pairing is one run: a client version, a server version, a server transport and a case.
type pairing struct {
	client, server, transport, caseName string
}

// pairings lists the runs: the portable cross pairs and baseline, then every
// pair on each kernel transport.
func pairings(labels []string, newest string, transports, cases []string) []pairing {
	var out []pairing
	for _, c := range cases {
		out = append(out, pairing{newest, newest, "portable", c})
		for _, old := range labels {
			if old == newest {
				continue
			}
			out = append(out, pairing{old, newest, "portable", c}, pairing{newest, old, "portable", c})
		}
	}
	for _, t := range transports {
		for _, c := range cases {
			for _, server := range labels {
				for _, client := range labels {
					out = append(out, pairing{client, server, t, c})
				}
			}
		}
	}
	return out
}

// ---- processes ----

type lineBuffer struct {
	mu    sync.Mutex
	lines []string
}

func (b *lineBuffer) add(line string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.lines = append(b.lines, line)
}

func (b *lineBuffer) all() []string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return append([]string(nil), b.lines...)
}

type process struct {
	cmd    *exec.Cmd
	output lineBuffer
	done   chan struct{}
	exit   int
}

func startProcess(ctx context.Context, logPath string, env []string, path string, args ...string) (*process, error) {
	p := &process{done: make(chan struct{}), exit: -1}
	p.cmd = exec.CommandContext(ctx, path, args...)
	p.cmd.Env = append(os.Environ(), env...)
	reader, writer := io.Pipe()
	p.cmd.Stdout = writer
	p.cmd.Stderr = writer
	logFile, err := os.Create(logPath)
	if err != nil {
		return nil, err
	}
	if err := p.cmd.Start(); err != nil {
		logFile.Close()
		return nil, err
	}
	scanned := make(chan struct{})
	go func() {
		scanner := bufio.NewScanner(reader)
		scanner.Buffer(make([]byte, 1<<20), 1<<20)
		for scanner.Scan() {
			p.output.add(scanner.Text())
			fmt.Fprintln(logFile, scanner.Text())
		}
		close(scanned)
	}()
	go func() {
		_ = p.cmd.Wait()
		writer.Close()
		<-scanned
		logFile.Close()
		p.exit = p.cmd.ProcessState.ExitCode()
		close(p.done)
	}()
	return p, nil
}

func (p *process) waitLine(prefix string, timeout time.Duration) (string, bool) {
	end := time.Now().Add(timeout)
	for time.Now().Before(end) {
		for _, line := range p.output.all() {
			if strings.HasPrefix(line, prefix) {
				return line, true
			}
		}
		select {
		case <-p.done:
			return "", false
		case <-time.After(20 * time.Millisecond):
		}
	}
	return "", false
}

func freeUDPPort() (int, error) {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return 0, err
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).Port, nil
}

func runDeadline() time.Duration {
	if os.Getenv("ROCKETNET_NIGHTLY_NO_TIMEOUT") == "1" {
		return 0
	}
	return defaultRunDeadline
}

var errPortTaken = errors.New("the server could not bind its port")

// runOnce runs one pairing; it returns the failure reasons (none on a pass).
func runOnce(bins map[string]string, p pairing, count int, dir string) ([]string, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	ctx := context.Background()
	if d := runDeadline(); d > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, d)
		defer cancel()
	}
	port, err := freeUDPPort()
	if err != nil {
		return nil, err
	}
	address := fmt.Sprintf("127.0.0.1:%d", port)
	server, err := startProcess(ctx, filepath.Join(dir, "server.log"), []string{"ROCKETNET_SERVER_TRANSPORT=" + p.transport}, bins[p.server], "server", address, "1")
	if err != nil {
		return nil, err
	}
	keyLine, ok := server.waitLine("SRV key ", 15*time.Second)
	if !ok {
		_ = server.cmd.Process.Kill()
		<-server.done
		for _, line := range server.output.all() {
			if strings.Contains(line, "Address already in use") {
				return nil, errPortTaken
			}
		}
		return []string{"server printed no key: " + strings.Join(server.output.all(), " | ")}, nil
	}
	key := strings.TrimPrefix(keyLine, "SRV key ")
	clientEnv := []string{"ROCKETNET_CLIENT_TRANSPORT=portable"}
	var clientLines []string
	clientExit := 0
	var reasons []string
	if p.caseName == "die" {
		victim, err := startProcess(ctx, filepath.Join(dir, "client-die.log"), clientEnv, bins[p.client], "client", address, key, "die", fmt.Sprint(count))
		if err != nil {
			return nil, err
		}
		if _, ok := victim.waitLine("CLI ready-to-die", 60*time.Second); !ok {
			reasons = append(reasons, "the client to kill never ran traffic")
		}
		_ = victim.cmd.Process.Signal(syscall.SIGKILL)
		<-victim.done
		for _, line := range victim.output.all() {
			if hasFailLine(line) {
				reasons = append(reasons, "killed client: "+line)
			}
		}
		next, err := startProcess(ctx, filepath.Join(dir, "client.log"), clientEnv, bins[p.client], "client", address, key, "basic", fmt.Sprint(count))
		if err != nil {
			return nil, err
		}
		<-next.done
		clientLines, clientExit = next.output.all(), next.exit
	} else {
		client, err := startProcess(ctx, filepath.Join(dir, "client.log"), clientEnv, bins[p.client], "client", address, key, p.caseName, fmt.Sprint(count))
		if err != nil {
			return nil, err
		}
		<-client.done
		clientLines, clientExit = client.output.all(), client.exit
	}
	// let the server print the last connection's end, then stop it
	time.Sleep(300 * time.Millisecond)
	_ = server.cmd.Process.Signal(syscall.SIGINT)
	select {
	case <-server.done:
	case <-time.After(20 * time.Second):
		_ = server.cmd.Process.Kill()
		<-server.done
		reasons = append(reasons, "server did not exit within 20 s of SIGINT")
	}
	if ctx.Err() != nil {
		reasons = append(reasons, "the run passed its deadline")
	}
	return append(reasons, grade(p.caseName, p.server, p.transport, clientExit, server.exit, clientLines, server.output.all())...), nil
}

func runPairings(bins map[string]string, list []pairing, count, parallel int, out string) bool {
	jobs := make(chan int)
	var mu sync.Mutex
	var wg sync.WaitGroup
	passed, failed := 0, 0
	for w := 0; w < parallel; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				p := list[i]
				dir := filepath.Join(out, fmt.Sprintf("%s-%s-c%s-s%s", p.transport, p.caseName, p.client, p.server))
				name := fmt.Sprintf("case=%s client=%s server=%s transport=%s", p.caseName, p.client, p.server, p.transport)
				start := time.Now()
				var reasons []string
				var err error
				// a port another process took between the probe and the bind is the
				// harness's miss, not the library's: try again on a fresh port
				for attempt := 1; attempt <= 3; attempt++ {
					reasons, err = runOnce(bins, p, count, dir)
					if !errors.Is(err, errPortTaken) {
						break
					}
				}
				if err != nil {
					reasons = append(reasons, "harness: "+err.Error())
				}
				mu.Lock()
				if len(reasons) == 0 {
					passed++
					fmt.Printf("XVER PASS %s %.0fs\n", name, time.Since(start).Seconds())
				} else {
					failed++
					for _, r := range reasons {
						fmt.Printf("XVER FAIL %s: %s\n", name, r)
					}
					fmt.Printf("XVER FAIL %s: logs in %s\n", name, dir)
				}
				mu.Unlock()
			}
		}()
	}
	for i := range list {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	fmt.Printf("XVER summary pass=%d fail=%d\n", passed, failed)
	return failed == 0
}

// ---- build ----

func build(src, tree, buildDir string) error {
	steps := [][]string{
		{"cmake", "-S", src, "-B", buildDir, "-DXVER_TREE=" + tree, "-DCMAKE_BUILD_TYPE=Release"},
		{"cmake", "--build", buildDir, "--target", "xver", "--parallel", fmt.Sprint(min(runtime.NumCPU(), 8))},
	}
	for _, step := range steps {
		cmd := exec.Command(step[0], step[1:]...)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("%s: %w", strings.Join(step, " "), err)
		}
	}
	return nil
}

// ---- command line ----

type binFlags map[string]string

func (b binFlags) String() string {
	var parts []string
	for k, v := range b {
		parts = append(parts, k+"="+v)
	}
	sort.Strings(parts)
	return strings.Join(parts, ",")
}

func (b binFlags) Set(value string) error {
	label, path, ok := strings.Cut(value, "=")
	if !ok || label == "" || path == "" {
		return fmt.Errorf("-bin wants <label>=<path>, not %q", value)
	}
	b[label] = path
	return nil
}

func splitList(s string) []string {
	var out []string
	for _, part := range strings.Split(s, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, part)
		}
	}
	return out
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: xver build -tree <source tree> -build <build dir> [-src .]")
	fmt.Fprintln(os.Stderr, "       xver run -bin <label>=<path> ... -new <label> [-transports gso,io_uring] [-cases ...] [-parallel 4] [-count 300] [-out dir]")
	os.Exit(2)
}

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	switch os.Args[1] {
	case "build":
		fs := flag.NewFlagSet("build", flag.ExitOnError)
		src := fs.String("src", ".", "the directory holding xver.c and its CMakeLists.txt (run from tools/xver, as the nightly does)")
		tree := fs.String("tree", "", "the rocketnet source tree to build against")
		buildDir := fs.String("build", "", "the build directory; the harness is <build>/xver")
		_ = fs.Parse(os.Args[2:])
		if *tree == "" || *buildDir == "" {
			usage()
		}
		absSrc, err1 := filepath.Abs(*src)
		absTree, err2 := filepath.Abs(*tree)
		if err := errors.Join(err1, err2); err != nil {
			fmt.Println("XVER FAIL build:", err)
			os.Exit(1)
		}
		if err := build(absSrc, absTree, *buildDir); err != nil {
			fmt.Printf("XVER FAIL build tree=%s: %v\n", *tree, err)
			os.Exit(1)
		}
		fmt.Printf("XVER built %s\n", filepath.Join(*buildDir, "xver"))
	case "run":
		fs := flag.NewFlagSet("run", flag.ExitOnError)
		bins := binFlags{}
		fs.Var(bins, "bin", "<label>=<xver binary>, once per version")
		newest := fs.String("new", "", "the label of the newest version")
		transports := fs.String("transports", "", "kernel server transports to run every pair on, e.g. gso,io_uring")
		cases := fs.String("cases", strings.Join(allCases, ","), "the cases to run")
		parallel := fs.Int("parallel", 4, "runs at once")
		count := fs.Int("count", 300, "messages per stream per session")
		out := fs.String("out", "xver-runs", "where each run's logs go")
		_ = fs.Parse(os.Args[2:])
		if _, ok := bins[*newest]; !ok || len(bins) < 1 || *parallel < 1 {
			usage()
		}
		for _, c := range splitList(*cases) {
			known := false
			for _, k := range allCases {
				known = known || k == c
			}
			if !known {
				fmt.Printf("XVER FAIL unknown case %q\n", c)
				os.Exit(2)
			}
		}
		var labels []string
		missing := false
		for label, path := range bins {
			labels = append(labels, label)
			if info, err := os.Stat(path); err != nil || info.IsDir() {
				fmt.Printf("XVER FAIL version=%s: no harness at %s\n", label, path)
				missing = true
			}
		}
		if missing {
			os.Exit(1)
		}
		sort.Strings(labels)
		list := pairings(labels, *newest, splitList(*transports), splitList(*cases))
		fmt.Printf("XVER plan %d runs: versions %v, newest %s, transports portable %v\n", len(list), labels, *newest, splitList(*transports))
		if !runPairings(bins, list, *count, *parallel, *out) {
			os.Exit(1)
		}
	default:
		usage()
	}
}
