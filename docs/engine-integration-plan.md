# psDoom on Crispy Doom — engine integration plan

How the psDoom process-management layer attaches to the vendored Crispy Doom engine. This is the
concrete first-increment plan; no engine code has been modified yet.

## Engine

- **Crispy Doom 7.1** vendored at `src/engine/crispy-doom/` (provenance in `src/engine/README.md`). GPL-2.0-or-later.
- **Build** (out-of-source, keeps the vendored tree pristine):
  ```bash
  cmake -S src/engine/crispy-doom -B build -DENABLE_SDL2_NET=Off
  cmake --build build -j
  ```
  Produces `build/src/crispy-doom` (verified: Mach-O arm64, "Crispy Doom 7.1.0").
- **Dependencies** (Homebrew): `sdl2 sdl2_mixer libsamplerate libpng fluid-synth` (`sdl2_net` optional, disabled — psDoom has no use for netplay).

## Design principle

Keep the upstream diff minimal. Only land in the vendored tree:
1. a few `mobj_t` fields,
2. thin hook *call-sites* (one-liners into our module),
3. CMake additions to compile our modules.

All real logic lives in new, isolated modules so the patch set stays auditable and re-appliable
when the engine is updated. Every engine edit gets recorded in `src/engine/README.md`.

## New modules

- `src/psdoom/` — game-side logic:
  - `psdoom.c` / `psdoom.h` — process↔monster registry, spawn/wound/kill policy, label text. Engine-facing API: `psdoom_init`, `psdoom_sync`, `psdoom_wound(mobj)`, `psdoom_kill(mobj)`.
- `src/platform/macos/` — native macOS process backend:
  - `proc_macos.c` / `proc_macos.h` — enumerate via `libproc` (`proc_listpids` / `proc_pidinfo`), renice via `setpriority(PRIO_PROCESS, pid, nice)`, kill via `kill(pid, SIGTERM)` escalating to `SIGKILL`.

## Hook points (vendored Crispy 7.1, `src/engine/crispy-doom/src/doom/`)

| Concern | File : symbol (line) | Hook |
|---|---|---|
| Per-monster state | `p_mobj.h` : `mobj_t` struct (211–300) | add fields `pid_t psd_pid; const char *psd_name;` before `} mobj_t;` |
| Spawn → process | `p_mobj.c` : `P_SpawnMapThing` (965) / `P_SpawnMobj` (745) | associate `MF_COUNTKILL` monsters with a process, or spawn-on-demand from the process list |
| Wound → renice | `p_inter.c` : `P_DamageMobj` (852) | if target is a process-monster and survives, `psdoom_wound(target)` → `setpriority` |
| Kill → kill | `p_inter.c` : `P_KillMobj` (728), called at line 971 | if target is a process-monster, `psdoom_kill(target)` → `kill(pid)` |
| Periodic sync | `p_tick.c` : `P_Ticker` (131) | throttled `psdoom_sync()` (~once/second) near `leveltime++`: add monsters for new procs, retire dead ones |
| Labels over monsters | `r_things.c` : `R_ProjectSprite` (558) / `R_DrawMasked` (1382) | draw `psd_name`/`psd_pid` above each process-monster sprite (text via `M_WriteText`) |
| One-time init | `d_main.c` : `D_DoomMain` (1449) | `psdoom_init()`: open backend, load config, default current-UID + allow/deny |

## Safety (built in from the start)

- Default to the **current UID's** processes only; system-wide requires explicit opt-in (`sudo`).
- **Allow/deny filter** so a stray shot can't signal a critical PID.
- Hide daemons / kernel tasks by default (a modern Mac runs hundreds of processes).
- Wounding = renice (recoverable); killing = a real signal with no confirmation — gate it behind the filter.

## CMake wiring

Add `src/psdoom` and `src/platform/macos` sources to the `crispy-doom` doom target (smallest
change to `src/engine/crispy-doom/src/CMakeLists.txt`), and record it in the psDoom-modifications
list in `src/engine/README.md`.

## First code increment (next session)

1. Add `mobj_t` fields + `psdoom_init/sync/wound/kill` **stubs** (no-ops) + call-sites; confirm the engine still builds and boots a WAD.
2. Implement `proc_macos` enumeration; map processes → spawned monsters (current UID only).
3. Wire wound→renice and kill→kill behind the safety filter.
4. Draw the process labels.
5. Generate the Xcode project via `cmake -G Xcode` once the integrated build is stable.

> Runtime note: a Doom or Doom II WAD is required to actually launch (not committed — not GPL).
