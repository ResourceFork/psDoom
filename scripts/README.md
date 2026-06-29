# scripts/ — reference backend scripts

This pair reproduces psDoom's **native** behavior through the
[script backend](../docs/script-backend.md), so it doubles as the canonical, copy-me example of
the protocol: enumerate the processes you own, size them by memory and CPU, and on a hit renice
(wound) or `SIGTERM` (kill).

| Script | Role | Env var |
|---|---|---|
| [`poll-processes.sh`](poll-processes.sh) | enumerate your processes (stdout) | `PSDOOM_POLL_CMD` |
| [`respond-processes.sh`](respond-processes.sh) | wound = renice, kill = SIGTERM | `PSDOOM_RESPOND_CMD` |

## Use

```sh
export PSDOOM_POLL_CMD="$PWD/scripts/poll-processes.sh"
export PSDOOM_RESPOND_CMD="$PWD/scripts/respond-processes.sh"
open build/psDoom.app          # or run the engine binary directly
```

Leave `PSDOOM_RESPOND_CMD` unset for a **read-only** run: monsters appear and can be shot, but no
process is ever signalled.

## How they map to the contract

`poll-processes.sh` emits one TAB-separated `key=value` line per process: `id`=pid,
`parent`=ppid, `weight`=resident bytes, `load`=%CPU, `flags` bit 0 = no controlling tty,
`label`=process name. `respond-processes.sh` dispatches on the `verb` ($1) and reads `amount=`
for the renice step. Full field/verb reference: [`../docs/script-backend.md`](../docs/script-backend.md).

## Safety

In script mode the native protected-process filter does **not** apply — the script decides what
is enumerable and killable. `poll-processes.sh` carries a small deny-list (loginwindow, Dock,
Finder, WindowServer, …) so a stray shot can't end your session; extend it for your machine. The
responder is exec'd without a shell, so a process name can never be interpreted as a command.
