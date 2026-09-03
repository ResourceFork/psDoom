# psDoom

A revival of the early-2000s **Mac OS X / X11 port of psDoom** — the game that
turns Unix process management into Doom. Every running process shows up as a
monster labeled with its name and PID. Shoot or punch a monster and it acts on
the real process: wounding lowers its scheduling priority (renice), killing it
sends a kill signal.

Originally authored over 20 years ago and published at [**resourcefork.com**](https://www.resourcefork.com),
officially linked from the canonical psDooM site. This repo tracks the effort to
recover the original work and modernize it on a current Doom engine with a
native macOS process layer.

## What psDoom is

psDoom maps Unix process management onto Doom:

- **Process → monster.** Each running process is rendered as a monster, labeled
  with its name/PID.
- **Wound → renice.** Wounding a monster lowers the corresponding process's
  scheduling priority.
- **Kill → kill.** Killing a monster sends a kill signal to the process. No
  confirmation, no undo.

It's part novelty, part genuinely useful "interactive process management" demo.

## Lineage & attribution

- **Original concept:** Dennis Chao — LISA 2000 paper _"Doom as an Interface for
  Process Management."_
- **psDooM** (the SourceForge project): maintained by David Koppenhofer; based
  on XDoom (Udo Munk), itself based on id Software's GPL Doom source.
- **This work:** the Mac OS X / X11 port of that lineage, originally hosted at
  resourcefork.com.

Related modern revivals of the same "system-as-Doom" idea: **Kube DOOM** (2022,
kills Kubernetes pods the same way) and **psdoom-ng** (a Chocolate Doom
reimplementation).

## Original port (v1.0)

- **Platform:** any Mac running Mac OS X + X11.
- **Requirements:** an Ultimate Doom or Doom II WAD (retail or shareware).
- **Run:** under X11, `cd` into the psdoom folder and `./psdoom`.
- Shipped pre-compiled, with an explicit warning that killing a labeled monster
  kills the process with no "save your work?" prompt.

The original source is preserved in this repo as a longevity archive, so the
revival never depends on a third party staying online. It's kept two ways under
[`docs/original/`](docs/original/): the pristine
[`psdoom-src.zip`](docs/original/psdoom-src.zip) (byte-for-byte as recovered) and
the same archive extracted to [`psdoom-src/`](docs/original/psdoom-src/) for easy
browsing — the XDoom-based engine, the `xdlaunch` helper, and the original
`CREDITS`, `CHANGELOG`, and `COPYING` docs. The compiled app (`psDoom.sit`) also
survives in the Internet Archive but isn't committed yet; see
[`docs/original/README.md`](docs/original/README.md) for recovery details.

## Revival plan

A faithful, buildable psDoom on Apple Silicon, with the OS integration written
as a small native macOS module.

**Engine:** built on **Crispy Doom 7.1** (SDL2) — vanilla-accurate, builds on
Apple Silicon via Homebrew — vendored under [`third_party/`](third_party/). The
engine is bootstrapped and wrapped in a native macOS `.app`; the psDoom process
layer is the next step.

**The OS contract is tiny.** Prior art (psdoom-ng) reduces the whole game↔OS
integration to three operations:

| Operation | Game event      | Native macOS call                                                        |
| --------- | --------------- | ------------------------------------------------------------------------ |
| Enumerate | spawn monsters  | `proc_listpids` / `proc_pidinfo` (`<libproc.h>`) or `sysctl` `KERN_PROC` |
| Renice    | wound a monster | `setpriority(PRIO_PROCESS, pid, niceValue)`                              |
| Kill      | monster death   | `kill(pid, SIGTERM)`, escalate to `SIGKILL`                              |

**Things the revival has to design around:**

- **Permissions:** modern macOS only lets you signal/renice processes you own
  unless running as root. Default to the current UID's processes; system-wide
  requires `sudo`.
- **Safety:** it really does kill processes with no confirmation. An allow/deny
  filter and an "only my UID" default keep a stray BFG blast from nuking
  something important.
- **Process count:** a modern Mac runs hundreds of processes, so filtering (hide
  daemons by default) matters.

## Repository layout

```
third_party/crispy-doom/   vendored Crispy Doom engine (see third_party/README.md)
src/app/macos/             the .app launcher
src/psdoom/                process<->monster logic        (in progress)
src/platform/macos/        native libproc/renice/kill     (in progress)
tools/                     deterministic arena generator and independent WAD validator
CMakeLists.txt             single build: engine + generated arena + psDoom.app
docs/                      architecture.md, handoff.md, original/ (recovered v1.0 archive)
wads/                      user-supplied runtime IWAD (gitignored, not GPL)
```

## Build & run

Requires a user-supplied Doom IWAD. A shareware `DOOM1.WAD` placed in `wads/`
is copied into the app automatically; it remains gitignored and is not part of
psDoom. The build deterministically generates `psdoom-arena.wad`, validates it,
and bundles it in the app. Dependencies via Homebrew:
`brew install sdl2 sdl2_mixer libsamplerate libpng fluid-synth`.

```bash
cmake -S . -B build
cmake --build build -j
open build/psDoom.app
```

A normal app launch first presents the stock `DEMO1` attract demo on IWAD `E1M5`.
Process management is inactive during every demo. Choose **New Game**, then use the
normal episode, skill, and Nightmare-confirmation flow to load the one-time
`PSDINTRO` arena. Its exit loads the original `E1M1` from the IWAD, after which
campaign progression remains `E1M1` → `E1M2` and onward. Explicit map controls
such as `-file`, `-merge`, `-warp`, demos, or save loading suppress the bundled
intro. Existing `DOOMWADDIR` and `DOOMWADPATH` values continue to select the
IWAD search location.

The arena can also be generated and validated independently with Python 3.10
or newer. Loading an IWAD campaign save from the running intro derives level
ownership from the save before base geometry is set up, so the saved IWAD map
is restored instead of `PSDINTRO`:

```bash
python3 tools/generate_octagonal_arena.py --output /tmp/psdoom-arena.wad
python3 tools/validate_wad.py --wad /tmp/psdoom-arena.wad
python3 tools/validate_wad.py --wad /tmp/psdoom-arena.wad --iwad wads/DOOM1.WAD
```

The final command additionally proves that every map texture and flat exists in
the supplied IWAD. No external WAD tools or Python packages are required.

Or generate an Xcode project from the same build and Run the `psDoom` scheme:

```bash
cmake -G Xcode -S . -B build-xcode && open build-xcode/psDoom.xcodeproj
```

See [`docs/architecture.md`](docs/architecture.md) for the architecture, engine
integration, arena format, and build.

## Status

The engine, process-management layer, and generated octagonal sky arena are
integrated in `psDoom.app` on Apple Silicon. The distinct `PSDINTRO` level
provides a clear 77-cell lower process arena, full elevated ring, eight
freestanding pillar landings, restrained shareware pickups, sky horizon, and a
reachable enclosed 32×72 exit into the IWAD's unmodified `E1M1`.

## References

- Upstream psDooM: <http://psdoom.sourceforge.net/>
- psdoom-ng (Chocolate Doom lineage):
  <https://github.com/orsonteodoro/psdoom-ng1>
- Dennis Chao, _Doom as an Interface for Process Management_ (LISA 2000)
- Internet Archive CDX API: <https://web.archive.org/cdx/search/cdx>
