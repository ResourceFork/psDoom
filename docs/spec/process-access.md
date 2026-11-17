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
   A poll that times out before its first line is slow, not empty: the last good list
   stands (no monster is read as exited) and the next poll starts. Only when no poll has
   answered yet, or on the 6th such timeout in a row, does it list nothing. A poll that
   cannot be run, or lists nothing because it exited non-zero or printed only
   `# summon error`, lists nothing at once (so does a poll that cannot even be started,
   for example when `fork` fails under a real fork bomb). Then there are no process monsters and no
   action. The latest reason is shown on the HUD and logged once per failure streak. A
   poll that times out after complete lines still uses those lines.
5. psDoom calls `kill()` in one place only: the script backend killing **its own** timed-out
   child process group, or its own poll still running when psDoom exits normally (not on
   a crash). `proc_pidinfo` is called only on `getpid()`, to list psDoom's own file
   descriptors.

`tests/no_native_process_access.cmake` scans `src/` for the obvious calls: `kill(` outside
that teardown, `killpg`, `sigqueue`, `setpriority`, `sysctl`, `proc_pid*`,
`proc_list*pids`, and `/bin/kill` or `renice` paths. It fails the tests if it finds any.
It is a tripwire, not a proof: it does not parse C or scan `third_party/`.
