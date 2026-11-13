package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestExecutionDeadlineOptIn(t *testing.T) {
	for _, value := range []string{"", "0", "true", "01", "1"} {
		t.Setenv("ROCKETNET_NIGHTLY_NO_TIMEOUT", value)
		deadline := executionDeadline(60)
		if deadline.IsZero() != (value == "1") {
			t.Fatalf("unexpected deadline for %q: %v", value, deadline)
		}
	}
}

func TestUnboundedWaitStillRequiresLineAndExit(t *testing.T) {
	c := &child{name: "fixture", buf: &syncBuffer{}, done: make(chan struct{})}
	expired := time.Now().Add(-time.Second)
	if err := waitForLine(c, "ready", expired, 1, "fixture", "ready"); err == nil || !strings.Contains(err.Error(), "deadline") {
		t.Fatalf("default expired deadline did not refuse: %v", err)
	}
	go func() {
		time.Sleep(5 * time.Millisecond)
		_, _ = c.buf.Write([]byte("ready\n"))
		close(c.done)
	}()
	if err := waitForLine(c, "ready", time.Time{}, 1, "fixture", "ready"); err != nil {
		t.Fatal(err)
	}
	if err := waitForExit(c, time.Time{}, 1, "fixture"); err != nil {
		t.Fatal(err)
	}
	if err := waitForLine(c, "missing", time.Time{}, 1, "fixture", "missing"); err == nil || !strings.Contains(err.Error(), "exited before") {
		t.Fatalf("unbounded wait accepted missing output: %v", err)
	}
}

// The test binary doubles as a real child, so the execution opt-in is checked
// across the same Start/Wait/output path used by the live happy-path harness.
func TestDeadlineChildProcess(t *testing.T) {
	if os.Getenv("NIGHTLY_DEADLINE_CHILD") != "1" {
		return
	}
	fmt.Println("started")
	// bounded like every wait in the tool: a parent that never releases
	// the child cannot leave it running past the cap
	capped := time.Now().Add(waitCap)
	for {
		if _, err := os.Stat(os.Getenv("NIGHTLY_DEADLINE_RELEASE")); err == nil {
			break
		}
		if !time.Now().Before(capped) {
			fmt.Println("never released")
			os.Exit(8)
		}
		time.Sleep(time.Millisecond)
	}
	fmt.Println("ready")
	os.Exit(7)
}

// A zero deadline (the nightly opt-in) no longer means an unbounded wait: a
// child that stays alive and silent ends the wait at the fixed cap, and the
// error names what was awaited.
func TestZeroDeadlineWaitsAreCapped(t *testing.T) {
	saved := waitCap
	waitCap = 20 * time.Millisecond
	defer func() { waitCap = saved }()
	t.Setenv("ROCKETNET_NIGHTLY_NO_TIMEOUT", "1")

	c := &child{name: "silent", buf: &syncBuffer{}, done: make(chan struct{})}
	defer close(c.done)

	err := waitForLine(c, "ready", executionDeadline(60), 60, "fixture", "the ready line")
	if err == nil || !strings.Contains(err.Error(), "per-wait deadline") || !strings.Contains(err.Error(), "the ready line") || !strings.Contains(err.Error(), "silent") {
		t.Fatalf("zero-deadline line wait was not capped with a message naming what it awaited: %v", err)
	}
	err = waitForExit(c, executionDeadline(60), 60, "fixture")
	if err == nil || !strings.Contains(err.Error(), "per-wait deadline") || !strings.Contains(err.Error(), "silent to exit") {
		t.Fatalf("zero-deadline exit wait was not capped with a message naming what it awaited: %v", err)
	}
}

func TestRealChildDeadlineAndFailureRemainDistinct(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	for _, value := range []string{"0", "1"} {
		t.Run(value, func(t *testing.T) {
			t.Setenv("ROCKETNET_NIGHTLY_NO_TIMEOUT", value)
			release := filepath.Join(t.TempDir(), "release")
			c, err := startChild("control", executable, []string{"-test.run=^TestDeadlineChildProcess$"}, []string{"NIGHTLY_DEADLINE_CHILD=1", "NIGHTLY_DEADLINE_RELEASE=" + release})
			if err != nil {
				t.Fatal(err)
			}
			defer c.kill()
			if err := waitForLine(c, "started", time.Time{}, 0, "control", "started"); err != nil {
				t.Fatal(err)
			}
			// Hold the child until after the default watchdog check, so host
			// scheduling cannot turn a delayed child into a fast false pass.
			deadline := executionDeadline(-1)
			if value == "0" {
				err = waitForLine(c, "ready", deadline, 0, "control", "ready")
				if err == nil || !strings.Contains(err.Error(), "deadline") {
					t.Fatalf("default execution cap did not expire: %v", err)
				}
			}
			if err := os.WriteFile(release, nil, 0600); err != nil {
				t.Fatal(err)
			}
			if value == "1" {
				if err := waitForLine(c, "ready", deadline, 0, "control", "ready"); err != nil {
					t.Fatal(err)
				}
			}
			if err := waitForExit(c, time.Time{}, .001, "control"); err != nil {
				t.Fatal(err)
			}
			if c.exitCode() != 7 || c.waitErr == nil {
				t.Fatalf("child failure disappeared: code=%d error=%v", c.exitCode(), c.waitErr)
			}
			if err := waitForLine(c, "missing", time.Time{}, .001, "control", "missing"); err == nil || !strings.Contains(err.Error(), "exited before") {
				t.Fatalf("missing output accepted: %v", err)
			}
		})
	}
}

// A connection the server ended on a timeout also brings its connected count
// to 0 and prints "client disconnected"; only the reason line separates a
// client that disconnected from one that went silent.
func TestDisconnectReasonRequiresTheCleanDisconnect(t *testing.T) {
	head := "server started\nclient connected\nclient disconnected\n"
	if err := checkDisconnectReason(head + "client disconnect reason CLIENT_DISCONNECTED\nserver stopped\n"); err != nil {
		t.Fatalf("clean disconnect refused: %v", err)
	}
	if err := checkDisconnectReason(head + "client disconnect reason CLIENT_DISCONNECTED\r\nserver stopped\r\n"); err != nil {
		t.Fatalf("clean disconnect with CRLF refused: %v", err)
	}
	refused := map[string]string{
		"timed out":     head + "client disconnect reason TIMED_OUT\nserver stopped\n",
		"reliable":      head + "client disconnect reason RELIABLE_TIMED_OUT\nserver stopped\n",
		"no reason":     head + "server stopped\n",
		"two reasons":   head + "client disconnect reason CLIENT_DISCONNECTED\nclient disconnect reason TIMED_OUT\n",
		"reason suffix": head + "client disconnect reason CLIENT_DISCONNECTED_LATE\n",
	}
	for name, text := range refused {
		err := checkDisconnectReason(text)
		if err == nil || !strings.HasPrefix(err.Error(), "disconnect: ") {
			t.Fatalf("%s: accepted or unstaged: %v", name, err)
		}
	}
	if err := checkDisconnectReason(head + "client disconnect reason TIMED_OUT\n"); err == nil || !strings.Contains(err.Error(), "TIMED_OUT") {
		t.Fatalf("timeout not named: %v", err)
	}
}

// The reason line the check reads is the one happy_server.c prints, from the
// library's own reason names.
func TestServerPrintsTheDisconnectReason(t *testing.T) {
	server, err := os.ReadFile("happy_server.c")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		`printf( "` + disconnectReasonPrefix + `%s\n", rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, 0 ) ) );`,
		"config.max_clients = 1;",
	} {
		if !strings.Contains(string(server), want) {
			t.Fatalf("happy_server.c lacks %q", want)
		}
	}
	api, err := os.ReadFile(filepath.Join("..", "..", "sdk", "rocketnet_api.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(api), "case ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED:") || !strings.Contains(string(api), `return "`+cleanDisconnectReason+`";`) {
		t.Fatalf("rocketnet_api.cpp no longer names CLIENT_DISCONNECTED as %q", cleanDisconnectReason)
	}
}
