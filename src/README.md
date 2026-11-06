# src/

Our code for the psDoom revival. The vendored Doom engine it builds on lives separately in
[`../third_party/`](../third_party/) — `src/` is psDoom's own code only.

## Layout

| Path | Purpose |
|---|---|
| `app/macos/` | The macOS `.app` launcher (`launcher_main.c`, `Info.plist.in`). Locates an IWAD and `exec()`s the engine binary inside the bundle. |
| `psdoom/` | Process↔monster game logic — `proc_select` (triage: filter/rank/truncate, fork-burst child counts) and `psdoom` (spawn/reconcile, memory/CPU classification, live re-classification with hysteresis + teleport-fog morph, fork-bomb swarms, wound→renice / kill→signal, labels), plus the options + menu modules. |
| `platform/` | Backend-neutral process layer — `proc_types.h` (the `psd_proc_t` record), `proc_backend.{h,c}` (the enumerate/uid/renice/kill/lookup vtable, active-backend selection, and the pid-reuse-checked `psd_proc_renice_checked`/`psd_proc_kill_checked` the game acts through), `proc_script.{h,c}` (an external-script backend wired via `PSDOOM_POLL_CMD`/`PSDOOM_RESPOND_CMD` — see [`../docs/script-backend.md`](../docs/script-backend.md)), and `proc_fake.{h,c}` (a scriptable in-memory backend for tests). |
| `platform/macos/` | Native macOS backend — `sysctl` enumeration, `proc_pid_rusage` footprint/CPU, `setpriority` renice, `kill`; registers `proc_macos_backend`. |

Engine-free policy (triage + backend plumbing) is unit-tested off-machine in
[`../tests/`](../tests/) against the fake backend (`ctest --test-dir build`).

See [`../docs/architecture.md`](../docs/architecture.md) for the engine hook points, module API,
and build; and [`../docs/handoff.md`](../docs/handoff.md) for project background and history.
