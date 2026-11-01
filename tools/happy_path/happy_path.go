// happy_path is the small end-to-end "yep, it works" check: it starts the
// real server and client binaries as separate processes over loopback on the
// portable transport, watches their stdout, and passes only when a client
// connects, one message of every reliability type arrives, the receipts for
// the receipt types come back, the client disconnects cleanly, and the server
// stops on its own.
//
// It is deliberately not exhaustive: no second channel, no large fragmented
// message, no counters, no network simulator, no stress.
//
// HAPPY_PATH_BREAK=client-skip-mode-<n> makes the client omit one send. It
// exists only to prove this tool's failing path; a normal run does not set it.
// HAPPY_PATH_DEADLINE_SECONDS overrides the tool's hard deadline (default 60).
// ROCKETNET_NIGHTLY_NO_TIMEOUT=1 disables only that harness execution deadline.
//
// The two binary names are constants below. When the examples' message-sending
// examples land, point these two constants at the example binaries so this
// check also proves the examples work: that is the whole swap.

package main

import (
	"bytes"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	serverBinary           = "happy_server"
	clientBinary           = "happy_client"
	defaultDeadlineSeconds = 60.0
)

var reliabilityNames = [8]string{
	"ROCKETNET_RELIABILITY_UNRELIABLE",
	"ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED",
	"ROCKETNET_RELIABILITY_RELIABLE",
	"ROCKETNET_RELIABILITY_RELIABLE_ORDERED",
	"ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED",
	"ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT",
	"ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT",
	"ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT",
}

type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

type child struct {
	name    string
	cmd     *exec.Cmd
	buf     *syncBuffer
	done    chan struct{}
	waitErr error
}

func startChild(name, path string, args, extraEnv []string) (*child, error) {
	c := &child{name: name, buf: &syncBuffer{}, done: make(chan struct{})}
	cmd := exec.Command(path, args...)
	cmd.Env = append(os.Environ(), extraEnv...)
	cmd.Stdout = c.buf
	cmd.Stderr = c.buf
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	c.cmd = cmd
	go func() {
		c.waitErr = cmd.Wait()
		close(c.done)
	}()
	return c, nil
}

func (c *child) exited() bool {
	select {
	case <-c.done:
		return true
	default:
		return false
	}
}

func (c *child) exitCode() int {
	if c.cmd == nil || c.cmd.ProcessState == nil {
		return -1
	}
	return c.cmd.ProcessState.ExitCode()
}

func (c *child) kill() {
	if c.cmd != nil && c.cmd.Process != nil {
		_ = c.cmd.Process.Kill()
	}
}

func formatSeconds(seconds float64) string {
	return strconv.FormatFloat(seconds, 'g', -1, 64)
}

func executionDeadline(seconds float64) time.Time {
	if os.Getenv("ROCKETNET_NIGHTLY_NO_TIMEOUT") == "1" {
		return time.Time{}
	}
	return time.Now().Add(time.Duration(seconds * float64(time.Second)))
}

func deadlineError(seconds float64, stage string) error {
	return fmt.Errorf("deadline of %s seconds exceeded at stage %s", formatSeconds(seconds), stage)
}

func hasLine(text, want string) bool {
	for _, line := range strings.Split(text, "\n") {
		if strings.TrimRight(line, "\r") == want {
			return true
		}
	}
	return false
}

func countLine(text, want string) int {
	n := 0
	for _, line := range strings.Split(text, "\n") {
		if strings.TrimRight(line, "\r") == want {
			n++
		}
	}
	return n
}

func lineIndex(text, want string) int {
	for i, line := range strings.Split(text, "\n") {
		if strings.TrimRight(line, "\r") == want {
			return i
		}
	}
	return -1
}

func waitForLine(c *child, want string, deadline time.Time, seconds float64, stage, what string) error {
	for {
		if hasLine(c.buf.String(), want) {
			return nil
		}
		if c.exited() {
			if hasLine(c.buf.String(), want) {
				return nil
			}
			return fmt.Errorf("%s: %s exited before %s (exit code %d)", stage, c.name, what, c.exitCode())
		}
		if !deadline.IsZero() && !time.Now().Before(deadline) {
			return deadlineError(seconds, stage)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func waitForExit(c *child, deadline time.Time, seconds float64, stage string) error {
	for {
		if c.exited() {
			return nil
		}
		if !deadline.IsZero() && !time.Now().Before(deadline) {
			return deadlineError(seconds, stage)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func candidateDirs() []string {
	var dirs []string
	add := func(dir string) {
		if dir != "" {
			dirs = append(dirs, filepath.Clean(dir))
		}
	}
	if _, file, _, ok := runtime.Caller(0); ok {
		if absolute, err := filepath.Abs(file); err == nil {
			add(filepath.Join(filepath.Dir(absolute), "..", "..", "bin"))
		}
	}
	if wd, err := os.Getwd(); err == nil {
		add(filepath.Join(wd, "bin"))
		add(filepath.Join(wd, "..", "..", "bin"))
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		add(dir)
		add(filepath.Join(dir, ".."))
	}
	return dirs
}

func findBinary(name string) (string, error) {
	file := name
	if runtime.GOOS == "windows" {
		file = name + ".exe"
	}
	seen := map[string]bool{}
	for _, dir := range candidateDirs() {
		var tries []string
		if runtime.GOOS == "windows" {
			tries = append(tries, filepath.Join(dir, "Release", file))
		}
		tries = append(tries, filepath.Join(dir, file))
		for _, path := range tries {
			if seen[path] {
				continue
			}
			seen[path] = true
			if info, err := os.Stat(path); err == nil && !info.IsDir() {
				return path, nil
			}
		}
	}
	return "", fmt.Errorf("could not find %s under bin/", file)
}

func freeUDPPort() (int, error) {
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		return 0, err
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).Port, nil
}

func run() int {
	seconds := defaultDeadlineSeconds
	if value := os.Getenv("HAPPY_PATH_DEADLINE_SECONDS"); value != "" {
		parsed, err := strconv.ParseFloat(value, 64)
		if err != nil || parsed <= 0 {
			fmt.Printf("error: HAPPY_PATH_DEADLINE_SECONDS=%q is not a positive number\n", value)
			return 1
		}
		seconds = parsed
	}
	deadline := executionDeadline(seconds)

	var children []*child
	defer func() {
		for _, c := range children {
			c.kill()
		}
	}()

	fail := func(err error) int {
		fmt.Printf("error: %v\n", err)
		for _, c := range children {
			fmt.Printf("----- %s stdout -----\n", c.name)
			text := c.buf.String()
			fmt.Print(text)
			if !strings.HasSuffix(text, "\n") {
				fmt.Println()
			}
			fmt.Printf("----- end %s stdout -----\n", c.name)
		}
		return 1
	}

	serverPath, err := findBinary(serverBinary)
	if err != nil {
		return fail(err)
	}
	clientPath, err := findBinary(clientBinary)
	if err != nil {
		return fail(err)
	}

	port, err := freeUDPPort()
	if err != nil {
		return fail(fmt.Errorf("could not pick a free loopback port: %v", err))
	}
	address := fmt.Sprintf("127.0.0.1:%d", port)

	fmt.Println("starting server:")
	server, err := startChild("server", serverPath, []string{address}, []string{
		"ROCKETNET_SERVER_IPV4_ADDRESS=" + address,
		"ROCKETNET_SERVER_TRANSPORT=portable",
	})
	if err != nil {
		return fail(fmt.Errorf("could not start server: %v", err))
	}
	children = append(children, server)

	if err := waitForLine(server, "server started", deadline, seconds, "server", "the server started"); err != nil {
		return fail(err)
	}
	if err := waitForLine(server, "server transport portable", deadline, seconds, "server", "the transport line arrived"); err != nil {
		return fail(err)
	}
	serverKey := ""
	for _, line := range strings.Split(server.buf.String(), "\n") {
		if key, ok := strings.CutPrefix(strings.TrimRight(line, "\r"), "server public key "); ok {
			serverKey = key
		}
	}
	if serverKey == "" {
		return fail(fmt.Errorf("server: the server did not print its public key"))
	}
	fmt.Println("verifying server ... OK")

	fmt.Println("starting client:")
	client, err := startChild("client", clientPath, []string{address, serverKey}, []string{
		"ROCKETNET_CLIENT_CONNECT_ADDRESS=" + address,
		"ROCKETNET_CLIENT_TRANSPORT=portable",
	})
	if err != nil {
		return fail(fmt.Errorf("could not start client: %v", err))
	}
	children = append(children, client)

	if err := waitForLine(client, "client connected", deadline, seconds, "client connected", "the client connected"); err != nil {
		return fail(err)
	}
	if err := waitForLine(server, "client connected", deadline, seconds, "client connected", "the server saw the client connect"); err != nil {
		return fail(err)
	}
	fmt.Println("verifying client connected ... OK")

	for mode, name := range reliabilityNames {
		stage := fmt.Sprintf("message %s", name)
		received := fmt.Sprintf("received mode %d %s ok", mode, name)
		if err := waitForLine(server, received, deadline, seconds, stage, fmt.Sprintf("message mode %d (%s) arrived", mode, name)); err != nil {
			return fail(err)
		}
		if mode >= 5 {
			receipt := fmt.Sprintf("receipt mode %d acked", mode)
			if err := waitForLine(client, receipt, deadline, seconds, stage, fmt.Sprintf("the receipt for mode %d arrived", mode)); err != nil {
				return fail(err)
			}
		}
		fmt.Printf("verifying message %s ... OK\n", name)
	}

	serverText := server.buf.String()
	for mode, name := range reliabilityNames {
		received := fmt.Sprintf("received mode %d %s ok", mode, name)
		if count := countLine(serverText, received); count != 1 {
			return fail(fmt.Errorf("message mode %d (%s) arrived %d times, want exactly once", mode, name, count))
		}
		bad := fmt.Sprintf("received mode %d BAD", mode)
		if countLine(serverText, bad) != 0 {
			return fail(fmt.Errorf("message mode %d (%s) arrived with the wrong bytes", mode, name))
		}
	}
	ordered := [3]int{3, 4, 7}
	for i := 1; i < len(ordered); i++ {
		before := fmt.Sprintf("received mode %d %s ok", ordered[i-1], reliabilityNames[ordered[i-1]])
		after := fmt.Sprintf("received mode %d %s ok", ordered[i], reliabilityNames[ordered[i]])
		if lineIndex(serverText, before) >= lineIndex(serverText, after) {
			return fail(fmt.Errorf("ordered messages arrived out of order: mode %d before mode %d", ordered[i-1], ordered[i]))
		}
	}

	if err := waitForLine(client, "disconnected", deadline, seconds, "disconnect", "the client disconnected"); err != nil {
		return fail(err)
	}
	if err := waitForLine(server, "client disconnected", deadline, seconds, "disconnect", "the server saw the client disconnect"); err != nil {
		return fail(err)
	}
	fmt.Println("verifying disconnect ... OK")

	if err := waitForLine(server, "server stopped", deadline, seconds, "server stopped", "the server stopped"); err != nil {
		return fail(err)
	}
	if err := waitForExit(client, deadline, seconds, "server stopped"); err != nil {
		return fail(err)
	}
	if err := waitForExit(server, deadline, seconds, "server stopped"); err != nil {
		return fail(err)
	}
	if code := client.exitCode(); code != 0 {
		return fail(fmt.Errorf("server stopped: the client exited %d, want 0", code))
	}
	if code := server.exitCode(); code != 0 {
		return fail(fmt.Errorf("server stopped: the server exited %d, want 0", code))
	}
	fmt.Println("verifying server stopped ... OK")

	fmt.Println("PASS")
	return 0
}

func main() {
	os.Exit(run())
}
