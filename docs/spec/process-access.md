# Spec: process access

1. psDoom (`src/`) never lists, renices or signals a process itself. There is no native
   backend.
2. The only path to real processes is Summoner. psDoom runs the bundled copy,
   `<app>/Contents/Resources/summoner/summon`, unless `PSDOOM_POLL_CMD` /
   `PSDOOM_RESPOND_CMD` override it for development.
3. psDoom runs `summon` with a `python3` it finds without a shell. It tries
   `/opt/homebrew/bin/python3`, `/usr/local/bin/python3`, the Command Line Tools', then
   Xcode's. It never uses the `/usr/bin/python3` install shim.
4. If Summoner is missing, no `python3` is found, or a poll gets no answer, the backend is
   `proc_none_backend`: no process monsters and no action. The reason is logged once and
   shown once on the HUD.
5. psDoom calls `kill()` in one place only: the script backend killing **its own** timed-out
   child process group. `proc_pidinfo` is called only on `getpid()`, to list psDoom's own
   file descriptors.

`tests/no_native_process_access.cmake` scans `src/` for anything else and fails the build's
tests if it finds any.
