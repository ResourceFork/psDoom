# Spec: process access

1. psDoom (`src/`) never lists, renices or signals a process itself. There is no native
   backend.
2. The only path to real processes is Summoner. psDoom runs the bundled copy,
   `<app>/Contents/Resources/summoner/summon`, unless `PSDOOM_POLL_CMD` /
   `PSDOOM_RESPOND_CMD` override it for development.
3. psDoom runs `summon` with a `python3` it finds without a shell. It tries
   `/opt/homebrew/bin/python3`, `/usr/local/bin/python3`, the Command Line Tools', then
   Xcode's. It never uses the `/usr/bin/python3` install shim.
4. If Summoner is missing or no `python3` is found, the backend is `proc_none_backend`.
   If a poll does not answer, or lists nothing because it exited non-zero or printed only
   `# summon error`, that poll lists nothing. Either way there are no process monsters and
   no action. The latest reason is shown on the HUD and logged once per failure streak. A
   poll that times out after complete lines still uses those lines.
5. psDoom calls `kill()` in one place only: the script backend killing **its own** timed-out
   child process group, or its own poll still running when psDoom quits. `proc_pidinfo` is called only on `getpid()`, to list psDoom's own
   file descriptors.

`tests/no_native_process_access.cmake` scans `src/` for the obvious calls: `kill(` outside
that teardown, `killpg`, `sigqueue`, `setpriority`, `sysctl`, `proc_pid*`,
`proc_list*pids`, and `/bin/kill` or `renice` paths. It fails the tests if it finds any.
It is a tripwire, not a proof: it does not parse C or scan `third_party/`.
