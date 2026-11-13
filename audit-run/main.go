// auditrun: the v1.2 dynamic audit's job driver (work branch only, never merged).
//
//	auditrun run  -plan b4|h2 -root DIR -budget CORES   run the plan, resumable (status.tsv)
//	auditrun fuzz -wt DIR -seconds N -par N             fuzz every target in one process
//
// Every job of a plan runs under `nova-reserve run --`, with its log in
// DIR/logs/<name>.log and one line per finished job in DIR/status.tsv.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

var useReserve = true

const commit ="ef7fc1eb1bd9b898fb5916796759dd80950859a9"

type job struct {
	name     string
	after    []string // must have exited 0
	afterAny []string // must have finished
	dir      string   // worktree name under root, or "" for root
	env      []string
	argv     []string
	cores    int
	reserve  bool
}

type plan struct {
	root string
	jobs []*job
}

func (p *plan) add(j *job) *job { p.jobs = append(p.jobs, j); return j }

func (p *plan) worktree(name string) string {
	jn := "wt-" + name
	p.add(&job{name: jn, argv: []string{"@worktree", name}})
	return jn
}

const asanFlags = "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
const tsanFlags = "-fsanitize=thread -fno-omit-frame-pointer -O1"
const fuzzFlags = "-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"

var asanEnv = []string{
	"ASAN_OPTIONS=detect_leaks=1",
	"UBSAN_OPTIONS=print_stacktrace=1",
	"LSAN_OPTIONS=suppressions=tools/lsan.supp:print_suppressions=1",
	"ROCKETNET_TEST_NIGHTLY=1",
	"ROCKETNET_TEST_KEEP_GOING=1",
	"ROCKETNET_TEST_JOBS=6",
}

var tsanEnv = []string{
	"TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1",
	"ROCKETNET_TEST_NIGHTLY=1",
	"ROCKETNET_TEST_KEEP_GOING=1",
	"ROCKETNET_TEST_JOBS=6",
	"ROCKETNET_TEST_CASE_TIME_SCALE=4",
}

func with(base []string, more ...string) []string {
	out := append([]string{}, base...)
	return append(out, more...)
}

func cxx(cc string) string {
	if cc == "gcc" {
		return "g++"
	}
	return "clang++"
}

type fuzzTarget struct{ target, seeds, dict string }

var fuzzTargets = []fuzzTarget{
	{"rocketnet_fuzz", "tests/fuzz/corpus", "tests/fuzz/dict/rocketnet_fuzz.dict"},
	{"rocketnet_fuzz_connect_token", "tests/fuzz/corpus_connect_token", "tests/fuzz/dict/rocketnet_fuzz_connect_token.dict"},
	{"rocketnet_fuzz_receive_structured", "tests/fuzz/corpus_receive_structured", "tests/fuzz/dict/rocketnet_fuzz_receive_structured.dict"},
	{"rocketnet_fuzz_message", "tests/fuzz/corpus_message", "tests/fuzz/dict/rocketnet_fuzz_message.dict"},
	{"rocketnet_fuzz_token_private", "tests/fuzz/corpus_token_private", "tests/fuzz/dict/rocketnet_fuzz_token_private.dict"},
	{"rocketnet_fuzz_round_trip", "tests/fuzz/corpus_round_trip", "tests/fuzz/dict/rocketnet_fuzz_round_trip.dict"},
	{"rocketnet_fuzz_config", "tests/fuzz/corpus_config", "tests/fuzz/dict/rocketnet_fuzz_config.dict"},
	{"rocketnet_fuzz_xdp_frame", "tests/fuzz/corpus_xdp_frame", "tests/fuzz/dict/rocketnet_fuzz_xdp_frame.dict"},
	{"rocketnet_fuzz_uring_recvmsg", "tests/fuzz/corpus_uring_recvmsg", "tests/fuzz/dict/rocketnet_fuzz_uring_recvmsg.dict"},
	{"rocketnet_fuzz_gateway_netlink", "tests/fuzz/corpus_gateway_netlink", "tests/fuzz/dict/rocketnet_fuzz_gateway_netlink.dict"},
	{"rocketnet_fuzz_server_transport", "tests/fuzz/corpus_server_transport", "tests/fuzz/dict/rocketnet_fuzz_server_transport.dict"},
	{"rocketnet_fuzz_client_transport", "tests/fuzz/corpus_client_transport", "tests/fuzz/dict/rocketnet_fuzz_client_transport.dict"},
	{"rocketnet_fuzz_session", "tests/fuzz/corpus_session", "tests/fuzz/dict/rocketnet_fuzz_session.dict"},
	{"rocketnet_fuzz_session_raw", "tests/fuzz/corpus_session_raw", "tests/fuzz/dict/rocketnet_fuzz_session.dict"},
	{"rocketnet_fuzz_endpoint", "tests/fuzz/corpus_endpoint", "tests/fuzz/dict/rocketnet_fuzz_endpoint.dict"},
	{"rocketnet_fuzz_xdp_filter", "tests/fuzz/corpus_xdp_filter", "tests/fuzz/dict/rocketnet_fuzz_xdp_filter.dict"},
}

func planB4(p *plan) {
	// 1 and 6: ASan + UBSan (+ LSan at exit), both compilers, Debug and RelWithDebInfo
	for _, cc := range []string{"gcc", "clang"} {
		for _, bt := range []string{"Debug", "RelWithDebInfo"} {
			n := "asan-" + cc + "-" + strings.ToLower(bt)
			wt := p.worktree(n)
			cfg := p.add(&job{name: n + "-configure", after: []string{wt}, dir: n, cores: 1, reserve: true, argv: []string{
				"cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=" + bt, "-DCMAKE_C_COMPILER=" + cc, "-DCMAKE_CXX_COMPILER=" + cxx(cc),
				"-DCMAKE_C_FLAGS=" + asanFlags, "-DCMAKE_CXX_FLAGS=" + asanFlags}})
			b := p.add(&job{name: n + "-build", after: []string{cfg.name}, dir: n, cores: 4, reserve: true, argv: []string{
				"cmake", "--build", "build", "--target", "test", "functional", "test_lifecycle", "test_transport", "test_sodium", "--parallel", "6"}})
			u := p.add(&job{name: n + "-unit", after: []string{b.name}, dir: n, cores: 4, reserve: true, env: asanEnv, argv: []string{"./bin/test"}})
			f := p.add(&job{name: n + "-functional", after: []string{b.name}, afterAny: []string{u.name}, dir: n, cores: 4, reserve: true,
				env: with(asanEnv, "ROCKETNET_TEST_CASE_TIME_SCALE=2"),
				argv: []string{"bash", "tools/run_functional.sh", "dev-sanitized", "--log", "functional-asan.log", "./bin/functional"}})
			last := f.name
			for _, bin := range []string{"test_lifecycle", "test_transport", "test_sodium"} {
				env := with(asanEnv)
				if bin != "test_sodium" {
					env = with(env, "ROCKETNET_SERVER_TRANSPORT=portable", "ROCKETNET_CLIENT_TRANSPORT=portable")
				}
				j := p.add(&job{name: n + "-" + bin, after: []string{b.name}, afterAny: []string{last}, dir: n, cores: 2, reserve: true, env: env, argv: []string{"./bin/" + bin}})
				last = j.name
			}
			p.add(&job{name: n + "-redzones-selftest", after: []string{b.name}, afterAny: []string{last}, dir: n, cores: 4, reserve: true,
				argv: []string{"bash", "scripts/red-zones-selftest", "build"}})
		}
	}

	// 4: fuzzers, Debug (asserts live) with libFuzzer, corpus replay then 600 s each
	fz := p.worktree("fuzz")
	fcfg := p.add(&job{name: "fuzz-configure", after: []string{fz}, dir: "fuzz", cores: 1, reserve: true, argv: []string{
		"cmake", "-B", "build-fuzz", "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
		"-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_C_FLAGS=" + fuzzFlags, "-DCMAKE_CXX_FLAGS=" + fuzzFlags}})
	mk := p.add(&job{name: "fuzz-mkdir", after: []string{fcfg.name}, dir: "fuzz", argv: []string{"mkdir", "-p", "bin", "crashes", "fuzz-corpus"}})
	replayTargets := []string{"cmake", "--build", "build-fuzz", "--target", "rocketnet_dev", "rocketnet_xdp_filter_host"}
	for _, t := range fuzzTargets {
		replayTargets = append(replayTargets, "fuzz_replay_"+t.target)
	}
	fb := p.add(&job{name: "fuzz-build", after: []string{mk.name}, dir: "fuzz", cores: 4, reserve: true, argv: append(replayTargets, "--parallel", "6")})
	xo := p.add(&job{name: "fuzz-xdp-host-obj", after: []string{fb.name}, dir: "fuzz", cores: 1, reserve: true, argv: []string{
		"clang", "-g", "-O1", "-std=gnu11", "-ffreestanding", "-ffp-contract=off", "-D__linux__", "-D__BPF__",
		"-fsanitize=fuzzer-no-link,address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
		"-Itests/xdp_host", "-Isdk", "-DROCKETNET_XDP_FILTER_SOURCE=\"@PWD@/sdk/xdp/server_xdp.c\"",
		"-c", "tests/xdp_host/server_xdp_host.c", "-o", "bin/xdp_host_fuzz.o"}})
	var built []string
	var replays []string
	for _, t := range fuzzTargets {
		args := []string{"clang++", "-g", "-O1", "-std=c++17", "-ffp-contract=off", "-DROCKETNET_DEV=1",
			"-fsanitize=fuzzer,address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-Isodium", "-Isdk", "-Itests"}
		after := []string{fb.name}
		if t.target == "rocketnet_fuzz_xdp_filter" {
			args = append(args, "-DROCKETNET_FUZZ_XDP_FILTER_HOST=1", "bin/xdp_host_fuzz.o")
			after = append(after, xo.name)
		}
		args = append(args, "tests/fuzz/"+t.target+".cpp", "build-fuzz/librocketnet_dev.a", "-lbpf", "-lxdp", "-luring", "-lpthread", "-o", "bin/"+t.target)
		j := p.add(&job{name: "fuzz-build-" + t.target, after: after, dir: "fuzz", cores: 1, reserve: true, argv: args})
		built = append(built, j.name)
		corpus := "tests/fuzz/corpus" + strings.TrimPrefix(t.target, "rocketnet_fuzz")
		r := p.add(&job{name: "fuzz-replay-" + t.target, after: []string{fb.name}, dir: "fuzz", cores: 1, reserve: true,
			env: []string{"ASAN_OPTIONS=detect_leaks=0"}, argv: []string{"./bin/fuzz_replay_" + t.target, corpus + "/*"}})
		replays = append(replays, r.name)
	}
	p.add(&job{name: "fuzz-run-debug", after: built, afterAny: replays, dir: "fuzz", cores: 8, reserve: true,
		env:  []string{"ASAN_OPTIONS=detect_leaks=1", "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"},
		argv: []string{"@self", "fuzz", "-wt", ".", "-seconds", "600", "-par", "8"}})
	nd := p.worktree("fuzz-ndebug")
	p.add(&job{name: "fuzz-run-ndebug", after: []string{nd}, afterAny: []string{"fuzz-run-debug"}, dir: "fuzz-ndebug", cores: 8, reserve: true,
		argv: []string{"bash", "tools/fuzz_ndebug.sh", "--seconds", "600", "--jobs", "8", "--build-jobs", "4"}})

	// 3: MemorySanitizer, the project's recipe
	ms := p.worktree("msan")
	p.add(&job{name: "msan-run", after: []string{ms}, dir: "msan", cores: 8, reserve: true, argv: []string{"bash", "tools/run_msan.sh"}})

	// 5: the whole functional suite forced onto each Linux server transport, 5 times, Release
	rl := p.worktree("release")
	rc := p.add(&job{name: "release-configure", after: []string{rl}, dir: "release", cores: 1, reserve: true, argv: []string{"cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"}})
	rb := p.add(&job{name: "release-build", after: []string{rc.name}, dir: "release", cores: 4, reserve: true, argv: []string{"cmake", "--build", "build", "--target", "functional", "test", "--parallel", "6"}})
	prev := ""
	for _, tr := range []string{"gso", "io_uring"} {
		for i := 1; i <= 5; i++ {
			j := &job{name: fmt.Sprintf("release-functional-%s-%d", tr, i), after: []string{rb.name}, dir: "release", cores: 4, reserve: true,
				env:  []string{"ROCKETNET_SERVER_TRANSPORT=" + tr, "ROCKETNET_TEST_NIGHTLY=1", "ROCKETNET_TEST_KEEP_GOING=1", "ROCKETNET_TEST_JOBS=6"},
				argv: []string{"./bin/functional"}}
			if prev != "" {
				j.afterAny = []string{prev}
			}
			p.add(j)
			prev = j.name
		}
	}
	if os.Getenv("AUDITRUN_XDP_CELL") == "1" {
		p.add(&job{name: "release-xdp-cell-x5", after: []string{rb.name}, afterAny: []string{prev}, dir: "release", cores: 2, reserve: true,
			argv: []string{"scripts/transport-cells-repeat", "--runs", "5", "--cell", "xdp/portable"}})
	}
}

// the rented pair (no CI runners): x9 = ASan, fuzz, forced transports; x8 = TSan, MSan
func planX9(p *plan) { planB4(p); p.drop("msan") }

func planX8(p *plan) {
	planH2(p)
	ms := p.worktree("msan")
	lib := filepath.Join(p.root, "msan-libcxx-21.1.8")
	lb := p.add(&job{name: "msan-libcxx", after: []string{ms}, dir: "msan", cores: 11, argv: []string{"bash", "tools/build_msan_libcxx.sh", lib}})
	p.add(&job{name: "msan-run", after: []string{lb.name}, dir: "msan", cores: 8, env: []string{"RUN_MSAN_LIBCXX=" + lib}, argv: []string{"bash", "tools/run_msan.sh"}})
}

// 7: macOS, as nightly-macos-sanitizers.yml: the system clang, Debug, msgx then portable pins
func planMac(p *plan) {
	prev := ""
	for _, leg := range []string{"asan", "tsan"} {
		n := leg + "-mac"
		wt := p.worktree(n)
		flags := asanFlags
		if leg == "tsan" {
			flags = tsanFlags
		}
		cfg := p.add(&job{name: n + "-configure", after: []string{wt}, dir: n, cores: 1, reserve: true, argv: []string{
			"cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
			"-DCMAKE_C_FLAGS=" + flags, "-DCMAKE_CXX_FLAGS=" + flags}})
		b := p.add(&job{name: n + "-build", after: []string{cfg.name}, dir: n, cores: 6, reserve: true, argv: []string{
			"cmake", "--build", "build", "--target", "test", "functional", "test_transport_control", "test_msgx", "--parallel", "6"}})
		env := []string{"ROCKETNET_NIGHTLY_NO_TIMEOUT=1", "ASAN_OPTIONS=detect_leaks=0", "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1",
			"TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1", "ROCKETNET_TEST_NIGHTLY=1", "ROCKETNET_TEST_KEEP_GOING=1", "ROCKETNET_TEST_JOBS=6"}
		fenv := env
		if leg == "tsan" {
			fenv = with(env, "ROCKETNET_TEST_CASE_TIME_SCALE=30")
		}
		msgx := []string{"ROCKETNET_SERVER_TRANSPORT=msgx", "ROCKETNET_CLIENT_TRANSPORT=msgx"}
		port := []string{"ROCKETNET_SERVER_TRANSPORT=portable", "ROCKETNET_CLIENT_TRANSPORT=portable"}
		steps := []*job{
			{name: n + "-unit-msgx", env: with(env, msgx...), argv: []string{"./bin/test"}},
			{name: n + "-functional-msgx", env: with(fenv, msgx...), argv: []string{"bash", "tools/run_functional.sh", "dev-sanitized-msgx", "--log", "functional-msgx.log", "./bin/functional"}},
			{name: n + "-functional-portable", env: with(fenv, port...), argv: []string{"bash", "tools/run_functional.sh", "dev-sanitized-portable", "--log", "functional-portable.log", "./bin/functional"}},
		}
		for _, s := range steps {
			s.after = []string{b.name}
			s.dir = n
			s.cores = 6
			s.reserve = true
			if prev != "" {
				s.afterAny = []string{prev}
			}
			p.add(s)
			prev = s.name
		}
	}
}

func (p *plan) drop(prefix string) {
	var kept []*job
	for _, j := range p.jobs {
		if !strings.HasPrefix(j.name, prefix) && j.name != "wt-"+prefix {
			kept = append(kept, j)
		}
	}
	p.jobs = kept
}

func planH2(p *plan) {
	// 2: ThreadSanitizer, both compilers, the project's flags
	for _, cc := range []string{"gcc", "clang"} {
		n := "tsan-" + cc
		wt := p.worktree(n)
		cfg := p.add(&job{name: n + "-configure", after: []string{wt}, dir: n, cores: 1, reserve: true, argv: []string{
			"cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_C_COMPILER=" + cc, "-DCMAKE_CXX_COMPILER=" + cxx(cc),
			"-DCMAKE_C_FLAGS=" + tsanFlags, "-DCMAKE_CXX_FLAGS=" + tsanFlags, "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"}})
		// clang's static tsan_cxx runtime and tests/test_channels.cpp both define the
		// global operator new/delete, so bin/test (and functional, which needs it) does
		// not link under clang TSan on Linux; the clang leg links with
		// --allow-multiple-definition (the runtime's operator new wins: the channels
		// allocator trap witness is expected to fail there, as under MSan)
		if cc == "clang" {
			cfg.argv = append(cfg.argv, "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -Wl,--allow-multiple-definition")
		}
		targets := []string{"cmake", "--build", "build", "--target", "test", "functional", "soak", "test_lifecycle", "test_wide_fill"}
		b := p.add(&job{name: n + "-build", after: []string{cfg.name}, dir: n, cores: 4, reserve: true, argv: append(targets, "--parallel", "6")})
		sa := []string{"setarch", "x86_64", "-R"}
		last := ""
		steps := []*job{
			{name: n + "-soak", env: tsanEnv, cores: 2, argv: with(sa, "./bin/soak", "20000")},
			{name: n + "-lifecycle", env: with(tsanEnv, "ROCKETNET_SERVER_TRANSPORT=portable", "ROCKETNET_CLIENT_TRANSPORT=portable"), cores: 2, argv: with(sa, "./bin/test_lifecycle")},
			{name: n + "-wide-fill", env: tsanEnv, cores: 4, argv: with(sa, "./bin/test_wide_fill")},
			{name: n + "-unit", env: tsanEnv, cores: 4, argv: with(sa, "./bin/test")},
			{name: n + "-functional", env: tsanEnv, cores: 4, argv: with([]string{"bash", "tools/run_functional.sh", "dev-sanitized", "--log", "functional-tsan.log"}, with(sa, "./bin/functional")...)},
			{name: n + "-functional-gso", env: with(tsanEnv, "ROCKETNET_SERVER_TRANSPORT=gso"), cores: 4, argv: with(sa, "./bin/functional")},
			{name: n + "-functional-io_uring", env: with(tsanEnv, "ROCKETNET_SERVER_TRANSPORT=io_uring"), cores: 4, argv: with(sa, "./bin/functional")},
		}
		for _, s := range steps {
			s.after = []string{b.name}
			s.dir = n
			s.reserve = true
			if last != "" {
				s.afterAny = []string{last}
			}
			p.add(s)
			last = s.name
		}
	}
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: auditrun run|fuzz ...")
		os.Exit(2)
	}
	switch os.Args[1] {
	case "run":
		runMain(os.Args[2:])
	case "fuzz":
		fuzzMain(os.Args[2:])
	default:
		fmt.Fprintln(os.Stderr, "unknown mode", os.Args[1])
		os.Exit(2)
	}
}

func runMain(args []string) {
	fs := flag.NewFlagSet("run", flag.ExitOnError)
	name := fs.String("plan", "", "b4 or h2")
	root := fs.String("root", "", "job root")
	budget := fs.Int("budget", 12, "cores")
	only := fs.String("only", "", "comma list of job name prefixes to run (default all)")
	redo := fs.String("redo", "", "comma list of job name prefixes to run again whatever status.tsv says")
	reserve := fs.Bool("reserve", true, "run jobs under nova-reserve (false on machines without it)")
	fs.Parse(args)
	useReserve = *reserve
	p := &plan{root: *root}
	switch *name {
	case "b4":
		planB4(p)
	case "h2":
		planH2(p)
	case "x9":
		planX9(p)
	case "x8":
		planX8(p)
	case "mac":
		planMac(p)
	default:
		fmt.Fprintln(os.Stderr, "unknown plan")
		os.Exit(2)
	}
	os.MkdirAll(filepath.Join(p.root, "logs"), 0o755)
	statusPath := filepath.Join(p.root, "status.tsv")
	done := map[string]int{}
	if f, err := os.Open(statusPath); err == nil {
		sc := bufio.NewScanner(f)
		for sc.Scan() {
			var n string
			var rc int
			if _, err := fmt.Sscanf(sc.Text(), "%s\t%d", &n, &rc); err == nil {
				done[n] = rc
			}
		}
		f.Close()
	}
	if *redo != "" {
		for _, pr := range strings.Split(*redo, ",") {
			for n := range done {
				if strings.HasPrefix(n, pr) {
					delete(done, n)
				}
			}
		}
	}
	var prefixes []string
	if *only != "" {
		prefixes = strings.Split(*only, ",")
	}
	selected := func(n string) bool {
		if len(prefixes) == 0 {
			return true
		}
		for _, pr := range prefixes {
			if strings.HasPrefix(n, pr) {
				return true
			}
		}
		return false
	}
	var mu sync.Mutex
	cond := sync.NewCond(&mu)
	free := *budget
	running := map[string]bool{}
	self, _ := os.Executable()
	for {
		mu.Lock()
		progressed := false
		pending := 0
		for _, j := range p.jobs {
			if _, ok := done[j.name]; ok || running[j.name] {
				continue
			}
			if !selected(j.name) {
				continue
			}
			pending++
			ready, dead := true, false
			for _, d := range j.after {
				rc, ok := done[d]
				if !ok {
					ready = false
				} else if rc != 0 {
					dead = true
				}
			}
			for _, d := range j.afterAny {
				if _, ok := done[d]; !ok && selected(d) {
					ready = false
				}
			}
			if dead {
				done[j.name] = -2
				appendStatus(statusPath, j.name, -2, time.Now(), time.Now())
				progressed = true
				continue
			}
			if !ready || j.cores > free {
				continue
			}
			free -= j.cores
			running[j.name] = true
			progressed = true
			go func(j *job) {
				start := time.Now()
				rc := execute(p.root, j, self)
				end := time.Now()
				mu.Lock()
				free += j.cores
				delete(running, j.name)
				done[j.name] = rc
				appendStatus(statusPath, j.name, rc, start, end)
				cond.Broadcast()
				mu.Unlock()
			}(j)
		}
		if pending == 0 && len(running) == 0 {
			mu.Unlock()
			break
		}
		if !progressed {
			cond.Wait()
		}
		mu.Unlock()
	}
	fmt.Println("plan finished")
}

func appendStatus(path, name string, rc int, start, end time.Time) {
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	fmt.Fprintf(f, "%s\t%d\t%s\t%s\t%.0f\n", name, rc, start.UTC().Format(time.RFC3339), end.UTC().Format(time.RFC3339), end.Sub(start).Seconds())
	f.Close()
}

func execute(root string, j *job, self string) int {
	dir := filepath.Join(root, "rocketnet")
	if j.dir != "" {
		dir = filepath.Join(root, j.dir)
	}
	logf, err := os.OpenFile(filepath.Join(root, "logs", j.name+".log"), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return -1
	}
	defer logf.Close()
	if len(j.argv) > 0 && j.argv[0] == "@worktree" {
		wt := filepath.Join(root, j.argv[1])
		if _, err := os.Stat(wt); err == nil {
			fmt.Fprintln(logf, "worktree exists", wt)
			return 0
		}
		cmd := exec.Command("git", "-C", filepath.Join(root, "rocketnet"), "worktree", "add", "--detach", wt, commit)
		cmd.Stdout, cmd.Stderr = logf, logf
		if err := cmd.Run(); err != nil {
			fmt.Fprintln(logf, "worktree:", err)
			return 1
		}
		return 0
	}
	var argv []string
	for _, a := range j.argv {
		a = strings.ReplaceAll(a, "@PWD@", dir)
		if a == "@self" {
			a = self
		}
		if strings.Contains(a, "*") {
			m, _ := filepath.Glob(filepath.Join(dir, a))
			for _, x := range m {
				rel, _ := filepath.Rel(dir, x)
				argv = append(argv, rel)
			}
			continue
		}
		argv = append(argv, a)
	}
	if j.reserve && useReserve {
		argv = append([]string{"nova-reserve", "run", "--why", "audit-run " + j.name, "--"}, argv...)
	}
	fmt.Fprintf(logf, "### %s dir=%s env=%v\n### argv=%q\n", time.Now().UTC().Format(time.RFC3339), dir, j.env, argv)
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), j.env...)
	cmd.Stdout, cmd.Stderr = logf, logf
	err = cmd.Run()
	rc := 0
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			rc = ee.ExitCode()
		} else {
			fmt.Fprintln(logf, "exec:", err)
			rc = 127
		}
	}
	fmt.Fprintf(logf, "### exit %d at %s\n", rc, time.Now().UTC().Format(time.RFC3339))
	return rc
}

func fuzzMain(args []string) {
	fs := flag.NewFlagSet("fuzz", flag.ExitOnError)
	wt := fs.String("wt", ".", "worktree")
	seconds := fs.Int("seconds", 600, "seconds per target")
	par := fs.Int("par", 8, "targets at once")
	fs.Parse(args)
	sem := make(chan struct{}, *par)
	var wg sync.WaitGroup
	var mu sync.Mutex
	failed := 0
	for _, t := range fuzzTargets {
		t := t
		wg.Add(1)
		sem <- struct{}{}
		go func() {
			defer wg.Done()
			defer func() { <-sem }()
			corpus := filepath.Join("fuzz-corpus", t.target)
			crashes := filepath.Join("crashes", t.target) + "/"
			os.MkdirAll(filepath.Join(*wt, corpus), 0o755)
			os.MkdirAll(filepath.Join(*wt, crashes), 0o755)
			logf, _ := os.Create(filepath.Join(*wt, "fuzz-"+t.target+".log"))
			defer logf.Close()
			cmd := exec.Command("./bin/"+t.target, corpus, t.seeds, "-dict="+t.dict,
				fmt.Sprintf("-max_total_time=%d", *seconds), "-print_final_stats=1", "-rss_limit_mb=4096", "-timeout=25",
				"-artifact_prefix="+crashes)
			cmd.Dir = *wt
			cmd.Stdout, cmd.Stderr = logf, logf
			err := cmd.Run()
			rc := 0
			if err != nil {
				rc = 1
				if ee, ok := err.(*exec.ExitError); ok {
					rc = ee.ExitCode()
				}
			}
			mu.Lock()
			fmt.Printf("FUZZ %s exit=%d\n", t.target, rc)
			if rc != 0 {
				failed++
			}
			mu.Unlock()
		}()
	}
	wg.Wait()
	if failed > 0 {
		os.Exit(1)
	}
}
