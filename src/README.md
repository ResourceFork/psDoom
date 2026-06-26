# src/

Our code for the psDoom revival. The vendored Doom engine it builds on lives separately in
[`../third_party/`](../third_party/) — `src/` is psDoom's own code only.

## Layout

| Path | Purpose |
|---|---|
| `app/macos/` | The macOS `.app` launcher (`launcher_main.c`, `Info.plist.in`). Locates an IWAD and `exec()`s the engine binary inside the bundle. |
| `psdoom/` | Process↔monster game logic — registry, spawn/wound/kill policy, label text. *(to be written)* |
| `platform/macos/` | Native macOS process backend — `libproc` enumeration, `setpriority` renice, `kill`. *(to be written)* |

See [`../docs/revival-plan.md`](../docs/revival-plan.md) for the engine hook points, module API,
and build; and [`../docs/handoff.md`](../docs/handoff.md) for project background and history.
