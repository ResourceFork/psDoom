# psDoom Revival — Context & Handoff

> **Purpose:** Self-contained capture of the conversation and research that led to recovering
> the original **Resource Fork "psDoom for OS X/X11"** port and planning its modern revival.
> An agent (or you) can resume from here with full context.
>
> **Captured:** 2026-06-22
> **Current location:** `docs/handoff.md` in the dedicated **psDoom** repo (`~/Source/psDoom`).
> **Status:** Decisions resolved and engine bootstrapped — revived on **Crispy Doom 7.1** as a
> native macOS `.app` that builds and runs (stock engine so far; the psDoom process layer is next).
> This file is the origin/history record; the architecture and features live in
> [`architecture.md`](architecture.md).

---

## TL;DR

- The user is the author of the early-2000s **Mac OS X / X11 port of psDoom**, published at
  **resourcefork.com**, and officially linked from the canonical psDooM site.
- The original **source and compiled binary are recoverable** from the Internet Archive
  (links below).
- Goal: **modernize** it — either bring it onto a modern Doom engine or start clean on a modern
  Doom codebase and reimplement the (small) process-management layer.
- The process-management ("ps") part is genuinely small: **list processes → spawn monsters**,
  **wound monster → renice**, **kill monster → kill process**.
- Recommended path: revive on the **Chocolate Doom lineage** (as `psdoom-ng` did) but replace the
  shell-`ps` plumbing with a small **native macOS module** (`libproc` / `kill(2)` / `setpriority(2)`).

---

## How to resume (read this first)

1. Skim [Recovered Artifacts](#recovered-artifacts) and grab the source zip into the new repo.
2. Read [Original Port Details](#original-port-details) and [Prior Art](#prior-art-psdoom-ng) to
   understand the behavior to reproduce.
3. Make the two calls in [Open Decisions](#open-decisions) (engine + native-vs-X11).
4. Follow [Next Steps](#next-steps).

---

## Background: what psDoom is

psDoom turns Unix process management into the game of Doom. Each running process is rendered as a
monster in the Doom world, labeled with its name/PID. Shooting/punching a monster acts on the
corresponding process: wounding lowers its scheduling priority (renice), killing it sends a kill
signal. It's part novelty, part genuinely useful "interactive process management" demo.

**Lineage & attribution:**

- Original idea: **Dennis Chao** — LISA 2000 paper *"Doom as an Interface for Process Management."*
- **psDooM** (the SourceForge project): maintained by **David Koppenhofer**; based on **XDoom**
  (Udo Munk), which is based on id Software's GPL Doom source.
- The user's work: the **OS X / X11 port** of that lineage, hosted at resourcefork.com.

> **Correction note:** Earlier in the conversation the assistant credited Koppenhofer as the
> *creator*. More precisely, the original concept is **Dennis Chao's**; Koppenhofer was the upstream
> maintainer. Recorded here so the error isn't propagated.

Related modern revivals of the same "system-as-Doom" meme: **Kube DOOM** (2022, kills Kubernetes
pods the same way).

---

## Recovered Artifacts (Internet Archive)

All under host `resourcefork.com`, path `/WWWserver/software/psDoom/` (later captures use the
`/archive/WWWserver/...` prefix). Use the `id_` raw-download form of the Wayback URL to get the
original bytes rather than the rewritten HTML.

| Artifact | Notes | Raw download URL |
|---|---|---|
| **Source code** | `psdoom-src.zip`, `application/zip`, **1,160,681 bytes**, capture `20051223232037` | `https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/psdoom-src.zip` |
| **Compiled app** | `psDoom.sit` (StuffIt), **479,917 bytes**, capture `20060214121914` | `https://web.archive.org/web/20060214121914id_/http://resourcefork.com/WWWserver/software/psDoom/psDoom.sit` |
| **Screenshots** | `screenshot1.gif` … `screenshot5.gif`, captures ~`20051223` | `https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/img/screenshot1.gif` (1–5) |
| **Info page** | Many captures 2004–2015 | `https://web.archive.org/web/20070307035053id_/http://resourcefork.com/WWWserver/software/psDoom/` |

**Enumerate everything via the CDX API:**

```bash
# All psDoom-related captures
curl -s "https://web.archive.org/cdx/search/cdx?url=resourcefork.com*&output=json&collapse=digest&filter=urlkey:.*psdoom.*&limit=200"

# All downloadable archives on the whole site
curl -s "https://web.archive.org/cdx/search/cdx?url=resourcefork.com*&output=json&collapse=urlkey&filter=original:.*\.(sit|hqx|gz|tgz|tar|dmg|zip|bin)$&limit=200"
```

> **Tip:** The `web_fetch`-style HTML extractor returned empty for these archive pages;
> `curl` works. The `.sit` is a StuffIt archive — `unar` (`brew install unar`) will expand it on
> modern macOS.

---

## Original Port Details

From the recovered info page (`psDoom for OS X/X11 - 1.0`):

- **What it does (the user's own words, abridged):** psDoom lets you kill most any running process
  by shooting/punching its representation in the Doom world; wounding a monster *decreases* the
  process's priority, killing it kills the process. Shipped **pre-compiled**.
- **No confirmation / no undo** — the page explicitly warns that killing a labeled monster kills
  the process with no "save your work?" prompt.
- **Requirements:** an Ultimate Doom or Doom II WAD (retail or shareware), any Mac running
  **Mac OS X + X11**.
- **Run:** under X11, `cd` into the psdoom folder and `./psdoom`.
- **Upstream link:** <http://psdoom.sourceforge.net/>

---

## Prior Art: psdoom-ng

`psdoom-ng` (orsonteodoro) already did the "reimplement psDooM on a modern Doom" move, on top of
**Chocolate Doom** (SDL). Repo (moved): `https://github.com/orsonteodoro/psdoom-ng1`.

Two takeaways:

1. **The OS contract is tiny.** psdoom-ng reduces the entire game↔OS integration to three external
   commands overridable via environment variables:
   - `PSDOOMPSCMD` — prints one process per line: `<user> <pid> <processname> <is_daemon=[1|0]>`
   - `PSDOOMRENICECMD` — receives a `pid` argument (wounding)
   - `PSDOOMKILLCMD` — receives a `pid` argument (death)
   This confirms the "ps functionality is probably very simple" hypothesis.
2. It has its **own Mac OS X support**, but that's a **separate, later lineage** (Chocolate Doom)
   credited to a different contributor — *not* derived from the user's X11 port.

---

## Modernization Options

| Option | Engine | Pros | Cons |
|---|---|---|---|
| **1. Faithful revival (recommended)** | Chocolate / Crispy Doom (SDL2) | Vanilla-accurate, cross-platform, builds on Apple Silicon via Homebrew, matches psdoom-ng lineage | Not visually "modern" |
| **2. Clean-room** | `doomgeneric` | Minimal, easy to bolt logic onto, clearest to reason about | More glue to write for input/render |
| **3. Modern visuals** | GZDoom (OpenGL/Vulkan, ZScript) | Most modern look, scriptable | Process control still needs a native helper; most complex |

**Recommendation:** Option 1 — clone the psdoom-ng / Chocolate Doom lineage for a faithful,
low-friction revival, then replace the shell-`ps` plumbing with a small native macOS process module.
That yields a real, buildable psDoom on Apple Silicon fastest, and the native module is the
genuinely new work.

---

## macOS Process Module — Design Notes

The "ps" layer to (re)implement, mapping the three psdoom-ng commands to native calls:

- **Enumerate** (→ spawn monsters): `proc_listpids(PROC_ALL_PIDS, ...)` + `proc_pidinfo(...)` from
  `<libproc.h>`, or `sysctl` with `KERN_PROC`. (Or just shell out to `ps` for a first cut.)
- **Kill** (monster death): `kill(pid, SIGTERM)`, escalate to `SIGKILL`.
- **Renice** (wounding): `setpriority(PRIO_PROCESS, pid, niceValue)`.

**Caveats to design around:**

- **Permissions (big change since 2004):** modern macOS only lets you signal/renice processes you
  **own** unless running as root. Default the game to the current UID's processes; system-wide
  requires `sudo`.
- **Safety:** it really kills processes with no confirmation. Add a guard rail — an allow/deny
  filter and/or an "only my UID" default — so a stray BFG blast doesn't nuke something important.
- **Process count:** a modern Mac has hundreds of processes; consider filtering (hide daemons by
  default, the `is_daemon` flag from the psdoom-ng contract is useful here).

---

## Open Decisions

Needed before writing code:

1. **Direction:**
   - Engine: faithful (Chocolate/Crispy Doom) **[recommended]** vs. clean-room (doomgeneric) vs.
     modern (GZDoom)?
   - Presentation: **native macOS** (Cocoa/SDL2 window) vs. keep the **X11** flavor for nostalgia?

> **Resolved:** repo location — the project now lives in its own repo at `~/Source/psDoom`.

---

## Next Steps

1. ~~User creates the new psDoom repo and relocates this file into it.~~ **Done** — repo lives at
   `~/Source/psDoom`; this file is at `docs/handoff.md`.
2. ~~Download the archived `psdoom-src.zip` into the repo for reference.~~ **Done** — committed at
   [`docs/original/psdoom-src.zip`](original/psdoom-src.zip) as a longevity archive (verified
   intact via `unzip -t`; full `psdoom-src/` tree present).
3. Optionally expand `psDoom.sit` (`brew install unar && unar psDoom.sit`) to compare the shipped app.
4. Pick the engine + presentation (Open Decisions).
5. Stand up the chosen engine building on Apple Silicon, then implement the native macOS process
   module and wire it to the three integration points (enumerate / renice / kill).

---

## Conversation Transcript

> Verbatim user messages; assistant turns condensed but faithful. Tooling/output omitted.

**User:** are you familiar with an app/program called `psDoom`? It's very old.

**Assistant:** Yes — a classic Unix tool. A Doom mod where running processes appear as monsters
labeled with name/PID; shooting a monster sends a kill signal to that process (wounding lowers its
priority). Built on id's GPL Doom source; more novelty/proof-of-concept than a daily sysadmin tool.
Mentioned the later psDooM follow-up and the modern Kube DOOM revival. Asked whether this was for a
project and noted the open `tunnel-app` workspace.
*(This turn credited Koppenhofer as creator — later corrected to Dennis Chao.)*

**User:** are you aware that i'm the creator of the Mac port of psDoom?

**Assistant:** Was not aware, and can't verify anyone's identity — and didn't have specific
knowledge of a Mac port. Offered to search the web for what's publicly documented.

**User:** yes, do a search. see if the mac port comes up. This is "Mac OS X", not "macOS" like it
is in its modern terms. I authored this over 20 years ago.

**Assistant:** Searched. The canonical psDooM site (psdoom.sourceforge.net) Download page states
there is a **Mac OS X (X11) port at Resource Fork** — corroborating the claim, and the early-2000s
timing fits. Couldn't extract an author name from the CREDITS at that point. Offered to dig further.

**User:** i own the resourcefork.com domain, website, projects. I am the creator.

**Assistant:** Took the user at their word — everything lines up. Offered directions: revive the
original, rework the concept, or work on the open `tunnel-app`. Asked which way to go.

**User:** see if you can find the old work. i want to modernize it, but it needs to incorporate a
more modern version of doom, or to completely start over on a modern doom codebase, reimplementing
the –probably very simple– ps functionality part.

**Assistant:** Created a 4-step plan. Used the Internet Archive CDX API to locate the original
**`psdoom-src.zip`**, the compiled **`psDoom.sit`**, five screenshots, and the info page. Recovered
the page text confirming **v1.0, OS X/X11, wound=renice, kill=kill**, and requirements. Reviewed
**psdoom-ng** prior art (the three-command env-var contract). Presented findings, modernization
options + recommendation, a native macOS process-module design, and safety/permission caveats.
Asked for two decisions (repo location + engine/presentation direction).

**User:** let's take this WHOLE conversation and plop it into a single markdown file, in this repo,
that i can relocate once i create a NEW psDoom repo in a different location. Then you can resume
from all this context and proceed.

**Assistant:** Created this file.

---

## Reference Links

- Upstream psDooM: <http://psdoom.sourceforge.net/>
- psDooM Download page (mentions the Mac OS X / X11 port): <https://psdoom.sourceforge.net/download.html>
- psdoom-ng (Chocolate Doom lineage): <https://github.com/orsonteodoro/psdoom-ng1>
- Dennis Chao, *Doom as an Interface for Process Management* (LISA 2000)
- Internet Archive CDX API: <https://web.archive.org/cdx/search/cdx>

> *Content from external sources was summarized/paraphrased for licensing compliance.*
