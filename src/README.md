# src/

Source code for the psDoom revival lives here.

This directory will be populated once the engine and presentation decisions are made
(see [`CONTEXT.md` § Open Decisions](../CONTEXT.md#open-decisions)).

## Planned structure

```
src/
├── engine/        # Chosen Doom engine (e.g. Chocolate Doom submodule or copy)
└── ps_macos/      # Native macOS process management module
    ├── ps_macos.h
    └── ps_macos.c
```

## macOS process module overview

The native process layer will implement the three integration points from the psdoom-ng contract:

| Contract point | Native API |
|---|---|
| Enumerate processes (spawn monsters) | `proc_listpids` + `proc_pidinfo` from `<libproc.h>` |
| Kill process (monster death) | `kill(pid, SIGTERM)` → `kill(pid, SIGKILL)` |
| Renice process (wounding) | `setpriority(PRIO_PROCESS, pid, niceValue)` |

See [`CONTEXT.md` § macOS Process Module](../CONTEXT.md#macos-process-module--design-notes) for
full design notes and caveats (permissions, safety filtering, process count).
