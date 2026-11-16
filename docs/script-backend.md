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
> psDoom's native behavior as a script (your processes, sized by RAM/CPU; it answers `post` with
> a renice and `delete` with SIGTERM).
> [`../examples/`](../examples/) wires psDoom to other things (a dependency-free synthetic demo,
> and Docker containers). Each is a small `sh` script implementing the contract below.

## Enabling the backend

Configuration is entirely via environment variables, read once at startup:

| Variable | Required | Default | Purpose |
| --- | --- | --- | --- |
| `PSDOOM_POLL_CMD` | no | the bundled Summoner | Command whose **stdout** enumerates entities, overriding the app's bundled Summoner (`python3 Contents/Resources/summoner/summon get`). Run via the shell, so pipelines are allowed (`ps -eo ... \| awk ...`). |
| `PSDOOM_RESPOND_CMD` | no | — (bundled Summoner when `PSDOOM_POLL_CMD` is unset too) | Command invoked to **act** on an entity (`post` / `delete`). With `PSDOOM_POLL_CMD` set and this unset, psDoom is a read-only visualizer: monsters appear and can be shot, but nothing is acted on. |
| `PSDOOM_POLL_TIMEOUT_MS` | no | `1000` | Max time a poll may run. If it overruns, its process group is killed and only the complete lines it wrote are used. The poll runs off the game thread, so the game never waits for it, timeout or not. |

If `PSDOOM_POLL_CMD` is unset, psDoom runs the Summoner bundled in the app
(`Contents/Resources/summoner`) with a `python3` it finds without a shell
(Homebrew's, then the Command Line Tools' or Xcode's real one; never the
`/usr/bin/python3` install shim). psDoom has **no native process backend**: if
Summoner is missing, no `python3` is found, or a poll does not answer, there
are no process monsters, psDoom says why once (log and HUD), and nothing is
acted on. The variables are for development: pointing psDoom at another
script, or at a `summon` with `--root` or `--plugin`.

## Poll output — what the game reads

`PSDOOM_POLL_CMD` writes UTF-8 text to stdout, **one entity per line**.

- Blank lines, and lines whose first non-space character is `#`, are ignored (comments).
- An entity line is a set of **TAB-separated `key=value` fields**. Field order does not
  matter. Unknown keys are ignored. A repeated key takes its last value. A value may
  contain spaces, but never a TAB or newline (those delimit fields and records).
- The game keeps at most 4096 entities per poll. It **keeps the script's order and filters
  nothing** ([Trust](#trust)): the first 64 become candidates, and the first ones up to the
  in-game live-monster cap become monsters. Put the most important entities first.
- A leading block of `#` lines may declare what the script can sort by
  ([Ordering](#ordering-the-hosts-request)).
- **When it runs.** psDoom polls about once a second, one poll at a time, on a thread of its
  own: a frame never waits for the poll. Until a poll finishes, the monsters stand as the
  last poll left them; its result is applied on the next tic after it ends, and the next poll
  starts about a second later. A result left waiting longer than the timeout plus a second
  (the game was paused) is dropped and polled again, and so is one from before a new level.
  The poll and responder start with no signal blocked. A poll still running when psDoom exits
  normally is stopped like one that timed out (not on a crash).

### Fields

| key | type | required | default | meaning |
| --- | --- | --- | --- | --- |
| `id` | integer > 1 | **yes** | — | Stable, unique handle for the entity; becomes the monster's identity. **Must stay the same across polls** for the same entity, so it can be tracked, re-graded, and not respawned after a kill. A script must not reuse an `id` for a different entity while that `id` is listed, because tracking which entity an `id` names is the script's job, not psDoom's. `0` and `1` are reserved (ignored if emitted). An `id` that is not plain decimal digits in the range 2 to 2147483647 (a sign, a space, trailing characters or overflow) is rejected, and that line yields no entity. |
| `parent` | integer ≥ 0 | no | `0` | `id` of this entity's parent. Used to group fork-bomb swarms (a parent whose child count spikes). `0` = no parent. |
| `label` | string | no | `""` | Text drawn above the monster. Rendered in the HUD font (uppercase ASCII; other glyphs show as spaces). Truncated to 31 bytes. |
| `weight` | unsigned integer | no | `0` | "Size" metric. Drives monster toughness when *Classify by* is **memory/weight** (the default). Compared against the [size ladder](#size-ladder). |
| `load` | integer (percent; `100` = one core) | no | `0` | "Load" metric. Drives monster toughness when *Classify by* is **CPU/load**. Compared against the [load ladder](#load-ladder). |
| `flags` | integer bitmask | no | `0` | Bit `0` (`1`) = background / low-priority: ranked below interactive entities when truncating to the cap. Other bits are reserved (emit `0` if unsure). |
| `group` | string | no | `""` | The entity's group (an app, a team, a parent). Drawn on its own line above the label when it differs from it. Truncated to 31 bytes. |
| `grade` | decimal number | no | — | Where the entity sits on its source's own full range: `0` = least, `1` = most. When present it **picks the monster**, overriding *Classify by* ([below](#grade)). Values below 0 or above 1 are clamped. A value that is not a finite decimal number (empty, `abc`, `0.5x`, a hex float such as `0x1p-1`, `nan`, `inf`, an overflow such as `1e999`, a leading space) is ignored, and the last valid `grade` on a line wins. |

`weight` and `load` are deliberately the same two metrics the native backend reports
(memory footprint in bytes, CPU percent), so the existing *Classify by* option and the
size/load ladders apply unchanged. Scale your metric onto the ladder you want, or send a
`grade` instead and let the game do the scaling.

### Grade

`grade` lets a source say where an entity sits on *its own* range without knowing anything
about the game's monsters. psDoom takes the tiers of its ladder that the loaded WAD can
actually draw (N of them; Zombieman is always one) and uses tier `min(floor(grade × N), N − 1)`,
counting from the weakest. So `1.0` is always the top drawable monster: the Cyberdemon with a
WAD that has its sprite, the Baron with the shareware WAD, whose five drawable tiers put
`0.8`–`1.0` on the Baron. An entity without `grade` is classified from `weight` or `load` by the
*Classify by* option, as before; both still feed the ranking that picks which entities are shown.

### Example

```
# three entities under one supervisor
id=4123	label=web-frontend	weight=734003200	load=140	parent=4000	grade=0.9
id=4124	label=postgres	grade=0.25	parent=4000
id=4000	label=supervisor	flags=1
```

The first two pick their monster by grade; the supervisor has none, so *Classify by* decides.

### Size ladder

`weight ≥` threshold selects the monster (when classifying by weight):

| weight ≥ | monster |
| --- | --- |
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
| --- | --- |
| `$1` `verb` | The action, lowercase: `post` when the monster was wounded (lower the entity's priority, move it down, renice it), `delete` when it was killed (remove or stop it). More verbs may be added later — **treat an unknown verb as a no-op and exit 0.** |
| `$2` `id` | The entity `id` from the poll output. Always present. |
| `$3…` `key=value` | Extensible, order-independent extras. Currently: `amount=<int>` (injury magnitude; present for `post`, absent for `delete`), `label=<string>` (the entity's label, when known) and `mode=<dryrun\|post-only\|delete>` (always present). Ignore keys you don't recognize. |

**`mode`** is the game's effect setting, sent with every event: `dryrun` (psDoom **Off**:
act on nothing), `post-only` (**Wounds only**: `post` may act, `delete` must not) or `delete`
(**Wounds and kills**: both may act). These protocol words never appear in the game's menus. psDoom always reports the event and leaves the decision to the responder, so
**a responder must honour `mode`**; treat a missing or unknown mode as `dryrun`. The
reference responders in `scripts/` and `examples/` do.

- **Exit code is not read.** psDoom runs the responder *detached* (double-forked) so it never
  blocks the game tick or leaves zombies — which means it does **not** capture the script's exit
  status. Whether an action "worked" is observed only indirectly: if the entity is gone at the
  next poll its monster retires; if it is still present the monster simply reappears, exactly
  like a process that ignores `SIGTERM`. Exiting `0` on success is still good hygiene (it matters
  when you run the script yourself), but psDoom does not act on it.
- `PSDOOM_RESPOND_CMD` is split on whitespace into a command plus fixed leading arguments, so
  `PSDOOM_RESPOND_CMD="summon --plugin csv"` works; psDoom appends `verb`, `id`, and the
  extras as further arguments. If your command's path contains spaces, wrap it in a script.
  A command named without a path is looked up along `PATH` when psDoom sends the event,
  before it forks; the child only execs the path found (a name found nowhere sends nothing).

### Example responder (`sh`)

```sh
#!/bin/sh
# $1=verb  $2=id  then key=value extras
verb=$1; id=$2; shift 2
amount=1; mode=dryrun
for kv in "$@"; do
    case $kv in
        amount=*)       amount=${kv#amount=} ;;
        label=*)        label=${kv#label=}   ;;
        mode=post-only) mode=post-only ;;
        mode=delete)    mode=delete ;;
    esac
done

# Honour the game's mode: nothing in dryrun, never delete in post-only.
case "$mode:$verb" in
    dryrun:*|post-only:delete) exit 0 ;;
esac

case $verb in
    post)   renice -n "$amount" -p "$id" >/dev/null 2>&1 ;;
    delete) kill "$id" >/dev/null 2>&1 ;;
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

## Ordering: the host's request

A script that can sort or group declares its **dimensions** in a leading block of `#` lines,
before the first entity, on every poll:

```
# dim id=mem kind=numeric dir=desc roles=sort unit=bytes label=Memory
# dim id=cpu kind=numeric dir=desc roles=sort unit=percent label=CPU
# dim id=app kind=category dir=desc roles=group unit= label=App
# sort id=mem dir=desc
```

- `# dim`: `id` (`[a-z][a-z0-9_]{0,15}`), `kind` (`numeric` sorts, `category` groups),
  `dir` (a numeric dimension's default direction), `roles` (`sort`, `group`), `unit`, and
  `label` (the rest of the line; shown in the game's menu).
- `# sort id=… dir=…`: the sort the output is actually in.
- `# effect verb=post|delete text=…`: optional, what a wound (`post`) and a kill (`delete`)
  do, in plain words (`text` is the rest of the line, at most 40 bytes; psDoom draws it as
  printable ASCII). psDoom shows them on the effect setting's help line, for example
  "Wound: lowers its priority" and "Kill: quits the process". A script that sends none gets
  a plain "acts on it".

Once a poll has declared dimensions, psDoom appends the request to `PSDOOM_POLL_CMD` on each
following poll: `limit=64` (its candidate cap) and, after the player picks one under
**Sort by** in the psDoom options menu, `sort=<id>`. A changed choice reaches the next poll,
about a second later. Only well-formed ids are appended, and a script that never declares a
dimension never gets any words. The words go on the **end** of the command line, so a
dimension-declaring `PSDOOM_POLL_CMD` must end with the command that reads them: no pipeline
stage after it and no trailing `# comment`. The script answers in that order, trimmed to `limit`, and
grades relative to the requested dimension.

With [Summoner](summoner.md) the words are `summon get sort=ID[:asc|desc][,ID…] group=ID
limit=N`; Summoner checks them against the plugin's dimensions and drops unknown ones with a
`# summon:` note, falling back to the plugin's default sort.

## Trust

**The script is trusted with selection.** On the script path psDoom does not filter or rank
entities: no protected-name list, no self or launcher-chain guard, no ranking by its own score.
The script, or Summoner's plugin, decides what is listed and in what order. Leaving out
session-critical processes, the game and whatever launched it is therefore **the script's job**.
Summoner's `ps` plugin does it: psDoom's protected names, session plumbing (login, sudo,
tmux, iTermServer, terminal apps), processes not fully yours (effective and real uid), its
own process chain, and the host chain Summoner passes it; and every `summon get` drops items
in its own calling chain before printing. The reference `scripts/poll-processes.sh` does the
same in `sh`: it skips its own ancestor chain (its shell, psDoom, psDoom's launchers).

**The game only reports.** It tells the responder which entity got `post` and which got
`delete`, and its mode. The responder (Summoner) decides what runs.

There is no other path: psDoom itself never lists, renices or signals a process.

## Security model

- The poll command is run through the shell **by you**, the user who set the env var; its
  output is data, never executed.
- The response command is **exec'd directly with an argument vector** — psDoom never builds a
  shell string from entity data, so a hostile `label` (e.g. `; rm -rf ~`) is just an inert
  argument string, not a command.
- Scripts run with **your** privileges and do whatever you write them to do. The native
  backend's safety net (the protected-process deny-list, the launcher-chain guard) does
  **not** apply in script mode ([Trust](#trust)): deciding what is safe to list and act on is
  the script's responsibility, and so is honouring `mode`. Treat `PSDOOM_RESPOND_CMD` as you
  would any tool that can terminate things by id.
- Request words appended to the poll command are limited to `sort=<id>` with a well-formed
  id and `limit=<n>`, so they cannot carry shell syntax.

## Forward-compatibility

The contract is designed so old scripts keep working as the game grows, and new scripts keep
working on older game builds:

- **New poll fields** are added as new keys. Older games ignore unknown keys; scripts that
  omit a new key get its documented default. Never reorder or repurpose an existing key.
- **New response data** is added as new `key=value` extras or new verbs. Scripts must ignore
  unrecognized keys and verbs (and exit 0), so a newer game calling an older script is safe.
- **Shapes are stable.** Poll records are always TAB-separated `key=value`; response calls are
  always `verb id [key=value…]`. The required pieces (`id`; `verb` + `id`) never move.
- **There is no version header.** A line starting with `#` is a comment wherever it appears;
  the contract grows only by the rules above, so there is nothing to negotiate.
