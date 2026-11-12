#!/usr/bin/env bash
# Exercise the real tracer: an exec'd child must not evade the socket gate.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
cat > "$tmp/fixture.c" <<'C'
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static int run_child(const char *self, const char *name) {
    pid_t pid = fork();
    if (pid < 0) return 3;
    if (pid == 0) {
        execl(self, self, name, (char *)0);
        _exit(4);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) return 5;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 6;
}
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "quiet-child") == 0) return 0;
    if (argc == 2) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return 2;
        close(fd);
        return 0;
    }
    const char *mode = getenv("RN_SOCKET_CHECK_MODE");
    if (mode && strcmp(mode, "fail") == 0) return 7;
    if (mode && strcmp(mode, "child") == 0) return run_child(argv[0], "socket-child");
    if (mode && strcmp(mode, "clean") == 0) return run_child(argv[0], "quiet-child");
    return 0;
}
C
"${CC:-cc}" "$tmp/fixture.c" -o "$tmp/fixture"
RN_SOCKET_CHECK_MODE=clean bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" > "$tmp/clean.log"
grep -F '(1 case workers)' "$tmp/clean.log"
# zero inputs: a run that starts no case worker checked nothing and is red
status=0
RN_SOCKET_CHECK_MODE=none bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" > "$tmp/none.log" || status=$?
test "$status" = 1
grep -F '0 cases ran' "$tmp/none.log"
status=0
RN_SOCKET_CHECK_MODE=child bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" > "$tmp/child.log" || status=$?
test "$status" = 1
grep -F '1 socket() call(s); first opener socket-child: socket(' "$tmp/child.log"
# arguments after the binary reach it (a shard runs as ./bin/test --shard i/2):
# one argument makes the fixture open a socket itself
status=0
RN_SOCKET_CHECK_MODE=clean bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" forwarded > "$tmp/forwarded.log" || status=$?
test "$status" = 1
grep -F '1 socket() call(s); first opener forwarded: socket(' "$tmp/forwarded.log"
status=0
RN_SOCKET_CHECK_MODE=fail bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" > "$tmp/fail.log" || status=$?
test "$status" = 7
grep -F 'exited 7' "$tmp/fail.log"
echo 'Socket gate controls passed: clean, zero cases, exec child, forwarded arguments, and failing suite.'
