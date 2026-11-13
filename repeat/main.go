// repeat: run one command N times in a row in DIR while a load command runs
// beside it, and print each run's result and a total.
//
// Usage: repeat <dir> <n> <load-command> -- <command> [args...]
//
// The load command (bash -c) starts first in <dir> and is killed when the
// runs end; a load that ends early is restarted so the load lasts the whole
// series. Each run prints "RUN i rc seconds" and the last line is
// "TOTAL pass/fail n".
package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"time"
)

func main() {
	if len(os.Args) < 6 || os.Args[4] != "--" {
		fmt.Fprintln(os.Stderr, "usage: repeat <dir> <n> <load-command> -- <command> [args...]")
		os.Exit(2)
	}
	dir := os.Args[1]
	n, err := strconv.Atoi(os.Args[2])
	if err != nil || n < 1 {
		fmt.Fprintln(os.Stderr, "n must be a positive count")
		os.Exit(2)
	}
	load, cmd := os.Args[3], os.Args[5:]

	ctx, stop := context.WithCancel(context.Background())
	loads := 0
	done := make(chan struct{})
	go func() {
		defer close(done)
		for ctx.Err() == nil {
			c := exec.CommandContext(ctx, "bash", "-c", load)
			c.Dir = dir
			loadLog, _ := os.OpenFile(dir+"/repeat-load.log", os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
			c.Stdout, c.Stderr = loadLog, loadLog
			_ = c.Run()
			loadLog.Close()
			loads++
		}
	}()
	warm := 3
	if s, err := strconv.Atoi(os.Getenv("REPEAT_WARMUP")); err == nil && s >= 0 {
		warm = s
	}
	time.Sleep(time.Duration(warm) * time.Second) // let the load get going
	fmt.Printf("WARMED %d s load1=%s\n", warm, loadavg())

	pass, fail := 0, 0
	for i := 1; i <= n; i++ {
		start := time.Now()
		c := exec.Command(cmd[0], cmd[1:]...)
		c.Dir = dir
		out, _ := c.CombinedOutput()
		rc := 0
		if c.ProcessState != nil {
			rc = c.ProcessState.ExitCode()
		}
		if rc == 0 {
			pass++
		} else {
			fail++
			os.Stdout.Write(out)
		}
		fmt.Printf("RUN %d rc=%d %.2fs load1=%s\n", i, rc, time.Since(start).Seconds(), loadavg())
	}
	stop()
	<-done
	fmt.Printf("TOTAL pass=%d fail=%d n=%d (load command ran %d time(s))\n", pass, fail, n, loads)
	if fail > 0 {
		os.Exit(1)
	}
}

func loadavg() string {
	b, err := os.ReadFile("/proc/loadavg")
	if err != nil || len(b) < 4 {
		return "?"
	}
	for i, ch := range b {
		if ch == ' ' {
			return string(b[:i])
		}
	}
	return string(b)
}
