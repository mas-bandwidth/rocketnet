#!/usr/bin/env bash
# Exercise the real tracer: an exec'd child must not evade the socket gate, and
# a thread start, a sleep outside the suite runner, or a process started by a
# case's forked child goes red, while the runner's own spawning and polling
# sleep stay green.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
cat > "$tmp/fixture.c" <<'C'
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static void *idle(void *arg) { return arg; }
static int reap(pid_t pid) {
    int status;
    if (pid < 0) return 3;
    if (waitpid(pid, &status, 0) != pid) return 5;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 6;
}
static int run(const char *self, const char *child) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(self, self, child, (char *)0);
        _exit(4);
    }
    return reap(pid);
}
int main(int argc, char **argv) {
    if (argc == 2) {
        if (strcmp(argv[1], "socket-child") == 0) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return 2;
            close(fd);
            return 0;
        }
        if (strcmp(argv[1], "thread-child") == 0) {
            pthread_t thread;
            if (pthread_create(&thread, NULL, idle, NULL) != 0) return 2;
            return pthread_join(thread, NULL) == 0 ? 0 : 2;
        }
        if (strcmp(argv[1], "sleep-child") == 0) {
            struct timespec t = { 0, 1000000 };
            return nanosleep(&t, NULL) == 0 ? 0 : 2;
        }
        pid_t pid = fork();
        if (pid == 0) {
            /* the worker's forked case child; in casefork-child it starts a process */
            if (strcmp(argv[1], "casefork-child") == 0) {
                pid_t grandchild = fork();
                if (grandchild == 0) _exit(0);
                _exit(reap(grandchild));
            }
            _exit(0);
        }
        return reap(pid);
    }
    const char *mode = getenv("RN_SOCKET_CHECK_MODE");
    if (mode && strcmp(mode, "fail") == 0) return 7;
    if (mode && strcmp(mode, "clean") == 0) {
        /* the runner's own machinery: a polling sleep, a spawned worker that forks its case */
        struct timespec t = { 0, 1000000 };
        if (nanosleep(&t, NULL) != 0) return 2;
        return run(argv[0], "worker-child");
    }
    if (mode && strcmp(mode, "child") == 0) return run(argv[0], "socket-child");
    if (mode && strcmp(mode, "thread") == 0) return run(argv[0], "thread-child");
    if (mode && strcmp(mode, "sleep") == 0) return run(argv[0], "sleep-child");
    if (mode && strcmp(mode, "casefork") == 0) return run(argv[0], "casefork-child");
    return 0;
}
C
"${CC:-cc}" -pthread "$tmp/fixture.c" -o "$tmp/fixture"
RN_SOCKET_CHECK_MODE=clean bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" > "$tmp/clean.log"
expect_red() {
    status=0
    RN_SOCKET_CHECK_MODE=$1 bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" > "$tmp/$1.log" || status=$?
    test "$status" = 1 || { echo "FAIL test_check_unit_no_sockets: mode $1 exited $status, expected 1"; cat "$tmp/$1.log"; exit 1; }
    grep -F "$2" "$tmp/$1.log" || { echo "FAIL test_check_unit_no_sockets: mode $1 did not print: $2"; cat "$tmp/$1.log"; exit 1; }
}
expect_red child '1 socket() call(s); first opener socket-child: socket('
expect_red thread '1 thread start(s); first thread-child: clone'
expect_red sleep '1 real sleep(s) outside the suite runner; first sleep-child: '
expect_red casefork '1 process start(s) by a case; first casefork-child: clone'
status=0
RN_SOCKET_CHECK_MODE=fail bash "$root/tools/check_unit_no_sockets.sh" "$tmp/fixture" > "$tmp/fail.log" || status=$?
test "$status" = 7
grep -F 'exited 7' "$tmp/fail.log"
echo 'Socket gate controls passed: clean runner, exec child socket, thread, sleep, case process start, and failing suite.'
