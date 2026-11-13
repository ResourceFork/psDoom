# ps-harness — the plugin harness, design

The [script backend](script-backend.md) lets psDoom show and act on anything a script can
list. Each backend script pair re-solves the same problems: stable integer ids, scaling a
metric onto the monster ladders, trimming labels, staying under the 1-second poll timeout,
and not deleting the wrong thing. The **plugin harness** solves them once. A *plugin* then
only has to answer three questions about its domain: *what is there* (GET), *push this one
down* (POST), *get rid of this one* (DELETE).

```
 psDoom / psQuake                    ps-harness                               plugin
 ───────────────                     ──────────                               ──────
 PSDOOM_POLL_CMD  ── sh -c ──▶  get ── reads cache.json ──────────────┐
   "ps-harness get"                  └─ stale? spawn detached refresh ─┼─▶  describe, list
   (1 s timeout)                                                      │
 PSDOOM_RESPOND_CMD ─ exec ──▶  post|delete <id> ─▶ id → key ─────────┼─▶  nudge <key> amount=N
   "ps-harness"                      dry run? opt-in? rate? audit     │    remove <key>
                                                                      │
 you, at a terminal  ────────▶  undo <action-id> ────────────────────┴─▶  restore  (stdin)
```

The harness speaks the apps' [script contract](script-backend.md): **v2** by default (a
`#!psdoom v2` header, a 0..1 `grade` per item, and the game sends `post` / `delete`), or **v1**
with `--contract v1` for an older game build (no grade; the game sends `wound` / `kill`). To
the game, the harness is just a poll command and a responder. Further contract ideas are in
the [appendix](#appendix-contract-proposals), as proposals only.

## Where it lives and what it needs

- `harness/ps-harness` — the command: a thin launcher for the harness (put it on PATH or
  symlink it; nothing is installed for you).
- `harness/psdoom_harness.py` — the harness, one file.
- `harness/plugins/processes.py` — your own processes, the native behaviour.
- `harness/plugins/csv_rows.py` — rows of a CSV file.
- `harness/tests/` — unit and subprocess tests of the harness and both plugins. Run them
  from a plain checkout with `python3 -I harness/tests/run_tests.py` (no CMake; scratch goes
  to `harness/tests/.scratch/`, ignored by `harness/.gitignore`; a symlinked root is
  refused). psDoom's `ctest` also runs them as `psdoom_harness_tests`, with a scratch root
  in its build tree.
- `tests/harness_e2e_test.c` — psDoom's app-side conformance test ([below](#one-contract-for-every-ps-app)).

Python 3.10+ standard library only. The build already requires Python 3.10 (`CMakeLists.txt`
uses it for the WAD tools), so the harness adds no dependency. A top-level `harness/` keeps it
apart from `scripts/` (the copy-me `sh` reference pair) and `tools/` (build-time tools): it is
a runtime component with its own tests.

**Self-contained.** Nothing in `harness/` imports or reads from psDoom's source or build, and
its tests need only Python, and it carries its own `.gitignore` and a summary of the v1
contract in its README, so the folder can later move to its own repository unchanged.
psDoom keeps only two hooks into it: the `ctest` registration and the conformance test.

## One contract for every ps-app

The harness's poll output and its responder calls *are* the
[script-backend contract](script-backend.md) (v2, or v1 for older builds). That is the single
contract every ps-app recognizes, whatever plugin sits behind the harness:

```
 ps-app (psDoom, psQuake, ...)  ◀── script contract ──▶  harness  ◀── plugin contract ──▶  plugin
       app conformance test                    harness tests (plugins tested here, no app)
```

That splits the testing in two:

- **Harness and plugins** are tested against the harness alone (`harness/tests/`), with no game
  involved: a plugin is correct if the harness turns its output into a valid poll and its
  actions into the right plugin calls.
- **Each app** proves its side once with a conformance test: the app's real script backend runs
  the harness's poll command, sees the listed entities, and its injure and kill reach the
  harness's responder and land on a test file. For psDoom that is `tests/harness_e2e_test.c`
  (inside `psdoom_tests`): the human's setup (`ps-harness get` and `ps-harness`) under v2,
  with grades arriving and a live `delete` and `post` against a CSV it writes, then
  `--contract v1` with the default dry run leaving the CSV alone.

**psQuake, or any later app, gets the same conformance test**: drive its own script backend
against the harness and the CSV plugin on a CSV it creates, with an explicit state root, and
check the same things (entities listed with the expected labels and grades, `delete` removes
the row, `post` lowers the priority, dry run changes nothing, and `--contract v1` still works). No plugin needs an app-specific
change, and a new plugin needs no app test.

## Plugin contract

A plugin is any executable. The harness runs it with an **argument vector, never a shell**,
stdin at `/dev/null` (except `restore`), in its own process group, with a timeout
(`plugin.timeout_s`, default 10 s; on timeout the whole group is killed). The configured
command comes first, so a plugin can take its own options (`csv_rows.py --file F ...`), then:

| Subcommand | Required | Stdout | Meaning |
| --- | --- | --- | --- |
| `describe` | yes | one JSON object | What the plugin supports and what its numbers mean. |
| `list` | yes | JSON Lines, one item per line | The items, in rank order or with `rank`. |
| `nudge <key> amount=N` | no | one JSON result | Lower the item's priority, or move it down, by `N`. |
| `remove <key>` | no | one JSON result | Delete or stop the item. |
| `restore` | no | one JSON result | Undo: the undo record from an earlier result arrives on stdin. |

Exit 0 on success. Unknown subcommands: print `{"ok": false}` and exit non-zero; the harness
only calls verbs that `describe` lists.

### `describe`

```json
{"protocol": 1, "name": "csv",
 "verbs": ["list", "nudge", "remove", "restore"],
 "size":   {"meaning": "priority", "scale": "relative"},
 "load":   {"meaning": "none", "scale": "relative"},
 "nudge":  {"meaning": "lower priority by amount"},
 "remove": {"meaning": "delete the row", "undoable": true}}
```

`size.scale` and `load.scale` pick how the harness puts the numbers on the games' ladders
([below](#normalisation)): `native` (already bytes / percent), `linear` (with `min`, `max`),
or `relative` (the default). The `meaning` strings are for people (`describe` prints them).

### `list` — one JSON object per line

```json
{"key": "r2", "name": "Fix the login bug", "size": 9, "rank": 0}
{"key": "4123@Thu-Nov-12-09:01:02-2026", "name": "node", "size": 734003200, "load": 140, "parent": "4000@...", "background": false}
```

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `key` | string | yes | The plugin's own stable identity for the item. 1 to 1024 characters, no NUL, must not start with `-`. Unique; later duplicates are dropped. |
| `name` | string | no | Shown above the monster (the harness trims it). Default: the key. |
| `grade` | number 0..1 | no | Where the item sits on *its own* range (0 = least, 1 = most). Clamped. Wins over size and load for the v2 `grade` the game sees. |
| `size` | number ≥ 0 | no | Drives toughness by *weight*. If no item has a size, rank decides (top rank = biggest). |
| `load` | number ≥ 0 | no | Drives toughness by *CPU/load*. |
| `rank` | number | no | Lower = more important. Default: line order. Used to sort and to keep the top `max_items`. |
| `parent` | string | no | Key of a parent item (fork-bomb grouping). Unknown parents become `0`. |
| `background` | bool | no | `true` sets v1 `flags` bit 0 (ranked below interactive entities). |

Unknown fields are ignored, so a plugin can carry extra data for its own use.

### Results of `nudge`, `remove`, `restore`

The last non-empty stdout line, a JSON object:

```json
{"ok": true, "message": "row deleted", "undo": {"op": "insert", "key": "r2", "index": 1, "row": ["r2", "Fix bug", "9"]}}
```

`undo` is opaque to the harness. It is stored in the audit log and handed back to `restore`
on stdin. `null` means "not undoable".

### Why JSON Lines, not the v1 TAB format

Plugin names come from spreadsheets, issue titles and agent prompts, and contain TABs,
newlines and `=` signs. The v1 format cannot carry those; JSON escapes anything. Every
language parses it without a library (`json` in Python, `jq` or `python3 -c` in shell, built
in for Node). Typed numbers and booleans avoid "is `1e3` a weight?" questions. The harness
does the one translation to v1, so the TAB framing rules live in one place.

## What the harness adds

### Stable ids

psDoom needs integer ids that never name two things. The harness keeps
`state_dir/ids.json`: `{"version": 1, "next": N, "keys": {key: {"id": n, "seen": t}}}`.

- A new key gets `next`, then `next` increments. The counter **never goes back**, so an id is
  never reused for a different key, even after the key is forgotten. Ids start at 2 (0 and 1
  are reserved by v1).
- A key that comes back gets its old id.
- At most 100 000 keys are remembered (least recently seen dropped); the counter is kept.
- If `ids.json` is missing (while a cache exists) or damaged, refresh **fails closed**
  rather than restart the counter, because a restarted counter could give an id the game
  still holds to a different item. To start over, delete the whole `state_dir`.
- Writes are atomic (temp file + rename) under an `flock` on `state_dir/state.lock`.

Rejected: hashing the key to an id. It needs no state, but two keys can collide, and v1 says
an id must never name two entities at once.

The processes plugin keys a process as `<pid>@<start time>`, so a pid the OS reuses becomes a
new key, a new id and a new monster, and the plugin re-checks the start time before it acts.

### Normalisation

v1 ladders (from [script-backend.md](script-backend.md)):

- size: `0`, 16 MB, 64 MB, 192 MB, 512 MB, 1.5 GB, 4 GB (7 tiers);
- load: `0`, 5, 25, 60, 120, 250, 500 percent.

| scale | weight / load emitted |
| --- | --- |
| `native` | the value itself, clamped to `[0, ladder[max_tier]]` |
| `linear` | `(v − min) / (max − min)` of the declared range, rounded to a tier, emitted as that tier's threshold |
| `relative` | as `linear`, with `min`/`max` taken from the current listing (all equal → bottom tier) |

`max_tier` (default 6) caps the size ladder, for example at 5 to keep Cyberdemons out, or
lower for the shareware WAD. Relative and linear values always land exactly on a rung.

**Grade (contract v2).** Each item also gets a `grade` from 0 to 1, so neither side needs to
know the other's scale: the plugin (or the harness for it) maps onto 0..1, and the game maps
0..1 onto however many monsters it can draw (`min(floor(grade × N), N − 1)`). The grade is
the plugin's own `grade` if it sent one, else the item's size (or its rank, when no item has
a size) on the declared `linear` or `relative` scale, else its load. A `native` scale (the
processes plugin: bytes and CPU percent) is already in the game's own units, so those items
carry **no grade** and the game's *Classify by* option decides, exactly as with psDoom's native
backend. When a plugin sends only a grade, `weight` and `load` are set from it too, for v1.

Labels become what the games can draw: Unicode is folded to ASCII where it can be (`Café` →
`Cafe`), anything else becomes a space, all whitespace (TAB and newline included) collapses
to single spaces, and the result is cut to **31 bytes**. A label can therefore never add a
field or a record to the poll output.

### Caching

The games kill a poll that takes over 1 s (`PSDOOM_POLL_TIMEOUT_MS`). Plugins that call web
APIs, spreadsheets or `ps` on a busy host can be slower, so `get` never runs the plugin:

1. `get` reads `cache.json` and prints it at once.
2. If the cache is older than `cache_ttl_s` (default 2 s), or an action marked it dirty,
   `get` spawns a **detached refresher** (its own session, so the game's timeout kill of the
   poll's process group cannot reach it; no handle on the poll's stdout, so the poll ends
   immediately).
3. The refresher takes `refresh.lock` (non-blocking; a second refresher exits), runs
   `describe` and `list`, assigns ids and rewrites the cache atomically.
4. With no cache yet, `get` waits up to `first_poll_wait_ms` (default 700 ms) for the first
   refresh, then prints an empty listing if it has not landed. The next poll picks it up.
5. A failing plugin is retried once per `cache_ttl_s`, not on every poll.

A successful `remove` also drops the item from the cache at once, so the monster's entity is
gone from the next poll without waiting for a refresh.

## Safety model

This tool can delete things. The defaults assume a mistake.

| Guard | Default | Effect |
| --- | --- | --- |
| `dry_run` | `true` | No plugin action runs. Each hit is audited as `dry-run`. |
| `plugin.allow_remove` | `false` | With `dry_run: false`, `kill` is audited as `blocked`; only `nudge` runs. |
| `rate_limit` | 10 actions / 60 s | Executed actions over the limit are audited as `rate-limited` and skipped. |
| Listed-only | always | `respond` acts only on an id in the current cache, and passes the plugin the key it listed. Unknown ids are `unknown-id` no-ops. |
| Verb map | always | `post` → `nudge`, `delete` → `remove`; the v1 game's `wound` / `kill` only under `--contract v1`. Any other word is an `ignored-verb` no-op that exits 0 (forward compatibility). A verb the plugin does not list is `unsupported`. |
| `amount` | clamped 1..100 | psDoom sends `amount=4`. |
| No shell | always | Plugins get an argument vector; keys and labels are data. |
| Audit log | always | `state_dir/audit.jsonl`: one JSON line per call, with outcome, key, name, amount, action id, plugin message and undo record. JSON escaping keeps hostile text on one line. |
| Undo | where the plugin lists `restore` | `ps-harness undo <action-id>` feeds the stored record to `restore`, once (under a lock). A record holds the value *before* that action, so undo several actions on one item newest first. |
| State location | always | A named plugin's state is `<root>/state/<name>` unless its config names a `state_dir`; a bare `--config` file must name one. It must be absolute and not `/`. |
| Private root and configs | always | The root, and every config file, must be owned by you and not writable by group or others: a config names a command to run. |
| Private state | always | `state_dir` must be a real directory (not a symlink) owned by you and not writable by group or others; it is created `0700`. Otherwise every command refuses, because `respond` trusts the cache in it to map ids to keys. |

`post` and `delete` always exit 0 (the game does not read it); the audit log is the record.
`get` always prints a valid poll (v2, or v1 under `--contract v1`), with a `#` comment on
error.

The harness does not decide what is *safe to list*. That stays the plugin's job, as in v1: the
processes plugin keeps the session-critical deny-list from `scripts/poll-processes.sh`, plus
the game and the Terminal and iTerm2 apps. It has no launcher-chain guard like the native
backend, and its pid-reuse check compares a start time with 1-second resolution, so a pid
reused within the same second would pass it.

## Configuration

```
ps-harness [--root DIR] [--plugin NAME | --config FILE] [--contract v1|v2] COMMAND ...
```

| Command | What it does |
| --- | --- |
| `get` | Print the poll output (what the game reads). |
| `post <id> [key=value ...]` | Nudge the item: lower its priority, move it down, renice it. |
| `delete <id> [key=value ...]` | Remove the item (only when its config allows it). |
| `use <name>` | Set the current plugin, and remember it. |
| `plugins` | List the plugins it knows; `*` marks the current one. |
| `refresh` | Run the plugin's `list` now, synchronously. |
| `describe` | Print the plugin's `describe`, validated. |
| `undo <action-id>` | Replay an action's undo record (ids are in the audit log). |

**The root** holds the current choice and the per-plugin configs:

```
<root>/current               the current plugin's name (written by `use`)
<root>/plugins/<name>.json   that plugin's config
<root>/state/<name>/         its ids, cache, audit log and rate limit
```

It is `--root DIR`, else `$PS_HARNESS_ROOT`, else `~/.config/ps-harness`. `--root` beats the
variable. **Which plugin**: `--config FILE` (a config file anywhere; it must name its own
`state_dir`), else `--plugin NAME` for this one call, else the `current` file, else `ps`.
`--plugin` and `--config` together are refused, and an unknown name fails with the list of
names it knows. `ps` (your own processes) is built in, dry run, remove off; a
`<root>/plugins/ps.json` replaces it.

**A plugin config** keeps each plugin's own settings:

```json
{
  "dry_run": true,
  "cache_ttl_s": 2,
  "first_poll_wait_ms": 700,
  "max_items": 256,
  "max_tier": 6,
  "rate_limit": {"actions": 10, "per_s": 60},
  "plugin": {
    "name": "csv",
    "command": ["python3", "${HARNESS_DIR}/plugins/csv_rows.py",
                "--file", "/Users/you/todo.csv", "--priority-column", "priority"],
    "allow_remove": false,
    "timeout_s": 10
  }
}
```

`${HARNESS_DIR}` in `plugin.command` expands to the harness directory. `harness/examples/`
has starting points (they carry a `state_dir` placeholder for `--config` use; drop that line
for a file under `<root>/plugins/`).

**Wire it to psDoom** (put `harness/` on PATH, or use the launcher's full path; paths must not
contain spaces, because the respond command is split on whitespace):

```sh
export PSDOOM_POLL_CMD="ps-harness get"
export PSDOOM_RESPOND_CMD="ps-harness"          # psDoom appends post|delete <id> ...
```

For a game build that only speaks v1, add `--contract v1` to both.

### No-kill run against psDoom (dry run, your processes)

```sh
cd ~/Source/psDoom-plugin-harness
export PATH="$PWD/harness:$PATH"        # or: ln -s "$PWD/harness/ps-harness" ~/bin/
ps-harness use ps                       # remembered in ~/.config/ps-harness/current
ps-harness plugins                      # * ps   built-in
ps-harness get | head                   # what psDoom will see (the first call may be empty)
export PSDOOM_POLL_CMD="ps-harness get"
export PSDOOM_RESPOND_CMD="ps-harness"
build/psDoom.app/Contents/MacOS/psDoom  # run from this shell so it inherits PATH and env
```

Every shot is then audited in `~/.config/ps-harness/state/ps/audit.jsonl` as `dry-run`, and
nothing is signalled: the built-in `ps` is dry run with remove off.

## Writing a plugin

The smallest useful plugin, in `sh`: a read-only view of the lines of a text file, top line
first (it takes the file as its first argument, from `plugin.command`):

```sh
#!/bin/sh
f=$1; shift
case $1 in
  describe) echo '{"verbs":["list"]}' ;;
  list)     awk '{ gsub(/["\\[:cntrl:]]/, ""); printf "{\"key\":\"%s\",\"name\":\"%s\"}\n", $0, $0 }' "$f" ;;
  *)        echo '{"ok":false}'; exit 1 ;;
esac
```

It keys each line by its text, not its line number, because a line number changes as soon as
a line above it goes. Rules of thumb:

1. **Key by a real identity.** A database id, a URL, a file name plus creation time. Never a
   position. If the thing can be replaced by a look-alike (a pid), include something that
   changes when it is replaced (a start time) and check it before acting.
2. **Re-check before acting.** The listing is up to `cache_ttl_s` old.
3. **Return an undo record** for anything you can put back: the deleted row, the old value.
4. **Be quick in `list`** if you can, but correctness first: the harness caches.
5. **Declare only the verbs you implement.** A read-only plugin lists `["list"]`.

## The examples

- **Processes** (`plugins/processes.py`, shipped). Key `pid@start`, size = resident bytes
  (`native`), load = CPU percent (`native`), `nudge` = raise nice by `amount` (cumulative,
  capped at 20), `remove` = SIGTERM. Not undoable: an unprivileged user cannot lower nice
  again, and a stopped process cannot be restarted.
- **Spreadsheet rows** (`plugins/csv_rows.py`, shipped). Key = a key column; with a priority
  column, rank by it and `nudge` lowers it; without one, `nudge` moves the row down `amount`
  places. `remove` deletes the row and returns it as the undo record. Writes are atomic and
  locked by a sidecar `.<file>.lock`. For a live Excel or Google sheet, the same plugin shape
  works over their APIs (Microsoft Graph workbook endpoints, the Google Sheets API).
- **Agents and workers** (sketch). `list` = running agent sessions or queue workers, key =
  session id, size = tokens or queued tasks, load = CPU or tasks per minute; `nudge` = lower
  the worker's queue priority or pause it; `remove` = cancel the session (not undoable) or
  drain it (undoable: resume). Keep `allow_remove` off until the cancel path is proven.
- **Wishlists** (not recommended). Amazon wishlists have no public API, so a plugin would
  have to scrape pages or drive a browser. That is fragile and likely against the site's
  terms. API-backed stand-ins fit the same shape:
  - **to-do apps** with APIs (Todoist, Microsoft To Do via Graph): key = task id, rank =
    order, `nudge` = lower priority, `remove` = complete or delete (undo: reopen);
  - **issue trackers** (GitHub Issues via `gh`, Jira): key = issue number, size = votes or
    age, `nudge` = drop a priority label, `remove` = close (undo: reopen);
  - **macOS Reminders** (via `osascript` or Shortcuts): key = reminder id, `remove` =
    complete (undo: un-complete).

## Limits of the prototype

- One plugin per game session: the games run one poll command. Switching with `use` takes
  effect at the next poll; the new plugin has its own ids, so the old monsters retire.
- After a `remove`, a refresh that had already started listing can bring the item back for
  one poll. A second hit then fails in the plugin (`no row with that key`) and is audited.
- Rate-limited and blocked hits still look like a hit in the game: the monster reappears,
  like a process that ignores SIGTERM. The contract has no channel to say why.
- The launcher runs `python3` from PATH (`#!/usr/bin/env python3`). A psDoom started from
  Finder or `open` gets a minimal PATH, so start it from a shell, or give the full path.
- The CSV plugin may change a file's quoting style on write (cells, order, line endings,
  BOM and file mode are kept; a symlink is written through). A hard link to the file is
  broken by the atomic rename. A key names the first row that has it; later duplicates are
  not listed. Do not point it at a file another program is editing at the same time.

## Appendix: contract proposals

**Done in v2** (now in [script-backend.md](script-backend.md#contract-v2)): the 0..1 `grade`
field, and the neutral response verbs `post` / `delete`, both behind the `#!psdoom v2` header
so v1 scripts are untouched.

**Proposals only.** The script contract is a reserved surface; none of the following is
implemented, and each needs the project owner's go before any change to
`docs/script-backend.md` or to how psDoom or psQuake call scripts.

1. **String ids / `key=`.** Let a poll line carry the plugin's string key (or replace the
   integer id), echoed back on respond, so the harness would not need its id map to act.
2. **`kind=` poll field.** A coarse category (`process`, `row`, `agent`) a game could theme by.
3. **A result channel.** Have the game read a one-line status from the responder (or a
   status file), so the HUD can show "dry run", "blocked" or "rate limited" instead of a
   monster that silently comes back.
4. **`rank=` poll field.** Let the game's cap truncation use the plugin's order directly
   instead of only `flags` bit 0.
5. **Declared capabilities.** A `#!psdoom v2 verbs=post,delete undo` header, so the game can
   grey out actions a backend does not support, and offer an in-game "resurrect" (undo).
6. **`dry=1` respond extra** from a game-side no-kill toggle, so the safe mode can be set in
   the menu rather than in each config.
7. **Longer labels / UTF-8**, if the renderer ever draws them; the harness would then raise
   its 31-byte cut.
