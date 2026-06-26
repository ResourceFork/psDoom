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

None yet. As we add the process→monster hooks, record each change here (file + purpose) so the
patch set against upstream stays auditable and re-appliable after an engine update.

### Updating the engine

1. Re-clone upstream at the new tag into a scratch dir and capture its commit SHA.
2. Diff the current `crispy-doom/` against the new tag to understand upstream changes.
3. Replace the snapshot, then re-apply the psDoom patches listed above; update this file.

### Build

Built out-of-source via CMake (see the top-level project docs) so this vendored tree stays
pristine. Dependencies: SDL2, SDL2_mixer, SDL2_net, libsamplerate, libpng, FluidSynth.
