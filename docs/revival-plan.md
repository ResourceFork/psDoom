# psDoom on Crispy Doom — revival plan

How the psDoom process-management layer attaches to the vendored Crispy Doom engine, and how the
macOS app is built. The engine is bootstrapped and the `.app` builds/runs **stock** Crispy Doom;
no engine code has been modified for psDoom yet.

## Repository layout

```
third_party/crispy-doom/   vendored engine, kept pristine (provenance: third_party/README.md)
src/app/macos/             the .app launcher (launcher_main.c + Info.plist.in)
src/psdoom/                process<->monster game logic + triage
src/platform/macos/        native sysctl/renice/kill backend
CMakeLists.txt             single build: engine + psDoom.app bundle
docs/                      handoff.md (origin context), revival-plan.md (this), original/ (archive)
wads/                      runtime IWAD (gitignored, not GPL)
```

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

- **Dependencies** (Homebrew): `sdl2 sdl2_mixer libsamplerate libpng fluid-synth` (`sdl2_net` disabled — no netplay).
- Verified: produces a Mach-O arm64 `psDoom.app` that boots to "DOOM Shareware" on Apple Silicon.

## Design principle

Keep the upstream diff minimal. Prefer adding our modules to the engine's `crispy-doom` target via
`target_sources(crispy-doom PRIVATE ...)` from the **top-level** `CMakeLists.txt`, so the vendored
`third_party/crispy-doom/` tree stays byte-identical to upstream. Only three kinds of change land
in the vendored tree, and each is recorded in `third_party/README.md`:

1. a few `mobj_t` fields,
2. thin hook *call-sites* (one-liners into our module),
3. (if unavoidable) minimal build glue.

## New modules

The process data flows through three isolated layers, OS -> policy -> game, so
each concern can be reasoned about (and the triage tested) on its own:

```
proc_macos        ->   proc_select         ->   psdoom
(read the OS)          (filter/rank/             (turn the curated
                        truncate)                 collection into monsters)
```

- `src/platform/macos/` — native macOS process backend (no engine, no policy):
  - `proc_macos.c` / `proc_macos.h` — enumerate via `sysctl(KERN_PROC_ALL)` (pid, ppid, uid,
    daemon flag, short name), renice via `setpriority(PRIO_PROCESS, pid, nice)`, kill via
    `kill(pid, SIGTERM)`.
- `src/psdoom/` — game-side logic:
  - `proc_select.c` / `proc_select.h` — the process reader/evaluator (no engine deps, no game
    state). `psd_select_collect()` acquires the live list and returns a curated, ranked,
    truncated collection of `psd_proc_t`; the pure `psd_select_triage()` does the
    filter+rank+truncate so the policy is testable in isolation. Owns the safety exclusions
    (session-critical name deny-list + launcher-chain `ppid` walk) and the relevance ranking
    (interactive/tty processes score highest; a tunable noise-substring list sinks system
    helpers) so the most relevant processes survive truncation.
  - `psdoom.c` / `psdoom.h` — the monster creator. Consumes `proc_select`'s collection and
    reconciles monsters (spawn/retire), draws labels, classifies by memory footprint or CPU load
    (per the "Classify by" option), and maps wound->renice / kill->SIGTERM. Engine-facing API:
    `psdoom_init`, `psdoom_sync`, `psdoom_wound(mobj)`, `psdoom_kill(mobj)`,
    `psdoom_draw_label(...)`.
  - `psdoom_options.{h,c}` — user-tunable settings (kill policy live/renice-only/simulate,
    target-all-users, show-labels, label distance, classify-by memory/CPU, max-live-monsters),
    the policy getters the game asks (`psdoom_should_kill` etc.), and `-psdoom-*` CLI parsing.
    Engine-free; the ints are bound to the config file (d_main.c) for persistence.
  - `psdoom_menu.{h,c}` — the in-game psDoom options page: its own menu definition, drawing and
    toggle wiring, fully isolated from the engine's menus. `m_menu.c` holds only a one-line
    "psDoom" entry in the Options menu (`M_PsDoom`) as the doorway.

## Hook points (vendored Crispy 7.1, `third_party/crispy-doom/src/doom/`)

| Concern | File : symbol (line) | Hook |
|---|---|---|
| Per-monster state | `p_mobj.h` : `mobj_t` struct (211–300) | add fields `pid_t psd_pid; const char *psd_name;` before `} mobj_t;` |
| Spawn → process | `p_mobj.c` : `P_SpawnMapThing` (965) / `P_SpawnMobj` (745) | associate `MF_COUNTKILL` monsters with a process, or spawn-on-demand from the process list |
| Wound → renice | `p_inter.c` : `P_DamageMobj` (852) | if target is a process-monster and survives, `psdoom_wound(target)` → `setpriority` |
| Kill → kill | `p_inter.c` : `P_KillMobj` (728), called at line 971 | if target is a process-monster, `psdoom_kill(target)` → `kill(pid)` |
| Periodic sync | `p_tick.c` : `P_Ticker` (131) | throttled `psdoom_sync()` (~once/second) near `leveltime++`: add monsters for new procs, retire dead ones |
| Labels over monsters | `r_things.c` : `R_ProjectSprite` (558) / `R_DrawVisSprite` (469) | copy `psd_pid`/`psd_name` onto the `vissprite_t`, then `psdoom_draw_label(...)` draws them above the sprite (HUD font `hu_font[]` via `V_DrawPatch`; `M_WriteText` is static in Crispy) |
| One-time init | `d_main.c` : `D_DoomMain` (1449) | `psdoom_init()`: open backend, load config, default current-UID + allow/deny |

## Safety (built in from the start)

- Default to the **current UID's** processes only; system-wide requires explicit opt-in (`sudo`).
- **Allow/deny filter** so a stray shot can't signal a critical PID.
- Hide daemons / kernel tasks by default (a modern Mac runs hundreds of processes).
- Wounding = renice (recoverable); killing = a real signal with no confirmation — gate it behind the filter.

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

## Status / next steps

- **Done:**
  - Engine vendored (`third_party/crispy-doom`, Crispy 7.1); single CMake build producing a
    double-clickable `psDoom.app`; Xcode project generated from the same CMake.
  - psDoom hooks wired into the engine (`mobj_t.psd_pid`/`psd_name` + call-sites).
  - `proc_macos` enumerates processes via `sysctl(KERN_PROC_ALL)`; `psdoom_sync` spawns a monster
    per current-UID process at the E1M1 courtyard (daemons -> demons, others -> shotgun guys);
    wound -> `setpriority` renice; kill -> `SIGTERM`. Verified on E1M1 (~35 process-monsters).
  - Process labels: each process-monster's PID and name are drawn above its sprite
    (`psdoom_draw_label` in `src/psdoom/psdoom.c`, HUD font; renderer carries `psd_pid`/`psd_name`
    on the `vissprite_t`). A scale gate skips distant sprites; placement/scale are tunable
    (`PSD_LABEL_MIN_SCALE`). Labels respect wall occlusion (drawn only if the sprite actually
    rendered a pixel, via `psd_col_drawn` in `R_DrawMaskedColumn`) and are offset by the 3D view
    window origin (`viewwindowx`/`viewwindowy`) so they stay aligned at reduced screen sizes.
  - Protected-process safety filter: session-critical UID-owned processes (`loginwindow`, `Dock`,
    `Finder`, `SystemUIServer`, ...) and the game's launcher chain (shell/terminal, found by
    walking `ppid` up from the game) never spawn as monsters, so a stray shot can't SIGTERM them.
    Also raised the per-sync process cap (`PSD_MAX_PROCS` 512 -> 4096): a busy desktop has >512
    processes, and the old cap silently dropped many (including those critical ones), which both
    hid processes and would have let the filter miss them.
  - Process-selection layer (`src/psdoom/proc_select.{h,c}`): an isolated, engine-free reader that
    acquires the live process list and returns a curated, ranked, truncated collection. It owns
    the safety exclusions and a relevance ranking (interactive/tty processes first; a tunable
    noise-substring list sinks system helpers) so the most relevant processes survive truncation
    to the candidate cap. `psdoom_sync` is now just the monster creator: it consumes the
    collection and reconciles monsters, with no triage logic of its own. The triage core
    (`psd_select_triage`) is a pure function over an input snapshot, so the policy can be
    exercised in isolation.
  - Resource-based monster classification: a process's memory footprint (read via
    `proc_pid_rusage`) drives monster toughness -- heavier process, bigger/harder monster.
    `PSD_ClassifyMonster` maps footprint tiers to a grounded ladder (Zombieman -> Shotgun Guy ->
    Imp -> Pinky -> Hell Knight -> Baron -> Cyberdemon; tunable table). Because a heavy monster
    takes more hits, and each non-fatal hit renices, a memory hog is literally harder to kill.
    Footprint also feeds the relevance ranking so heavy processes survive truncation. The
    classifier is WAD-aware: it checks each candidate monster's sprite is actually loaded
    (`sprites[].numframes`) and degrades to a lighter, drawable type otherwise -- so the
    shareware IWAD (no Hell Knight / Cyberdemon sprites) caps at Baron instead of crashing
    (`R_ProjectSprite` I_Error), while a registered Doom / Doom II WAD lights up the full ladder.
  - In-game options menu: a dedicated psDoom page (reached from Options) with kill policy
    (live / renice only / simulate), target-all-users, show-labels, label-distance and a
    max-live-monsters slider (5..35). All of the
    menu's data, drawing and toggles live in `src/psdoom/psdoom_menu.{c,h}` +
    `psdoom_options.{c,h}`, isolated from the engine's menus (`m_menu.c` gains only a one-line
    doorway). Settings persist in the config file and can be overridden with `-psdoom-safe` /
    `-psdoom-simulate` / `-psdoom-live` / `-psdoom-allusers` / `-psdoom-nolabels`. Default kill
    policy is **renice-only** (safe: wounding renices, but a monster's death never terminates the
    process); real killing is opt-in via the menu or `-psdoom-live`, and the menu shows the live
    mode in red. Killing a
    process-monster is remembered for the level (`psd_killed`, by pid+name), so it doesn't
    respawn on the next sync -- without this, a "killed" process in renice-only/simulate mode
    (still running) would just reappear a second later. The set clears on level restart.
  - Live-monster cap: a menu slider (`psdoom_monster_cap`, 5..35, default 10, persisted): 30+
    monsters made the E1M1 courtyard/hallways impassable, so `psdoom_sync` only spawns up to N
    *live* process-monsters at once (corpses don't count -- they're non-solid). The candidate
    pool (`PSD_CANDIDATE_CAP`, 64) stays relevance-ordered, so the cap keeps the most relevant
    processes on screen. Raising the cap takes effect next sync; lowering it stops new spawns and
    lets the count thin naturally (existing monsters aren't culled).
  - Placement: monsters now spawn into the first *free* courtyard cell, scanned in relevance order
    (`PSD_SpawnMonster` grid-search via `P_CheckPosition` + relocate), instead of a pid-hash cell.
    So the most-relevant processes reliably appear (no more losing monsters to pid collisions),
    and the wider grid spacing (56, was 40) fits Baron-sized monsters so the big memory-hogs
    actually spawn. Monsters wander once active, so this only sets the initial position.
  - Selectable classifier (memory / CPU load): a "Classify by" option (psDoom menu + persisted +
    `-psdoom-cpu`) chooses whether a process's monster *type* is sized by its memory footprint
    (default) or its CPU load. CPU load is a true rate -- the backend reads cumulative CPU time
    (`proc_pid_rusage`) and divides the delta by the monotonic-clock interval between syncs, so
    100% = one full core busy for the whole interval (a multi-threaded hog can exceed 100). The
    CPU sampler is primed in `psdoom_init` so CPU mode works from the first spawn rather than
    reporting every process idle. A separate CPU-tier table
    (5/25/60/120/250/500% -> Zombieman..Cyberdemon) mirrors the memory ladder; both share the
    WAD-aware degrade. Unit note: on Apple Silicon `proc_pid_rusage` reports CPU time in mach
    absolute-time units (~41.67 ns each, timebase 125/3), not nanoseconds, so the backend
    converts via `mach_timebase_info` before comparing against the `clock_gettime` wall clock
    (a no-op on Intel's 1/1 timebase). Without this a fully-pegged process read ~2% instead of
    ~100%.
    Limitations: classification happens once, at spawn time -- a monster doesn't re-grade if its
    process's CPU later spikes or idles (a process that's busy when first seen keeps that monster
    type for its life). The CPU tier thresholds are calibrated for 100 = one core and are tunable.
    Ranking is still memory-based, so the candidate pool surfaces notable + interactive processes,
    which CPU mode then sizes by live load.
- **Next:**
  1. CPU-driven *behavior* and live re-classification. The classifier above sizes the monster
     *type* once at spawn; the dynamic-behavior idea is still open -- e.g. a high-CPU process is
     more aggressive/faster, and monsters re-grade as their process's load changes. (The earlier
     concern that CPU would make *types* flap frame-to-frame is sidestepped by classifying only at
     spawn; live re-grading would need hysteresis to avoid that flapping.)
  2. Larger arena / custom WAD: the courtyard (~11x7 cells at 56) tops out short of the slider's
     max of 35 on busy maps, and a Cyberdemon (radius 40) needs more elbow room than 56 spacing
     gives. A dedicated pen (as the original psDoom shipped) would lift both limits.
  3. Safety polish: a kill confirmation / undo grace period (all-users mode and a non-lethal
     "safe" kill policy now exist via the psDoom options menu / `-psdoom-*` flags).

> Monster variety note: the bundled shareware `DOOM1.WAD` only has Episode 1 monster sprites, so
> the classifier caps at Baron of Hell. Point psDoom at a registered Doom or Doom II IWAD to get
> Hell Knights and Cyberdemons for your heaviest processes.

> Runtime note: a Doom or Doom II WAD is required to launch (not committed — not GPL). A shareware
> `DOOM1.WAD` in `wads/` is bundled automatically by the build.
