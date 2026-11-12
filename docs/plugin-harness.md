# psDoom plugin harness — design

The [script backend](script-backend.md) lets psDoom show and act on anything a script can
list. Each backend script pair re-solves the same problems: stable integer ids, scaling a
metric onto the monster ladders, trimming labels, staying under the 1-second poll timeout,
and not deleting the wrong thing. The **plugin harness** solves them once. A *plugin* then
only has to answer three questions about its domain: *what is there* (GET), *push this one
down* (POST), *get rid of this one* (DELETE).

```
 psDoom / psQuake                    harness/psdoom_harness.py                plugin
 ───────────────                     ─────────────────────────                ──────
 PSDOOM_POLL_CMD  ── sh -c ──▶  poll ── reads cache.json ─────────────┐
   (1 s timeout)                     └─ stale? spawn detached refresh ─┼─▶  describe, list
                                                                      │
 PSDOOM_RESPOND_CMD ─ exec ──▶  respond wound|kill <id> ─▶ id → key ──┼─▶  nudge <key> amount=N
   (detached)                         dry run? opt-in? rate? audit    │    remove <key>
                                                                      │
 you, at a terminal  ────────▶  undo <action-id> ────────────────────┴─▶  restore  (stdin)
```

The games keep the **v1 contract unchanged**. The harness *is* a v1 poll command and a v1
responder; psDoom cannot tell it from a hand-written script. Ideas that would need the
contract to change are in the [appendix](#appendix-v2-contract-proposals), as proposals only.

## Where it lives and what it needs

- `harness/psdoom_harness.py` — the harness, one file.
- `harness/plugins/processes.py` — your own processes, the native behaviour.
- `harness/plugins/csv_rows.py` — rows of a CSV file.
- `harness/tests/` — unit and subprocess tests of the harness and both plugins. Run them
  from a plain checkout with `python3 -I harness/tests/run_tests.py` (no CMake; scratch goes
  to the git-ignored `harness/tests/.scratch/`). psDoom's `ctest` also runs them as
  `psdoom_harness_tests`, with a scratch root in its build tree.
- `tests/harness_e2e_test.c` — psDoom's app-side conformance test ([below](#one-contract-for-every-ps-app)).

Python 3.10+ standard library only. The build already requires Python 3.10 (`CMakeLists.txt`
uses it for the WAD tools), so the harness adds no dependency. A top-level `harness/` keeps it
apart from `scripts/` (the copy-me `sh` reference pair) and `tools/` (build-time tools): it is
a runtime component with its own tests.

**Self-contained.** Nothing in `harness/` imports or reads from psDoom's source or build, and
its tests need only Python, so the folder can later move to its own repository unchanged.
psDoom keeps only two hooks into it: the `ctest` registration and the conformance test.

## One contract for every ps-app

The harness's poll output and its responder calls *are* the
[v1 script-backend contract](script-backend.md). That is the single contract every ps-app
recognizes, whatever plugin sits behind the harness:

```
 ps-app (psDoom, psQuake, ...)  ◀── v1 contract ──▶  harness  ◀── plugin contract ──▶  plugin
       app conformance test                    harness tests (plugins tested here, no app)
```

That splits the testing in two:

- **Harness and plugins** are tested against the harness alone (`harness/tests/`), with no game
  involved: a plugin is correct if the harness turns its output into valid v1 and its actions
  into the right plugin calls.
- **Each app** proves its side once with a conformance test: the app's real script backend runs
  the harness's poll command, sees the listed entities, and its injure and kill reach the
  harness's responder and land on a test file. For psDoom that is `tests/harness_e2e_test.c`
  (inside `psdoom_tests`): a live kill and wound against a CSV it writes, then the default
  dry-run config leaving the CSV alone.

**psQuake, or any later app, gets the same conformance test**: drive its own script backend
against the harness and the CSV plugin on a CSV it creates, with an explicit state root, and
check the same four things (entities listed with the expected labels and sizes, `kill` removes
the row, `wound` lowers the priority, dry run changes nothing). No plugin needs an app-specific
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

Labels become what the games can draw: Unicode is folded to ASCII where it can be (`Café` →
`Cafe`), anything else becomes a space, all whitespace (TAB and newline included) collapses
to single spaces, and the result is cut to **31 bytes**. A label can therefore never add a
field or a record to the v1 output.

### Caching

The games kill a poll that takes over 1 s (`PSDOOM_POLL_TIMEOUT_MS`). Plugins that call web
APIs, spreadsheets or `ps` on a busy host can be slower, so `poll` never runs the plugin:

1. `poll` reads `cache.json` and prints it as v1 at once.
2. If the cache is older than `cache_ttl_s` (default 2 s), or an action marked it dirty,
   `poll` spawns a **detached refresher** (its own session, so the game's timeout kill of the
   poll's process group cannot reach it; no handle on the poll's stdout, so the poll ends
   immediately).
3. The refresher takes `refresh.lock` (non-blocking; a second refresher exits), runs
   `describe` and `list`, assigns ids and rewrites the cache atomically.
4. With no cache yet, `poll` waits up to `first_poll_wait_ms` (default 700 ms) for the first
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
| Verb map | always | `wound` → `nudge`, `kill` → `remove`. Any other verb is an `ignored-verb` no-op (v1 forward compatibility). A verb the plugin does not list is `unsupported`. |
| `amount` | clamped 1..100 | psDoom sends `amount=4`. |
| No shell | always | Plugins get an argument vector; keys and labels are data. |
| Audit log | always | `state_dir/audit.jsonl`: one JSON line per call, with outcome, key, name, amount, action id, plugin message and undo record. JSON escaping keeps hostile text on one line. |
| Undo | where the plugin lists `restore` | `psdoom_harness.py --config C undo <action-id>` feeds the stored record to `restore`, once (under a lock). A record holds the value *before* that action, so undo several actions on one item newest first. |
| Explicit state | always | `state_dir` must be in the config, absolute, and not `/`. There is no default location and nothing is read from environment variables. |
| Private state | always | `state_dir` must be a real directory (not a symlink) owned by you and not writable by group or others; it is created `0700`. Otherwise every command refuses, because `respond` trusts the cache in it to map ids to keys. |

`respond` always exits 0 (psDoom does not read it); the audit log is the record. `poll`
always prints valid v1, with a `#` comment on error.

The harness does not decide what is *safe to list*. That stays the plugin's job, as in v1: the
processes plugin keeps the session-critical deny-list from `scripts/poll-processes.sh`, plus
the game and the Terminal and iTerm2 apps. It has no launcher-chain guard like the native
backend, and its pid-reuse check compares a start time with 1-second resolution, so a pid
reused within the same second would pass it.

## Configuration

One JSON file per plugin, passed as `--config` to every command.

```json
{
  "state_dir": "/Users/you/psdoom-harness/processes",
  "dry_run": true,
  "cache_ttl_s": 2,
  "first_poll_wait_ms": 700,
  "max_items": 256,
  "max_tier": 6,
  "rate_limit": {"actions": 10, "per_s": 60},
  "plugin": {
    "name": "processes",
    "command": ["python3", "${HARNESS_DIR}/plugins/processes.py"],
    "allow_remove": false,
    "timeout_s": 10
  }
}
```

`${HARNESS_DIR}` in `plugin.command` expands to the harness directory. Ready-made files are
in `harness/examples/`; each has a placeholder `state_dir` you must replace.

Commands:

```sh
H=harness/psdoom_harness.py
python3 $H --config C describe     # check the plugin, print its describe
python3 $H --config C refresh      # run list now (synchronously)
python3 $H --config C poll         # what psDoom sees
python3 $H --config C respond kill 17          # what psDoom runs
python3 $H --config C undo 4a267496a7d7        # undo a recorded action
```

Wire it to psDoom (paths must not contain spaces: v1 splits `PSDOOM_RESPOND_CMD` on
whitespace):

```sh
export PSDOOM_POLL_CMD="python3 $PWD/harness/psdoom_harness.py --config $CFG poll"
export PSDOOM_RESPOND_CMD="python3 $PWD/harness/psdoom_harness.py --config $CFG respond"
```

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

- One plugin per config, one config per game session (the games run one poll command).
- After a `remove`, a refresh that had already started listing can bring the item back for
  one poll. A second hit then fails in the plugin (`no row with that key`) and is audited.
- Rate-limited and blocked hits still look like a hit in the game: the monster reappears,
  like a process that ignores SIGTERM. v1 has no channel to say why.
- The CSV plugin may change a file's quoting style on write (cells, order, line endings,
  BOM and file mode are kept; a symlink is written through). A hard link to the file is
  broken by the atomic rename. A key names the first row that has it; later duplicates are
  not listed. Do not point it at a file another program is editing at the same time.

## Appendix: v2 contract proposals

**Proposals only.** The v1 contract is a reserved surface; none of this is implemented, and
each item needs the project owner's go before any change to `docs/script-backend.md` or to
how psDoom or psQuake call scripts.

1. **`key=` poll field and respond extra.** Let a poll line carry the plugin's string key,
   echoed back as `key=<…>` on respond. The harness would then not need the id→key lookup to
   act (it would still need ids). Backward compatible: unknown keys are already ignored.
2. **A result channel.** Have the game read a one-line status from the responder (or a
   status file), so the HUD can show "dry run", "blocked" or "rate limited" instead of a
   monster that silently comes back.
3. **`rank=` poll field.** Let the game's cap truncation use the plugin's order directly
   instead of only `flags` bit 0.
4. **Declared capabilities.** A `#!psdoom v1 verbs=wound,kill undo` header, so the game can
   grey out actions a backend does not support, and offer an in-game "resurrect" (undo).
5. **`dry=1` respond extra** from a game-side no-kill toggle, so the safe mode can be set in
   the menu rather than in each config.
6. **Longer labels / UTF-8**, if the renderer ever draws them; the harness would then raise
   its 31-byte cut.
