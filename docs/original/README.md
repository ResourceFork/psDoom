# docs/original/

Original psDoom artifacts recovered from the Internet Archive and committed here as a **longevity
archive**, so the revival never depends on a third party staying online.

## What's committed

| Path | Description |
|---|---|
| [`psdoom-src.zip`](psdoom-src.zip) | Pristine original source archive, byte-for-byte as recovered (1,251,597 bytes; verified intact via `unzip -t`). Keep this untouched as the canonical copy. |
| [`psdoom-src/`](psdoom-src/) | The same archive extracted for easy browsing and reference. macOS cruft (`__MACOSX/`, `.DS_Store`) stripped; `.DS_Store` is gitignored. |

The extracted tree is the XDoom-based lineage the port was built on: `psdoomdoc/` (original
`README`, `CHANGELOG`, `COPYING`, `CREDITS`), `xdlaunch/` (GUI launcher), and `xdoomsrc/` (the
engine, with `musserv/`, `sndserv/`, `pwads/`, and per-platform `xdoom/` subdirs).

> The pristine zip and the extracted tree are intentionally redundant: the zip preserves exact
> bytes for integrity, the tree makes the code greppable and linkable without unzipping.

## Source recovery details

Recovered from the Wayback Machine (capture `20051223232037`):

```
https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/psdoom-src.zip
```

## Not yet captured

These survive in the Internet Archive but are not committed here yet:

| Artifact | Description | Source |
|---|---|---|
| `psDoom.sit` | Original compiled app — StuffIt archive (479,917 bytes) | [Wayback Machine](https://web.archive.org/web/20060214121914id_/http://resourcefork.com/WWWserver/software/psDoom/psDoom.sit) |
| `screenshot1.gif`–`screenshot5.gif` | Original site screenshots | `https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/img/screenshotN.gif` |

To capture them:

```bash
# Compiled app (StuffIt) — expand with: brew install unar && unar psDoom.sit
curl -L -o docs/original/psDoom.sit \
  "https://web.archive.org/web/20060214121914id_/http://resourcefork.com/WWWserver/software/psDoom/psDoom.sit"
```
