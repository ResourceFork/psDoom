# psDoom

A modern revival of the original **Mac OS X / X11 port of psDoom** by Resource Fork.

psDoom turns Unix process management into the game of Doom. Each running process appears as a
monster in the Doom world, labeled with its name and PID. Shooting or punching a monster acts
on the corresponding process:

- **Wound** a monster → `renice` (lower scheduling priority)
- **Kill** a monster → `kill` (send SIGTERM/SIGKILL)

> ⚠️ **No confirmation, no undo.** Killing a monster kills the real process immediately.

---

## Background

The original concept is by **Dennis Chao** (*"Doom as an Interface for Process Management,"*
LISA 2000). The SourceForge **psDooM** project (David Koppenhofer) built on XDoom / id Software's
GPL Doom source. This repo is the **Mac OS X / X11 port** originally published at resourcefork.com
and linked from the canonical psDooM download page.

---

## Revival Goal

Modernize the port onto a current Doom engine with a native macOS process layer:

- **Engine:** Chocolate Doom lineage (SDL2) — vanilla-accurate, builds on Apple Silicon
- **Process layer:** `libproc` / `kill(2)` / `setpriority(2)` — no shell-out to `ps`

See [`CONTEXT.md`](CONTEXT.md) for the full research notes, recovered Internet Archive artifacts,
prior-art review (psdoom-ng), and open decisions.

---

## Requirements (planned)

- macOS (Apple Silicon or Intel), any version supporting SDL2
- A retail Ultimate Doom or Doom II WAD (or the Doom shareware WAD, `doom1.wad`, for testing)

---

## Original Artifacts

The original source and compiled binary are archived at the Internet Archive. See
[`CONTEXT.md` § Recovered Artifacts](CONTEXT.md#recovered-artifacts) for raw download URLs and
CDX API queries.

```bash
# Download original source into docs/original/
curl -L -o docs/original/psdoom-src.zip \
  "https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/psdoom-src.zip"
```

---

## Reference Links

- Upstream psDooM: http://psdoom.sourceforge.net/
- psdoom-ng (Chocolate Doom lineage): https://github.com/orsonteodoro/psdoom-ng1
- Dennis Chao, *Doom as an Interface for Process Management* (LISA 2000)
