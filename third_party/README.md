# Vendored engine

This directory holds the third-party Doom engine that psDoom is built on, vendored (committed
in full) rather than referenced as a submodule. Vendoring keeps the repo self-contained — in
line with the project's longevity goal — and lets us patch the engine in place for the psDoom
process-management hooks.

## crispy-doom/

| | |
|---|---|
| **Project** | Crispy Doom (limit-removing, enhanced-resolution Doom source port based on Chocolate Doom) |
| **Upstream** | https://github.com/fabiangreffrath/crispy-doom |
| **Tag** | `crispy-doom-7.1` |
| **Commit** | `0a022e0ee6c74d9bab173ed9ee5212312e90ce3a` |
| **Vendored** | 2026-06-23 |
| **License** | GPL-2.0-or-later (see `crispy-doom/COPYING.md`) |

### What was changed when vendoring

- The upstream nested `.git/` was removed — this is a flat vendored snapshot, not a submodule.
- The upstream `quickcheck` submodule (https://github.com/chocolate-doom/quickcheck, a fuzz/QA
  helper) was **not** fetched. It is not needed to build or run the game; the `quickcheck/`
  directory is intentionally left empty.
- Otherwise the `crispy-doom/` subtree is byte-for-byte identical to the upstream tag, so a diff
  against upstream cleanly shows only our psDoom patches.

### psDoom local modifications

Hooks added so the psDoom module (in `src/psdoom/`, compiled into the `doom` library from the
top-level `CMakeLists.txt`) can drive process management. The module itself lives outside this
tree; only these thin call-sites and fields are in here:

| File | Change |
|---|---|
| `src/doom/p_mobj.h` | Added `int psd_pid;` + `char psd_name[16];` to `mobj_t` (0 = ordinary monster). |
| `src/doom/d_main.c` | `#include "psdoom.h"`; call `psdoom_init()` before the final `D_DoomLoop()`. `#include "psdoom_options.h"` and `M_BindIntVariable` the four `psdoom_*` settings (alongside the crispy binds) so they persist in the config file. |
| `src/doom/p_tick.c` | `#include "psdoom.h"`; call `psdoom_sync()` each tic in `P_Ticker` (after `P_RespawnSpecials`). |
| `src/doom/p_inter.c` | `#include "psdoom.h"`; `psdoom_kill(target)` in `P_KillMobj`; `psdoom_wound(target)` in `P_DamageMobj` (survival path). |
| `src/doom/r_defs.h` | Added `int psd_pid;` + `const char *psd_name;` to `vissprite_t` so the renderer can carry process identity onto a sprite. |
| `src/doom/r_things.c` | `#include "psdoom.h"`; copy `psd_pid`/`psd_name` from the `mobj_t` onto the `vissprite_t` in `R_ProjectSprite`. `R_DrawMaskedColumn` records (`psd_col_drawn`/`psd_col_top`) whether a sprite actually rendered a pixel and its topmost row; `R_DrawVisSprite` resets that per sprite and calls `psdoom_draw_label(...)` only when something drew, so labels are suppressed for monsters fully hidden behind walls and anchored to the visible sprite top. |
| `src/doom/m_menu.h` | Moved the `menuitem_t` / `menu_t` typedefs here from `m_menu.c`, and declared `M_SetupNextMenu` + `M_WriteText`, so the psDoom options page (`src/psdoom/psdoom_menu.c`) can build its own menu without living in `m_menu.c`. |
| `src/doom/m_menu.c` | `#include "psdoom_menu.h"`; one `"psDoom"` item in `OptionsMenu` calling `M_PsDoom` (the doorway into the isolated psDoom menu). The moved typedefs are compiled out via `#if 0`; `M_SetupNextMenu`/`M_WriteText` forward decls un-`static`ed (extern, like the existing `M_StringWidth`). No psDoom menu data/draw code lives here. |
| `src/m_config.c` | Four `CONFIG_VARIABLE_INT(psdoom_*)` entries so the options persist in the config file. |

The `psdoom_*` entry points are implemented in `src/psdoom/` (+ `src/platform/macos/` for the
native process backend): enumerate processes, spawn one monster per current-UID process on E1M1,
wound -> renice, kill -> SIGTERM, draw each process-monster's PID/name label, and present an
in-game psDoom options menu (`src/psdoom/psdoom_menu.c` + `psdoom_options.{h,c}`). All the menu's
data, drawing and toggles live in `src/psdoom/`; `m_menu.c` only gains the one-line Options
doorway. After an engine update, re-apply these call-site/field edits.

### Updating the engine

1. Re-clone upstream at the new tag into a scratch dir and capture its commit SHA.
2. Diff the current `crispy-doom/` against the new tag to understand upstream changes.
3. Replace the snapshot, then re-apply the psDoom patches listed above; update this file.

### Build

Built out-of-source via CMake (see the top-level project docs) so this vendored tree stays
pristine. Dependencies: SDL2, SDL2_mixer, SDL2_net, libsamplerate, libpng, FluidSynth.
