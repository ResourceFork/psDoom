# psDoom on Crispy Doom — revival plan

How the psDoom process-management layer attaches to the vendored Crispy Doom engine, and how the
macOS app is built. The engine is bootstrapped and the `.app` builds/runs **stock** Crispy Doom;
no engine code has been modified for psDoom yet.

## Repository layout

```
third_party/crispy-doom/   vendored engine, kept pristine (provenance: third_party/README.md)
src/app/macos/             the .app launcher (launcher_main.c + Info.plist.in)
src/psdoom/                process<->monster game logic        (to be written)
src/platform/macos/        native libproc/renice/kill backend  (to be written)
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

- `src/psdoom/` — game-side logic:
  - `psdoom.c` / `psdoom.h` — process↔monster registry, spawn/wound/kill policy, label text. Engine-facing API: `psdoom_init`, `psdoom_sync`, `psdoom_wound(mobj)`, `psdoom_kill(mobj)`.
- `src/platform/macos/` — native macOS process backend:
  - `proc_macos.c` / `proc_macos.h` — enumerate via `libproc` (`proc_listpids` / `proc_pidinfo`), renice via `setpriority(PRIO_PROCESS, pid, nice)`, kill via `kill(pid, SIGTERM)` escalating to `SIGKILL`.

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
- **Next:**
  1. Placement: the E1M1 courtyard only fits a few dozen monsters, so on a busy machine most
     processes collide and don't appear. Add a larger arena / custom WAD (as the original did)
     and/or daemon filtering.
  2. Safety polish: optional all-users mode (opt-in), and a kill confirmation / undo grace period.

> Runtime note: a Doom or Doom II WAD is required to launch (not committed — not GPL). A shareware
> `DOOM1.WAD` in `wads/` is bundled automatically by the build.
