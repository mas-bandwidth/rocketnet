// The nightly adapter uses the pinned public TLC engine and outcome matcher.
// It deliberately writes separate JSON evidence, never bounded RUNS.tsv rows.
package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"syscall"
	"time"

	"github.com/mas-bandwidth/nova-tools/pkg/tlc"
)

const engineCommit = "0c97d5958bf2b3cb1ad24e6e1b58f5b04e70347a"
const pinnedJar = "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"

// Set from this source file's SHA256 at build time, independently of the
// engine's model-input fingerprint. The adapter changes execution scheduling.
var adapterSHA string

type options struct {
	root, out, group, java, jar, adapter string
	shards, shard                        int
}

type evidence struct {
	Mode          string      `json:"mode"`
	ElapsedCap    *float64    `json:"elapsed_cap_seconds"`
	AdapterSHA    string      `json:"adapter_sha256"`
	EngineCommit  string      `json:"engine_commit"`
	Case          tlc.Case    `json:"case"`
	Inputs        []tlc.Input `json:"inputs"`
	InputSHA      string      `json:"engine_input_sha256"`
	JarSHA        string      `json:"jar_sha256"`
	JavaVersion   string      `json:"java_version"`
	Host          string      `json:"host"`
	CPUs          int         `json:"cpus"`
	Started       string      `json:"started_utc"`
	Seconds       float64     `json:"seconds"`
	Command       []string    `json:"command"`
	Workers       int         `json:"workers"`
	Exit          int         `json:"exit"`
	Generated     string      `json:"generated"`
	Distinct      string      `json:"distinct"`
	Result        string      `json:"result"`
	Reason        string      `json:"reason,omitempty"`
	Interrupted   bool        `json:"interrupted"`
	InputsCurrent bool        `json:"inputs_unchanged"`
}

// hooks keep mutation/refusal controls deterministic without changing a model.
type hooks struct {
	execute    tlc.Executor
	beforeCopy func()
}

func validDigest(value string) bool {
	raw, err := hex.DecodeString(value)
	return err == nil && len(raw) == sha256.Size
}

func runCase(ctx context.Context, o options, h hooks) (rec evidence, err error) {
	rec = evidence{Mode: "nightly-no-timeout", AdapterSHA: o.adapter,
		EngineCommit: engineCommit, Host: runtime.GOOS + "/" + runtime.GOARCH,
		CPUs: runtime.NumCPU(), Exit: tlc.ExitNoStart, Result: "FAIL"}
	if !validDigest(o.adapter) {
		return rec, errors.New("build with the adapter source SHA256")
	}
	if err = os.MkdirAll(filepath.Dir(o.out), 0o755); err != nil {
		return rec, err
	}
	// Refuse a reused output directory instead of replacing previous evidence.
	if err = os.Mkdir(o.out, 0o755); err != nil {
		return rec, err
	}
	defer func() {
		if err != nil {
			rec.Result, rec.Reason = "FAIL", err.Error()
		}
		raw, marshalErr := json.MarshalIndent(rec, "", "  ")
		if marshalErr == nil {
			marshalErr = os.WriteFile(filepath.Join(o.out, "result.json"), append(raw, '\n'), 0o644)
		}
		if marshalErr != nil {
			err = errors.Join(err, marshalErr)
		}
	}()
	if err = tlc.CheckRunner(o.root); err != nil {
		return rec, err
	}
	plan, err := tlc.LoadCases(o.root)
	if err != nil {
		return rec, err
	}
	chosen, err := tlc.Select(plan, o.group, o.shards, o.shard)
	if err != nil {
		return rec, err
	}
	if len(chosen) != 1 {
		return rec, errors.New("nightly invocation must select exactly one declared case")
	}
	src, err := tlc.SourceAt(o.root)
	if err != nil {
		return rec, err
	}
	// Bind selection to the plan bytes included in the fingerprint.
	digested, err := tlc.ParseCases(bytes.NewReader(src.Plan))
	if err != nil {
		return rec, err
	}
	selected, err := tlc.Select(digested, o.group, o.shards, o.shard)
	if err != nil || len(selected) != 1 || selected[0] != chosen[0] {
		return rec, errors.New("case selection changed before fingerprinting")
	}
	rec.Case = selected[0]
	rec.Inputs, err = src.Inputs(rec.Case.Config)
	if err != nil {
		return rec, err
	}
	rec.InputSHA = tlc.Digest(rec.Inputs)
	jar, err := tlc.FindJar(o.jar, os.Getenv)
	if err != nil {
		return rec, err
	}
	rec.JarSHA = jar.SHA256
	if jar.SHA256 != pinnedJar {
		return rec, errors.New("TLC JAR does not match the pinned digest")
	}
	java, err := tlc.FindHelper("java", o.java, exec.LookPath)
	if err != nil {
		return rec, err
	}
	version, err := exec.CommandContext(ctx, java, "-version").CombinedOutput()
	if err != nil {
		return rec, fmt.Errorf("read java version: %w", err)
	}
	rec.JavaVersion, err = tlc.JavaVersion(string(version))
	if err != nil {
		return rec, err
	}
	if h.beforeCopy != nil {
		h.beforeCopy()
	}
	work := filepath.Join(o.out, "work")
	if err = tlc.CopyModels(src.TLADir, work); err != nil {
		return rec, err
	}
	frozen := src
	frozen.TLADir = work
	if err = sameInputs(frozen, rec.Case.Config, rec.InputSHA); err != nil {
		return rec, fmt.Errorf("copy refused: %w", err)
	}
	if err = os.WriteFile(filepath.Join(work, tlc.CasesFile), src.Plan, 0o644); err != nil {
		return rec, err
	}
	cfg, err := os.ReadFile(filepath.Join(work, rec.Case.Config))
	if err != nil {
		return rec, err
	}
	scratch := filepath.Join(o.out, "tlc")
	if err = os.Mkdir(scratch, 0o755); err != nil {
		return rec, err
	}
	rec.Workers = 1
	if rec.Case.Expected == "pass" {
		rec.Workers = 2
	}
	run := tlc.Run{
		Java: java, Jar: jar.Path, Dir: work,
		JVM:    []string{"-XX:+UseParallelGC", "-XX:ActiveProcessorCount=2", "-Xmx2g"},
		TmpDir: scratch, Workers: rec.Workers, LnCheckFinal: true,
		NoDeadlock: rec.Case.Deadlock == "ignore-terminal",
		MetaDir:    filepath.Join(scratch, "states"), Config: rec.Case.Config, Module: rec.Case.Module,
	}
	rec.Command = append([]string{java}, run.Args()...)
	execute := h.execute
	if execute == nil {
		execute = tlc.Execute
	}
	started := time.Now()
	rec.Started = started.UTC().Format(time.RFC3339Nano)
	// No deadline is attached. Explicit cancellation still reaches the child.
	rec.Exit = execute(ctx, run, filepath.Join(o.out, "tlc.log"))
	rec.Seconds = time.Since(started).Seconds()
	rec.Interrupted = ctx.Err() != nil
	raw, err := os.ReadFile(filepath.Join(o.out, "tlc.log"))
	if err != nil {
		return rec, err
	}
	outcome := tlc.Parse(string(raw))
	rec.Generated, rec.Distinct = outcome.Generated, outcome.Distinct
	after, err := tlc.SourceAt(o.root)
	if err != nil {
		return rec, err
	}
	if err = sameInputs(after, rec.Case.Config, rec.InputSHA); err != nil {
		return rec, fmt.Errorf("source changed during execution: %w", err)
	}
	if err = sameInputs(frozen, rec.Case.Config, rec.InputSHA); err != nil {
		return rec, fmt.Errorf("frozen inputs changed during execution: %w", err)
	}
	afterJar, err := tlc.FindJar(o.jar, os.Getenv)
	if err != nil || afterJar.SHA256 != rec.JarSHA {
		return rec, errors.New("TLC JAR changed during execution")
	}
	rec.InputsCurrent = true
	if rec.Interrupted {
		rec.Reason = "interrupted; no proof verdict"
	} else if tlc.Accepts(rec.Case, rec.Exit, string(raw), string(cfg)) && outcome.HasStats() {
		rec.Result = "PASS"
	} else {
		rec.Reason = "declared outcome was not reached"
	}
	return rec, nil
}

func sameInputs(src tlc.Source, config, want string) error {
	got, _, err := src.Fingerprint(config)
	if err != nil {
		return err
	}
	if got != want {
		return errors.New("model/config/plan/engine input fingerprint differs")
	}
	return nil
}

func main() {
	var o options
	flag.StringVar(&o.root, "root", "proofs", "root containing tla/")
	flag.StringVar(&o.out, "dir", "", "new evidence directory")
	flag.StringVar(&o.group, "group", "", "declared group")
	flag.StringVar(&o.jar, "jar", "", "pinned TLC JAR")
	flag.StringVar(&o.java, "java", "", "java executable")
	flag.IntVar(&o.shards, "shards", 1, "group cardinality")
	flag.IntVar(&o.shard, "shard", 0, "zero-based case index")
	flag.Parse()
	if runtime.GOOS != "linux" || o.out == "" || flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "nightly TLC requires Linux, --dir and no positional arguments")
		os.Exit(2)
	}
	var err error
	o.root, err = filepath.Abs(o.root)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	o.out, err = filepath.Abs(o.out)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	o.adapter = adapterSHA
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	rec, err := runCase(ctx, o, hooks{})
	fmt.Printf("CASE %s config=%s mode=%s seconds=%.3f exit=%d\n", rec.Result, rec.Case.Config, rec.Mode, rec.Seconds, rec.Exit)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
	}
	if err != nil || rec.Result != "PASS" {
		os.Exit(1)
	}
}
