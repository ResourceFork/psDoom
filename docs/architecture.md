# psDoom — architecture & features

psDoom turns the processes running on your machine into Doom monsters: each process is a monster
in the E1M1 courtyard, wounding it renices the process, and killing it signals it. This document
describes how the process-management layer attaches to the vendored Crispy Doom engine, the
modules it is built from, and the features it ships. For the project's origin and history, see
[`handoff.md`](handoff.md).

The `.app` builds and runs on the vendored engine, which carries only a thin set of psDoom hooks
— a few `mobj_t` fields plus one-line call-sites, all recorded in
[`../third_party/README.md`](../third_party/README.md); the rest of psDoom lives in `src/`.

## Repository layout

```
third_party/crispy-doom/   vendored engine, kept pristine (provenance: third_party/README.md)
src/app/macos/             the .app launcher (launcher_main.c + Info.plist.in)
src/psdoom/                process<->monster game logic + triage
src/platform/              backend interface + script/fake backends (proc_backend/proc_script/proc_fake)
src/platform/macos/        native sysctl/renice/kill backend
scripts/                   reference backend scripts (process poller + responder)
examples/                  example script backends (demo, docker)
tests/                     off-machine host tests (triage + backend plumbing)
CMakeLists.txt             single build: engine + psDoom.app bundle
docs/                      architecture.md (this), script-backend.md, handoff.md, original/ (archive)
wads/                      runtime IWAD (gitignored, not GPL)
```

## Architecture

Process data flows through three isolated layers — OS → policy → game — so each concern can be
reasoned about (and the triage tested) on its own:

```
backend           ->   proc_select         ->   psdoom
(read the OS,          (filter/rank/             (turn the curated
 or a script)          truncate)                 collection into monsters)
```

### Modules

- `src/platform/` — the OS-facing layer, behind a backend interface (no engine, no policy):
  - `proc_backend.{h,c}` / `proc_types.h` — the four-operation backend **vtable**
    (`list` / `current_uid` / `renice` / `kill`) over a backend-neutral `psd_proc_t`. The rest of
    psDoom calls `proc_backend()->…`, so the OS is swappable.
  - `macos/proc_macos.c` — the native backend: enumerate via `sysctl(KERN_PROC_ALL)` (pid, ppid,
    uid, daemon flag, short name), memory/CPU via `proc_pid_rusage`, renice via `setpriority`,
    kill via `kill(pid, SIGTERM)`. Registers `proc_macos_backend` (the default).
  - `proc_script.{h,c}` — an external-script backend that shells out to user commands; see the
    [Script backend](#external-script-backend) feature and [`script-backend.md`](script-backend.md).
  - `proc_fake.{h,c}` — a scriptable in-memory backend for the off-machine tests.
- `src/psdoom/` — game-side logic:
  - `proc_select.{c,h}` — the process reader/evaluator (no engine deps, no game state).
    `psd_select_collect()` acquires the live list and returns a curated, ranked, truncated
    collection of `psd_proc_t`; the pure `psd_select_triage()` does the filter+rank+truncate so
    the policy is testable in isolation. Owns the safety exclusions (session-critical name
    deny-list + launcher-chain `ppid` walk) and the relevance ranking (interactive/tty processes
    score highest; a tunable noise-substring list sinks system helpers).
  - `psdoom.{c,h}` — the monster creator. Consumes `proc_select`'s collection, reconciles
    monsters (spawn/retire), draws labels, classifies by memory footprint or CPU load, re-grades
    live monsters, and spawns fork-bomb swarms. Engine-facing API: `psdoom_init`, `psdoom_sync`,
    `psdoom_wound(mobj)`, `psdoom_kill(mobj)`, `psdoom_draw_label(...)`.
  - `psdoom_options.{h,c}` — user-tunable settings (kill policy, target-all-users, show-labels,
    show-pid, label distance, classify-by, max-live-monsters), the policy getters the game asks,
    and `-psdoom-*` CLI parsing. Engine-free; the ints are bound to the config file (d_main.c).
  - `psdoom_menu.{h,c}` — the in-game psDoom options page, fully isolated from the engine's menus.
    `m_menu.c` holds only a one-line "psDoom" entry (`M_PsDoom`) as the doorway.

### Design principle: minimal engine diff

Keep the upstream diff minimal. psDoom modules are compiled into the engine's `crispy-doom`
target via `target_sources(... PRIVATE ...)` from the **top-level** `CMakeLists.txt`, so the
vendored `third_party/crispy-doom/` tree stays byte-identical to upstream. Only three kinds of
change land in the vendored tree, and each is recorded in `third_party/README.md`:

1. a few `mobj_t` fields,
2. thin hook *call-sites* (one-liners into our module),
3. (if unavoidable) minimal build glue.

## Engine integration (hook points)

Vendored Crispy 7.1, `third_party/crispy-doom/src/doom/`:

| Concern | File : symbol (line) | Hook |
|---|---|---|
| Per-monster state | `p_mobj.h` : `mobj_t` struct (211–300) | add fields `pid_t psd_pid; const char *psd_name;` before `} mobj_t;` |
| Spawn → process | `p_mobj.c` : `P_SpawnMapThing` (965) / `P_SpawnMobj` (745) | associate `MF_COUNTKILL` monsters with a process, or spawn-on-demand from the process list |
| Wound → renice | `p_inter.c` : `P_DamageMobj` (852) | if target is a process-monster and survives, `psdoom_wound(target)` → `setpriority` |
| Kill → kill | `p_inter.c` : `P_KillMobj` (728), called at line 971 | if target is a process-monster, `psdoom_kill(target)` → `kill(pid)` |
| Periodic sync | `p_tick.c` : `P_Ticker` (131) | throttled `psdoom_sync()` (~once/second) near `leveltime++`: add monsters for new procs, retire dead ones |
| Labels over monsters | `r_things.c` : `R_ProjectSprite` (558) / `R_DrawVisSprite` (469) | copy `psd_pid`/`psd_name` onto the `vissprite_t`, then `psdoom_draw_label(...)` draws them above the sprite (HUD font `hu_font[]` via `V_DrawPatch`; `M_WriteText` is static in Crispy) |
| One-time init | `d_main.c` : `D_DoomMain` (1449) | `psdoom_init()`: open backend, load config, default current-UID + allow/deny |

## Features

- **Engine & build.** Engine vendored (`third_party/crispy-doom`, Crispy 7.1); single CMake build
  producing a double-clickable `psDoom.app`; Xcode project generated from the same CMake.
- **Process → monster.** `psdoom_sync` spawns a monster per current-UID process at the E1M1
  courtyard; wound → `setpriority` renice; kill → `SIGTERM`. Killing a process-monster is
  remembered for the level (`psd_killed`, by pid+name) so it doesn't respawn on the next sync —
  essential when the kill policy leaves the real process running. The set clears on level restart.
- **Process labels.** Each process-monster's name (and, optionally, its PID) is drawn above its
  sprite using the HUD font; the renderer carries `psd_pid`/`psd_name` on the `vissprite_t`. PID
  is hidden by default to keep a crowded courtyard readable. Distant sprites are skipped; labels
  respect wall occlusion (drawn only if the sprite rendered a pixel, via `psd_col_drawn`) and are
  offset by the 3D view-window origin so they stay aligned at reduced screen sizes.
- **Selection / triage** (`proc_select`). An isolated, engine-free reader returns a curated,
  ranked, truncated collection. It owns the safety exclusions (session-critical deny-list +
  launcher-chain `ppid` walk) and a relevance ranking (interactive/tty first; a tunable
  noise-substring list sinks system helpers), so the most relevant processes survive truncation
  to the candidate cap. The triage core (`psd_select_triage`) is a pure function over a snapshot.
- **Resource-based classification.** A process's memory footprint (or CPU load — see below) drives
  monster toughness via a grounded ladder (Zombieman → Shotgun Guy → Imp → Pinky → Hell Knight →
  Baron → Cyberdemon; tunable). A heavier monster takes more hits, and each non-fatal hit renices,
  so a hog is literally harder to kill. WAD-aware: it checks each type's sprite is loaded and
  degrades to a drawable type otherwise, so the shareware IWAD caps at Baron instead of crashing.
- **Selectable classifier (memory / CPU).** A "Classify by" option chooses whether monster *type*
  is sized by memory footprint (default) or CPU load. CPU load is a true rate — cumulative CPU
  time delta over the monotonic-clock interval between syncs (100% = one full core; multi-threaded
  hogs exceed 100). A separate CPU-tier table mirrors the memory ladder. (Apple Silicon note:
  `proc_pid_rusage` reports CPU time in mach absolute-time units, converted via
  `mach_timebase_info` before comparison.)
- **Live re-classification (hysteresis + morph fog).** Each sync a monster re-grades to the tier
  its *current* footprint/CPU earns, gated by an asymmetric hold (promote ~2 syncs, demote ~5, by
  `spawnhealth`) so it never flaps. `PSD_MorphMonster` swaps type/size, scales health to preserve
  the damage fraction, defers a size increase that would clip (`P_CheckPosition`), and pops an
  `MT_TFOG` flash + teleport cue. (See [design notes](#2-live-re-classification-with-hysteresis--morph-fog).)
- **Fork-bomb swarms.** A parent whose live child count spikes (a `fork()` storm) spawns a capped
  swarm of inert Lost Souls around its monster, degraded to a drawable type on the shareware WAD.
  The souls are cosmetic threats the player clears by shooting; they never signal anything. (See
  [design notes](#3-fork-bomb-swarms).)
- **Backends behind a vtable.** The OS layer is a four-op interface (`proc_backend_t`) with a
  native macOS implementation, a scriptable fake (for tests), and an external-script backend.
  (See [design notes](#1-platform-backend-interface--fake-backend).)
- **External-script backend.** Setting `PSDOOM_POLL_CMD` wires psDoom to *anything* you can
  enumerate from a script — containers, queues, build jobs — not just OS processes; `weight`/
  `load`/`parent` map onto the existing classifier, so classification, re-classification and
  swarms all work for those entities. Contract: [`script-backend.md`](script-backend.md);
  reference + example scripts in [`../scripts/`](../scripts/) and [`../examples/`](../examples/).
  (See [design notes](#4-external-script-backend).)
- **In-game options menu.** A dedicated psDoom page (reached from Options) with kill policy
  (live / renice-only / simulate), target-all-users, show-labels, label-distance, classify-by, and
  a max-live-monsters slider (5..35). All menu data/drawing/toggles live in `src/psdoom/`; settings
  persist in the config file and can be overridden with `-psdoom-safe` / `-psdoom-simulate` /
  `-psdoom-live` / `-psdoom-allusers` / `-psdoom-nolabels` / `-psdoom-cpu`. Default policy is
  **renice-only** (safe); real killing is opt-in (the menu shows live mode in red).
- **Live-monster cap & placement.** `psdoom_sync` spawns up to N *live* process-monsters
  (corpses don't count) so the courtyard stays passable; the candidate pool stays relevance-
  ordered so the cap keeps the most relevant on screen. Monsters spawn into the first *free*
  courtyard cell (grid-search via `P_CheckPosition`), so the most-relevant processes reliably
  appear and big monsters get room; they wander once active.

## Design notes

Deeper rationale, approach, and engine-diff impact for the most recent features. They build on
the three-layer pipeline; the backend interface is the foundation the others (and the
external-script backend) plug into.

### 1. Platform backend interface + fake backend

**Why:** the OS layer is a four-operation contract
(`proc_macos_list` / `proc_macos_current_uid` / `proc_macos_renice` / `proc_macos_kill`).
Lifting it behind a vtable unlocks (a) a deterministic *fake* backend so the pure triage and
classification policy can be unit-tested off-machine, and (b) a future Linux backend
(`/proc`, `setpriority`, `kill`) with zero changes above the interface.

**Design:**

- `src/platform/proc_types.h` — backend-neutral `psd_proc_t` + `PSD_PROC_NAME_MAX`.
- `src/platform/proc_backend.h` — the vtable and accessor:
  ```c
  typedef struct {
      int          (*list)(psd_proc_t *out, int max);
      unsigned int (*current_uid)(void);
      void         (*renice)(int pid, int nice_delta);
      int          (*kill)(int pid);
  } proc_backend_t;
  const proc_backend_t *proc_backend(void);        /* current backend      */
  void                  proc_backend_set(const proc_backend_t *b); /* override */
  ```
- `src/platform/proc_backend.c` — holds the current backend pointer; defaults to
  `proc_macos_backend` on Apple. `proc_backend_set` lets a test (or a future replay backend)
  swap it.
- `proc_macos.c` — exports `const proc_backend_t proc_macos_backend`; `proc_select.c` / `psdoom.c`
  call `proc_backend()->list(...)` etc.
- `src/platform/proc_fake.c` — serves a scripted array of snapshots (one per `list()` call) and
  logs `renice`/`kill` for assertions. Drives a host test (`tests/`, its own CMake target, *not*
  linked into the engine or the `.app`).

**Diff impact:** none in the vendored engine — a pure `src/` refactor + new test target.

### 2. Live re-classification with hysteresis + morph fog

**Why:** `psd_proc_t` already carries a *live* `footprint` and `cpu_percent` recomputed every
`list()` call, but classification originally ran only once, at spawn, so a process that later
spiked or idled kept its original monster type. Re-grading the live monster makes a runaway
process visibly *grow angrier*.

**Design (all in `psdoom.c`, keyed by `psd_pid` — no new engine fields):**

- Per-monster hysteresis side table `psd_grade_t { int pid; mobjtype_t pending; int streak; }`,
  compacted each sync against the live set.
- Each sync, for every live process-monster in the curated set: `desired = PSD_ClassifyMonster(p)`.
  If it equals the current type, clear the streak; otherwise count consecutive syncs voting for
  the same `desired` and commit the morph once a hold threshold is met. **Asymmetric holds** —
  promote fast (`PSD_REGRADE_UP_HOLD`, ~2 syncs), demote slow (`PSD_REGRADE_DOWN_HOLD`, ~5 syncs),
  compared by `mobjinfo[].spawnhealth` — so spikes show immediately but brief dips don't shrink a
  monster. This defeats frame-to-frame flapping.
- **Morph** (`PSD_MorphMonster`): target type routed through `PSD_ClassifyByTiers` (always
  WAD-drawable). Swap `type`/`info`/`radius`/`height`/`flags` (re-clear `MF_COUNTKILL`), scale
  `health` to the new `spawnhealth` (a half-dead Imp doesn't become a full-HP Baron), and
  `P_SetMobjState` to the see-state. A radius *increase* is gated by `P_CheckPosition`; if it
  would clip, the morph is deferred (streak kept) and retried next sync.
- **Morph fog:** spawn `MT_TFOG` + play `sfx_telept` on a successful morph (gated by
  `PSD_MonsterDrawable(MT_TFOG)`), so the change reads as a deliberate teleport-in.

Re-classification honors the "Classify by" option automatically: dynamic in CPU mode, slow-moving
in memory mode.

**Diff impact:** none in the vendored engine — reuses `P_SpawnMobj` / `P_SetMobjState` /
`P_CheckPosition`.

### 3. Fork-bomb swarms

**Why:** `ppid` is in every `psd_proc_t`, so the parent/child graph is available each sync. A
process whose child count climbs fast is a fork bomb; a swarm of Lost Souls around its monster is
both thematic and informative.

**Design:**

- **Detection** runs on the *raw* snapshot (children are filtered out of the curated set):
  `proc_select` exposes `psd_select_child_count(pid)` over the last raw snapshot. A burst is
  `child_count - prev >= PSD_FORK_BURST` (e.g. +5 in one sync). Prev-counts and a per-parent
  cooldown are plain pid-keyed tables (no mobj pointers, so nothing can dangle).
- **Swarm:** spawn up to `min(burst, PSD_SWARM_PER_BURST)` Lost Souls near the parent monster,
  capped overall by `PSD_MAX_SWARM_LIVE`. Type `MT_SKULL`, degraded via `PSD_MonsterDrawable` (the
  shareware IWAD is Episode 1 only — no Lost Soul sprite — so it falls back to a drawable grunt).
- **Safety / lifetime:** souls are spawned **inert** — tagged `psd_pid = PSD_SWARM_PID` (-1) so
  every process path ignores them (`psdoom_wound`/`psdoom_kill` gate on `psd_pid > 1` / `<= 1`;
  reconcile / live-count / label draw are tightened to `psd_pid > 0`). A soul never signals or
  renices anything; the player just clears it. `psdoom_kill` decrements a live-soul counter when a
  tagged soul dies. Per-parent cooldown + per-burst cap + global ceiling bound accumulation.

**Diff impact:** none in the vendored engine — Lost Souls already exist; detection is pure `src/`
logic, scriptable against the fake backend.

### 4. External-script backend

**Why:** with the backend behind a vtable, the OS isn't the only thing psDoom can represent. A
backend that shells out to user commands lets anyone wire psDoom to arbitrary systems by writing a
poll script and a response script. Because `weight`/`load`/`parent` map onto the existing
classifier, re-classification and fork swarms work for those entities for free.

**Design** (`src/platform/proc_script.{c,h}`, no engine edits):

- `list()` runs `PSDOOM_POLL_CMD` (via the shell, so pipelines work), captures stdout with a
  timeout so a slow script never freezes the game tick, and parses it; `renice`/`kill` run
  `PSDOOM_RESPOND_CMD` *directly* (no shell — entity labels/ids can't be interpreted as syntax),
  double-forked so they neither block nor leak zombies. `psdoom_init` installs it when
  `PSDOOM_POLL_CMD` is set, else the native backend stays.
- The pure parser (`psd_script_parse`) is engine/OS-free and unit-tested; the host test also
  drives a real poll/respond subprocess round-trip.
- **The wire contract is documented authoritatively in [`script-backend.md`](script-backend.md)**
  — poll stdout is tab-separated `key=value` (`id`/`parent`/`label`/`weight`/`load`/`flags`,
  unknown keys ignored for forward compatibility); responses are `<verb> <id> [key=value...]` with
  `verb` extensible.
- **Safety:** the native protected-process filter is process-specific and does not apply to
  arbitrary entities, so in script mode the script owns what is safe to enumerate and act on.
  Responses are exec'd without a shell to keep hostile labels inert.

**Diff impact:** none in the vendored engine — one new backend module + the env-driven install
call in `psdoom_init`.

## Build

Single top-level CMake project (out-of-source, keeps `third_party/` pristine):

```bash
cmake -S . -B build
cmake --build build -j
open build/psDoom.app
```

Xcode project, generated from the same CMake:

```bash
cmake -G Xcode -S . -B build-xcode
open build-xcode/psDoom.xcodeproj    # select the "psDoom" scheme, then Run
```

Off-machine tests (triage + backend plumbing, against the fake/script backends):

```bash
cmake --build build --target psdoom_tests
ctest --test-dir build --output-on-failure
```

- **Dependencies** (Homebrew): `sdl2 sdl2_mixer libsamplerate libpng fluid-synth` (`sdl2_net`
  disabled — no netplay).
- Verified: produces a Mach-O arm64 `psDoom.app` that boots to "DOOM Shareware" on Apple Silicon.

## Packaging (macOS .app)

- The bundle's executable is a small compiled launcher (`src/app/macos/launcher_main.c`) — a real
  Mach-O, so the bundle is signable. It `exec()`s `Contents/MacOS/crispy-doom`.
- **IWAD discovery:** at build time, if `wads/DOOM1.WAD` exists it is copied into the app's
  `Resources/`. At launch the launcher sets `DOOMWADPATH` (unless the caller already set
  `DOOMWADDIR`/`DOOMWADPATH`) to search, in order: the app's `Contents/Resources/`, then
  `~/Library/Application Support/psDoom/`.
- **Distribution follow-ups** (not needed to run locally; future home: `packaging/macos/`):
  - Bundle the Homebrew dylibs (SDL2 / SDL2_mixer / libsamplerate / libpng / FluidSynth) into
    `Contents/Frameworks` and rewrite install names (`install_name_tool` / `dylibbundler`).
  - Developer ID signing + notarization so Gatekeeper allows it after download.
  - An app icon (`.icns`) — none is set yet.

## Safety model

- Default to the **current UID's** processes only; system-wide requires explicit opt-in.
- **Protected-process filter:** session-critical UID-owned processes (`loginwindow`, `Dock`,
  `Finder`, `SystemUIServer`, …) and the game's launcher chain (shell/terminal, found by walking
  `ppid` up from the game) never spawn as monsters, so a stray shot can't signal them.
- **Kill policy** defaults to renice-only (recoverable); real killing (`SIGTERM`) is opt-in via
  the menu or `-psdoom-live`, with a simulate mode for no real effect at all.
- **Script mode** shifts safety to the script: the process-specific filter does not apply to
  arbitrary entities, and responses are exec'd without a shell so a hostile label stays inert.
  See [`script-backend.md`](script-backend.md#security-model).

## Roadmap

1. **Larger arena / custom WAD.** The courtyard (~11×7 cells at 56) tops out short of the slider's
   max of 35 on busy maps, and a Cyberdemon (radius 40) needs more elbow room than 56 spacing
   gives. A dedicated pen (as the original psDoom shipped) would lift both limits.
2. **Safety polish.** A kill confirmation / undo grace period (all-users mode and a non-lethal
   "safe" kill policy already exist via the psDoom options menu / `-psdoom-*` flags).

> Monster variety note: the bundled shareware `DOOM1.WAD` only has Episode 1 monster sprites, so
> the classifier caps at Baron of Hell. Point psDoom at a registered Doom or Doom II IWAD to get
> Hell Knights and Cyberdemons for your heaviest processes.

> Runtime note: a Doom or Doom II WAD is required to launch (not committed — not GPL). A shareware
> `DOOM1.WAD` in `wads/` is bundled automatically by the build.
