# psDoom script backend — protocol

psDoom's process layer is a four-operation contract behind a backend interface
(`src/platform/proc_backend.h`): **enumerate**, **current-uid**, **renice/injure**,
**kill**. The *script backend* implements that interface by shelling out to user-supplied
commands, so you can point psDoom at anything you can enumerate and act on from a script —
containers, queues, build jobs, pull requests — not just OS processes.

This document is the stable contract between psDoom and those scripts. The game trusts it
and parses defensively; scripts can rely on it not changing underneath them. See the
[forward-compatibility rules](#forward-compatibility) for how it evolves.

> **Runnable scripts.** [`../scripts/`](../scripts/) holds the reference pair that reproduces
> psDoom's native behavior (your processes, sized by RAM/CPU; wound = renice, kill = SIGTERM).
> [`../examples/`](../examples/) wires psDoom to other things (a dependency-free synthetic demo,
> and Docker containers). Each is a small `sh` script implementing the contract below.

## Enabling the backend

Configuration is entirely via environment variables, read once at startup:

| Variable | Required | Default | Purpose |
|---|---|---|---|
| `PSDOOM_POLL_CMD` | yes | — | Command whose **stdout** enumerates entities. Setting it switches psDoom to the script backend. Run via the shell, so pipelines are allowed (`ps -eo ... \| awk ...`). |
| `PSDOOM_RESPOND_CMD` | no | — | Command invoked to **act** on an entity (injure / kill). If unset, psDoom is a read-only visualizer: monsters appear and can be shot, but nothing is signalled. |
| `PSDOOM_POLL_TIMEOUT_MS` | no | `1000` | Max time to wait for `PSDOOM_POLL_CMD`. If it overruns, the child is killed and that poll yields no update (the game never blocks). |

If `PSDOOM_POLL_CMD` is unset, psDoom uses its native OS backend as before.

## Poll output — what the game reads

`PSDOOM_POLL_CMD` writes UTF-8 text to stdout, **one entity per line**.

- Blank lines, and lines whose first non-space character is `#`, are ignored (comments).
- An entity line is a set of **TAB-separated `key=value` fields**. Field order does not
  matter. Unknown keys are ignored. A repeated key takes its last value. A value may
  contain spaces, but never a TAB or newline (those delimit fields and records).
- The game keeps at most 4096 entities per poll and shows the most relevant up to the
  in-game live-monster cap.

### Fields

| key | type | required | default | meaning |
|---|---|---|---|---|
| `id` | integer > 1 | **yes** | — | Stable, unique handle for the entity; becomes the monster's identity. **Must stay the same across polls** for the same entity, so it can be tracked, re-graded, and not respawned after a kill. `0` and `1` are reserved (ignored if emitted). |
| `parent` | integer ≥ 0 | no | `0` | `id` of this entity's parent. Used to group fork-bomb swarms (a parent whose child count spikes). `0` = no parent. |
| `label` | string | no | `""` | Text drawn above the monster. Rendered in the HUD font (uppercase ASCII; other glyphs show as spaces). Truncated to 31 bytes. |
| `weight` | unsigned integer | no | `0` | "Size" metric. Drives monster toughness when *Classify by* is **memory/weight** (the default). Compared against the [size ladder](#size-ladder). |
| `load` | integer (percent; `100` = one core) | no | `0` | "Load" metric. Drives monster toughness when *Classify by* is **CPU/load**. Compared against the [load ladder](#load-ladder). |
| `flags` | integer bitmask | no | `0` | Bit `0` (`1`) = background / low-priority: ranked below interactive entities when truncating to the cap. Other bits are reserved (emit `0` if unsure). |

`weight` and `load` are deliberately the same two metrics the native backend reports
(memory footprint in bytes, CPU percent), so the existing *Classify by* option and the
size/load ladders apply unchanged. Scale your metric onto the ladder you want.

### Example

```
#!psdoom v1
# three entities under one supervisor
id=4123	label=web-frontend	weight=734003200	load=140	parent=4000
id=4124	label=postgres	weight=2147483648	load=30	parent=4000
id=4000	label=supervisor	flags=1
```

### Size ladder

`weight ≥` threshold selects the monster (when classifying by weight):

| weight ≥ | monster |
|---|---|
| `0` | Zombieman |
| `16777216` (16 MB) | Shotgun Guy |
| `67108864` (64 MB) | Imp |
| `201326592` (192 MB) | Pinky |
| `536870912` (512 MB) | Hell Knight † |
| `1610612736` (1.5 GB) | Baron of Hell † |
| `4294967296` (4 GB) | Cyberdemon † |

### Load ladder

`load ≥` `5` / `25` / `60` / `120` / `250` / `500` percent selects
Shotgun Guy / Imp / Pinky / Hell Knight † / Baron † / Cyberdemon †; below `5` is a Zombieman.

† Needs a registered Doom/Doom II IWAD. With the shareware WAD (Episode 1 sprites only) the
ladder degrades to its highest drawable tier — Baron — so it never crashes the renderer.

## Response invocation — what the game runs

When the player injures or kills a monster and `PSDOOM_RESPOND_CMD` is set, psDoom runs it
**directly via `exec` (never through a shell)**, so an entity's `id` or `label` can never be
interpreted as shell syntax:

```
$PSDOOM_RESPOND_CMD <verb> <id> [key=value ...]
```

| argument | meaning |
|---|---|
| `$1` `verb` | The action: `wound` or `kill` (lowercase). More verbs may be added later — **treat an unknown verb as a no-op and exit 0.** |
| `$2` `id` | The entity `id` from the poll output. Always present. |
| `$3…` `key=value` | Extensible, order-independent extras. Currently: `amount=<int>` (injury magnitude; present for `wound`, absent for `kill`) and `label=<string>` (the entity's label, when known). Ignore keys you don't recognize. |

- **Exit code is not read.** psDoom runs the responder *detached* (double-forked) so it never
  blocks the game tick or leaves zombies — which means it does **not** capture the script's exit
  status. Whether an action "worked" is observed only indirectly: if the entity is gone at the
  next poll its monster retires; if it is still present the monster simply reappears, exactly
  like a process that ignores `SIGTERM`. Exiting `0` on success is still good hygiene (it matters
  when you run the script yourself), but psDoom does not act on it.
- `PSDOOM_RESPOND_CMD` is split on whitespace into a command plus fixed leading arguments, so
  `PSDOOM_RESPOND_CMD="docker kill"` works; psDoom appends `verb`, `id`, and the extras as
  further arguments. If your command's path contains spaces, wrap it in a script.

### Example responder (`sh`)

```sh
#!/bin/sh
# $1=verb  $2=id  then key=value extras
verb=$1; id=$2; shift 2
amount=1
for kv in "$@"; do
    case $kv in
        amount=*) amount=${kv#amount=} ;;
        label=*)  label=${kv#label=}   ;;
    esac
done

case $verb in
    wound) renice -n "$amount" -p "$id" >/dev/null 2>&1 ;;
    kill)  kill "$id" >/dev/null 2>&1 ;;
    *)     : ;;   # unknown verb: no-op, succeed
esac
exit 0
```

### Example poller (`sh`) — the native behavior, reimplemented

```sh
#!/bin/sh
# Your own processes as monsters, sized by RSS, loaded by %CPU.
ps -axo pid=,ppid=,rss=,%cpu=,comm= | awk '
{
    rss_bytes = $3 * 1024;          # ps reports RSS in KiB
    load_pct  = int($4);            # %CPU -> load
    name = $5; for (i = 6; i <= NF; i++) name = name " " $i;
    if ($1 > 1)
        printf "id=%s\tparent=%s\tweight=%d\tload=%d\tlabel=%s\n",
               $1, $2, rss_bytes, load_pct, name;
}'
```

## Security model

- The poll command is run through the shell **by you**, the user who set the env var; its
  output is data, never executed.
- The response command is **exec'd directly with an argument vector** — psDoom never builds a
  shell string from entity data, so a hostile `label` (e.g. `; rm -rf ~`) is just an inert
  argument string, not a command.
- Scripts run with **your** privileges and do whatever you write them to do. The native
  backend's safety net (the protected-process deny-list, the launcher-chain guard) is
  process-specific and does **not** apply to arbitrary entities — in script mode, deciding
  what is safe to enumerate and act on is the script's responsibility. Treat
  `PSDOOM_RESPOND_CMD` as you would any tool that can terminate things by id.

## Forward-compatibility

The contract is designed so old scripts keep working as the game grows, and new scripts keep
working on older game builds:

- **New poll fields** are added as new keys. Older games ignore unknown keys; scripts that
  omit a new key get its documented default. Never reorder or repurpose an existing key.
- **New response data** is added as new `key=value` extras or new verbs. Scripts must ignore
  unrecognized keys and verbs (and exit 0), so a newer game calling an older script is safe.
- **Shapes are stable.** Poll records are always TAB-separated `key=value`; response calls are
  always `verb id [key=value…]`. The required pieces (`id`; `verb` + `id`) never move.
- An optional first poll line `#!psdoom v1` may declare the protocol version. It is a comment
  to current parsers; absence means v1. A future incompatible revision would bump this and the
  game would refuse a version it doesn't understand rather than misread it.
