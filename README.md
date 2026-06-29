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

- **Original concept:** Dennis Chao — LISA 2000 paper *"Doom as an Interface for
  Process Management."*
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

| Operation | Game event | Native macOS call |
|---|---|---|
| Enumerate | spawn monsters | `proc_listpids` / `proc_pidinfo` (`<libproc.h>`) or `sysctl` `KERN_PROC` |
| Renice | wound a monster | `setpriority(PRIO_PROCESS, pid, niceValue)` |
| Kill | monster death | `kill(pid, SIGTERM)`, escalate to `SIGKILL` |

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
third_party/crispy-doom/   vendored Crispy Doom engine (pristine; see third_party/README.md)
src/app/macos/             the .app launcher
src/psdoom/                process<->monster logic        (in progress)
src/platform/macos/        native libproc/renice/kill     (in progress)
CMakeLists.txt             single build: engine + psDoom.app
docs/                      architecture.md, handoff.md, original/ (recovered v1.0 archive)
wads/                      runtime IWAD (gitignored, not GPL)
```

## Build & run

Requires a Doom IWAD; a shareware `DOOM1.WAD` placed in `wads/` is bundled into
the app automatically. Dependencies via Homebrew:
`brew install sdl2 sdl2_mixer libsamplerate libpng fluid-synth`.

```bash
cmake -S . -B build && cmake --build build -j
open build/psDoom.app
```

Or generate an Xcode project from the same build and Run the `psDoom` scheme:

```bash
cmake -G Xcode -S . -B build-xcode && open build-xcode/psDoom.xcodeproj
```

See [`docs/architecture.md`](docs/architecture.md) for the architecture, engine
integration, and build.

## Status

Engine bootstrapped: `psDoom.app` builds and runs **stock** Crispy Doom on Apple
Silicon. The psDoom process-management layer (process→monster, wound→renice,
kill→kill, labels) is the next step — see the revival plan above.

## References

- Upstream psDooM: <http://psdoom.sourceforge.net/>
- psdoom-ng (Chocolate Doom lineage):
  <https://github.com/orsonteodoro/psdoom-ng1>
- Dennis Chao, *Doom as an Interface for Process Management* (LISA 2000)
- Internet Archive CDX API: <https://web.archive.org/cdx/search/cdx>
