# docs/original/

This directory holds the original archived artifacts recovered from the Internet Archive.

## Contents (download manually)

| File | Description | Source |
|---|---|---|
| `psdoom-src.zip` | Original source code (1,160,681 bytes) | [Wayback Machine](https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/psdoom-src.zip) |
| `psDoom.sit` | Original compiled app — StuffIt archive (479,917 bytes) | [Wayback Machine](https://web.archive.org/web/20060214121914id_/http://resourcefork.com/WWWserver/software/psDoom/psDoom.sit) |

## Download commands

```bash
# Source zip
curl -L -o docs/original/psdoom-src.zip \
  "https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/psdoom-src.zip"

# Compiled app (StuffIt)
curl -L -o docs/original/psDoom.sit \
  "https://web.archive.org/web/20060214121914id_/http://resourcefork.com/WWWserver/software/psDoom/psDoom.sit"

# Expand the StuffIt archive (requires unar: brew install unar)
unar docs/original/psDoom.sit -o docs/original/
```

## Screenshots

Five screenshots from the original site are available at:

```
https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/img/screenshot1.gif
https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/img/screenshot2.gif
https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/img/screenshot3.gif
https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/img/screenshot4.gif
https://web.archive.org/web/20051223232037id_/http://resourcefork.com/WWWserver/software/psDoom/img/screenshot5.gif
```

> The large binary files (`*.zip`, `*.sit`) are excluded from git via `.gitignore`.
> Download them locally as needed.
