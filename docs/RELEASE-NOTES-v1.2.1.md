# RocketNet v1.2.1

Changes compared with the `v1.2.0` tag. A point release: no packet layout or
protocol version change, no public API or ABI change and no new counter (the
contract still holds 292 counters), and v1.2.1 endpoints interoperate with
every v1.x endpoint.

- Linux, system allocations after create: on a process that is refused
  real-time thread priority (any unprivileged process), every
  `rocketnet_client_connect` and every reconnect called the system allocator,
  outside the caller's allocator. The refused `pthread_setschedparam` call
  named its error with `strerror`, on every refusal although the warning is
  logged once, and glibc's `strerror` and `strerror_r` call `malloc` and
  `free` on every call (two of each, measured on glibc 2.43). The debug-level
  send failure lines did the same on every failed send, at any log level, and
  so did the gso server's segment refusal and the xdp transport's thread
  placement, stats and wakeup lines. An allocation-counting functional test
  reported it. Measured on a production build, unprivileged, with the library
  on its own allocator, from `rocketnet_client_connect` until connected:

  | | v1.2.0 | v1.2.1 |
  | --- | --- | --- |
  | client connect | 4 system allocator calls | 0 |
  | reconnect | 4 | 0 |

  (`pthread_create` itself allocates once for the client's first network
  thread in both versions; that is the C library's and is not counted above.)
- What changed: every failure line in the code a Linux build compiles now
  names an OS error as `errno <n> <NAME>`, the number and its `errno.h` name
  from a fixed table, formatted into the caller's bytes with no allocation
  and no locale. A lint rule (`tools/check_no_strerror.py`) refuses
  `strerror`, `strerror_r`, `strerror_l`, `perror` and `%m` in that code.
  Nothing else changes: a refused priority call is still counted in
  `thread_priority_refusals`, logged once per call kind, and never fails the
  caller.
- Log text, the one visible change: the text in parentheses or after the
  colon on these lines is `errno <n> <NAME>` where it was the C library's
  message, for example

  ```
  thread priority: pthread_setschedparam SCHED_RR 1 refused (errno 1 EPERM); the thread runs at its default priority
  ```

  where v1.2.0 printed `(Operation not permitted)`. On Linux that is the
  thread priority and affinity refusals, the `connect`, `sendto` and
  `sendmmsg` failure lines, the gso client and server send lines, and the xdp
  transport's pre-flight, create, stats and wakeup lines. The
  `connect (...) failed:` and `sendto (...) failed:` lines are shared code,
  so their text changes on macOS and the BSDs too; the macOS-only msgx lines
  keep the C library's text (Apple's `strerror` does not allocate for an
  errno it knows). A number the library has no name for prints as
  `errno <n> ?`. Anything that matches these lines by the old message text
  should match the errno name instead.
- One allocation is the C library's and stays: with the default log sink,
  the process's first `printf` allocates stdout's buffer (one `malloc`). It
  falls inside connect only when the connect's line is the first the process
  prints; a log callback of your own, or any earlier output, avoids it.

Upgrade: replace v1.2.0 with v1.2.1. No source change is needed.
