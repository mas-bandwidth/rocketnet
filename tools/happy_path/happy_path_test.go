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
	for {
		if _, err := os.Stat(os.Getenv("NIGHTLY_DEADLINE_RELEASE")); err == nil {
			break
		}
		time.Sleep(time.Millisecond)
	}
	fmt.Println("ready")
	os.Exit(7)
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
