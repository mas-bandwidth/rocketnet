// gates: run the v1.2/token-wipe-seam gate list on vision, one ssh per step,
// sequentially; print one line per step and the tail of a failing log.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
)

const repo = "/home/glenn/rowan-jobs/v12-wipe/rocketnet"
const logs = "/home/glenn/rowan-jobs/v12-wipe/logs"

type step struct {
	name string
	cmd  string
	warn bool
}

func cfg(dir string, args string) string {
	return "cmake -S . -B " + dir + " -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + repo + "/" + dir + "/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + repo + "/" + dir + "/bin " + args
}

const res = "nova-reserve run -- "

func main() {
	tsanEnv := "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 ROCKETNET_TEST_CASE_TIME_SCALE=4 "
	tsanFlags := "-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ '-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -O1' '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -O1' -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"
	asanFlags := "-DCMAKE_BUILD_TYPE=Debug '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'"
	trace := "PASS seeds=1-50 runs=50 trace=$(sed -n 's/^  DST_REFERENCE_TRACE: \\([0-9a-f]*\\)$/\\1/p' .github/workflows/nightly-dst.yml)"

	all := []step{
		// lint and text checkers
		{"format", "sh tools/format/check_format.sh", false},
		{"abi", "python3 tools/test_check_abi.py && python3 tools/check_abi.py", false},
		{"purity", "python3 tools/check_unit_purity.py --self-test && python3 tools/check_unit_purity.py .", false},
		{"text", "python3 tools/check_sdk_sources.py && python3 tools/check_header_guards.py && python3 tools/check_client_server_words.py && python3 tools/check_no_owned_linked_lists.py && python3 tools/check_byte_order.py && python3 tools/check_tsan_flags.py && python3 tools/check_platform_policy.py && python3 tools/check_transport_matrix.py", false},
		{"standard", "python3 tools/conformance/check_rn_standard.py && python3 tools/conformance/check_witnesses.py", false},
		{"tla", "python3 tools/check_tla_coverage.py && python3 tools/check_tla_records.py", false},

		// production build (DEV=OFF Release) and its checks
		{"cfg-off", cfg("g-off", "-DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF"), false},
		{"build-off", res + "cmake --build g-off --parallel 4", true},
		{"unit-off", res + "g-off/bin/test", false},
		{"functional-production", res + "g-off/bin/functional_production", false},
		{"production-lifecycle", res + "g-off/bin/test_production_client_lifecycle", false},
		{"exports-off", "python3 tools/check_exports.py g-off/bin/librocketnet.so g-off/librocketnet.a", false},
		{"hardening-off", "python3 tools/check_hardening.py g-off/bin/librocketnet.so", false},
		{"scaffolding-off", "python3 tools/check_test_scaffolding_boundary.py --build g-off --shared g-off/bin/librocketnet.so --archive g-off/librocketnet.a", false},
		{"scaffolding-red", "python3 tools/check_test_scaffolding_boundary.py --red-controls", false},

		// main's production build for the comparison
		{"main-wt", "git worktree add -f ../main-wt 76edea48e", false},
		{"cfg-main-off", "cd ../main-wt && cmake -S . -B ../main-off -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/home/glenn/rowan-jobs/v12-wipe/main-off/bin -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=/home/glenn/rowan-jobs/v12-wipe/main-off/bin -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF", false},
		{"build-main-off", res + "cmake --build ../main-off --target rocketnet rocketnet_shared --parallel 4", true},

		// Release DEV=ON
		{"build-rel", res + "cmake --build build-rel --parallel 4", true},
		{"unit-rel", res + "build-rel/bin/test", false},
		{"no-sockets", "bash tools/test_check_unit_no_sockets.sh && bash tools/check_unit_no_sockets.sh build-rel/bin/test", false},
		{"functional-rel", res + "build-rel/bin/functional", false},
		{"dst", "want=\"" + trace + "\"; " + res + "build-rel/bin/dst --seeds 1-50 > ../logs/dst-out.log 2>&1; tail -n 1 ../logs/dst-out.log; ! grep -q '^FAIL' ../logs/dst-out.log && [ \"$(tail -n 1 ../logs/dst-out.log)\" = \"$want\" ]", false},
		{"memory", "python3 tools/check_memory.py build-rel/bin/transport_memory", false},
		{"source-audit", "build-rel/bin/source_audit", false},
		{"nightly-rss", "ROCKETNET_TEST_NIGHTLY=1 " + res + "build-rel/bin/test --enforce-rss", false},

		// Debug DEV=ON
		{"cfg-dbg", cfg("g-dbg", "-DCMAKE_BUILD_TYPE=Debug -DROCKETNET_DEV=ON"), false},
		{"build-dbg", res + "cmake --build g-dbg --target test functional test_wide --parallel 4", true},
		{"unit-dbg", res + "g-dbg/bin/test", false},
		{"functional-dbg", res + "g-dbg/bin/functional", false},
		{"wide-dbg", res + "g-dbg/bin/test_wide", false},

		// clang Debug, as nightly.yml
		{"cfg-clang", cfg("g-clang", "-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"), false},
		{"build-clang", res + "cmake --build g-clang --parallel 4", true},
		{"unit-clang", res + "g-clang/bin/test", false},
		{"functional-clang", res + "g-clang/bin/functional", false},

		// TSan exactly as nightly.yml
		{"cfg-tsan", cfg("g-tsan", tsanFlags), false},
		{"build-tsan", res + "cmake --build g-tsan --target test functional --parallel 4", true},
		{"unit-tsan", tsanEnv + res + "setarch $(uname -m) -R g-tsan/bin/test", false},
		{"functional-tsan", tsanEnv + res + "setarch $(uname -m) -R g-tsan/bin/functional", false},

		// ASan + UBSan as ci.yml
		{"cfg-asan", cfg("g-asan", asanFlags), false},
		{"build-asan", res + "cmake --build g-asan --target test functional test_lifecycle test_transport test_sodium --parallel 4", true},
		{"unit-asan", res + "g-asan/bin/test", false},
		{"sodium-asan", res + "g-asan/bin/test_sodium", false},
	}

	only := map[string]bool{}
	for _, a := range os.Args[1:] {
		only[a] = true
	}

	for _, s := range all {
		if len(only) > 0 && !only[s.name] {
			continue
		}
		log := logs + "/" + s.name + ".log"
		remote := "cd " + repo + " && { " + s.cmd + " ; } > " + log + " 2>&1"
		err := exec.Command("ssh", "vision", remote).Run()
		line := "ok   " + s.name
		if err != nil {
			line = "FAIL " + s.name + " (" + log + ")"
		}
		if s.warn {
			out, _ := exec.Command("ssh", "vision", "grep -c 'warning:' "+log).Output()
			line += " warnings=" + strings.TrimSpace(string(out))
		}
		fmt.Println(line)
		if err != nil {
			out, _ := exec.Command("ssh", "vision", "tail -n 8 "+log).Output()
			fmt.Print(string(out))
		}
	}
	fmt.Println("done")
}
