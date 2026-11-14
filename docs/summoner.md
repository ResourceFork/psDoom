# Summoner — design

The [script backend](script-backend.md) lets psDoom show and act on anything a script can
list. Each backend script pair re-solves the same problems: stable integer ids, scaling a
metric onto the monster ladders, trimming labels, staying under the 1-second poll timeout,
and not deleting the wrong thing. **Summoner** solves them once. A *plugin* then
only has to answer three questions about its domain: *what is there* (GET), *push this one
down* (POST), *get rid of this one* (DELETE).

```
 psDoom / psQuake                    summon                               plugin
 ───────────────                     ──────────                               ──────
 PSDOOM_POLL_CMD  ── sh -c ──▶  get ── reads cache.json ──────────────┐
   "summon get"                  └─ stale? spawn detached refresh ─┼─▶  describe, list
   (1 s timeout)                                                      │
 PSDOOM_RESPOND_CMD ─ exec ──▶  post|delete <id> ─▶ id → key ─────────┼─▶  nudge <key> amount=N
   "summon"                      dry run? opt-in? rate? audit     │    remove <key>
                                                                      │
 you, at a terminal  ────────▶  undo <action-id> ────────────────────┴─▶  restore  (stdin)
```

Summoner speaks the apps' [script contract](script-backend.md): TAB-separated
`key=value` lines with a 0..1 `grade` per item, and the game sends `post` / `delete`. To the
game, Summoner is just a poll command and a responder. Further contract ideas are in
the [appendix](#appendix-contract-proposals), as proposals only.

## Where it lives and what it needs

- `summoner/summon` — the command: a thin launcher for Summoner (put it on PATH or
  symlink it; nothing is installed for you).
- `summoner/summoner.py` — Summoner, one file.
- `summoner/plugins/processes.py` — your own processes, the native behaviour.
- `summoner/plugins/csv_rows.py` — rows of a CSV file.
- `summoner/tests/` — unit and subprocess tests of Summoner and both plugins. Run them
  from a plain checkout with `python3 -I summoner/tests/run_tests.py` (no CMake; scratch goes
  to `summoner/tests/.scratch/`, ignored by `summoner/.gitignore`; a symlinked root is
  refused). psDoom's `ctest` also runs them as `psdoom_summoner_tests`, with a scratch root
  in its build tree.
- `tests/summoner_e2e_test.c` — psDoom's app-side conformance test ([below](#one-contract-for-every-ps-app)).

Python 3.10+ standard library only. The build already requires Python 3.10 (`CMakeLists.txt`
uses it for the WAD tools), so Summoner adds no dependency. A top-level `summoner/` keeps it
apart from `scripts/` (the copy-me `sh` reference pair) and `tools/` (build-time tools): it is
a runtime component with its own tests.

**Self-contained.** Nothing in `summoner/` imports or reads from psDoom's source or build, and
its tests need only Python, and it carries its own `.gitignore` and a summary of the script
contract in its README, so the folder can later move to its own repository unchanged.
psDoom keeps only two hooks into it: the `ctest` registration and the conformance test.

## One contract for every ps-app

Summoner's poll output and its responder calls *are* the
[script-backend contract](script-backend.md). That is the single
contract every ps-app recognizes, whatever plugin sits behind Summoner:

```
 ps-app (psDoom, psQuake, ...)  ◀── script contract ──▶  summon  ◀── plugin contract ──▶  plugin
       app conformance test                    Summoner tests (plugins tested here, no app)
```

That splits the testing in two:

- **Summoner and plugins** are tested against Summoner alone (`summoner/tests/`), with no game
  involved: a plugin is correct if Summoner turns its output into a valid poll and its
  actions into the right plugin calls.
- **Each app** proves its side once with a conformance test: the app's real script backend runs
  Summoner's poll command, sees the listed entities, and its injure and kill reach the
  Summoner's responder and land on a test file. For psDoom that is `tests/summoner_e2e_test.c`
  (inside `psdoom_tests`): the human's setup (`summon get` and `summon`), with
  grades arriving and a live `delete` and `post` against a CSV it writes, then the default
  dry run leaving the CSV alone.

**psQuake, or any later app, gets the same conformance test**: drive its own script backend
against Summoner and the CSV plugin on a CSV it creates, with an explicit state root, and
check the same things (entities listed with the expected labels and grades, `delete` removes
the row, `post` lowers the priority, dry run changes nothing). No plugin needs an app-specific
change, and a new plugin needs no app test.

## Plugin contract

A plugin is any executable. Summoner runs it with an **argument vector, never a shell**,
stdin at `/dev/null` (except `restore`), in its own process group, with a timeout
(`plugin.timeout_s`, default 10 s; on timeout the whole group is killed). The configured
command comes first, so a plugin can take its own options (`csv_rows.py --file F ...`), then:

| Subcommand | Required | Stdout | Meaning |
| --- | --- | --- | --- |
| `describe` | yes | one JSON object | What the plugin supports and what its numbers mean. |
| `list [sort=ID:dir,..] [group=ID] [limit=N]` | yes | JSON Lines, one item per line | The items, **curated**: in the order to show them, trimmed to `limit`, graded on the primary sort. The words come only to a plugin that declares dimensions. |
| `nudge <key> amount=N` | no | one JSON result | Lower the item's priority, or move it down, by `N`. |
| `remove <key>` | no | one JSON result | Delete or stop the item. |
| `restore` | no | one JSON result | Undo: the undo record from an earlier result arrives on stdin. |

Exit 0 on success. Unknown subcommands: print `{"ok": false}` and exit non-zero; Summoner
only calls verbs that `describe` lists.

### `describe`

```json
{"protocol": 1, "name": "csv",
 "verbs": ["list", "nudge", "remove", "restore"],
 "size":   {"meaning": "priority", "scale": "relative"},
 "load":   {"meaning": "none", "scale": "relative"},
 "nudge":  {"meaning": "lower priority by amount"},
 "remove": {"meaning": "delete the row", "undoable": true},
 "dimensions": [
   {"id": "priority", "label": "priority", "kind": "numeric", "dir": "desc", "roles": ["sort"]},
   {"id": "team", "label": "Team", "kind": "category", "roles": ["group"]}],
 "default_sort": "priority"}
```

**Dimensions** are what a host may ask the plugin to sort or group by. Each has an `id`
(`[a-z][a-z0-9_]{0,15}`, safe on a command line), a `label` (a game shows it in its menu), a
`kind` (`numeric` sorts, `category` groups), a default `dir` (`asc` or `desc`), a `unit`, and
`roles` (`sort`, `group`; default by kind). `default_sort` names the sort used when the host
asks for none, or for one the plugin does not offer. Summoner validates all of it.

`size.scale` and `load.scale` pick how Summoner puts the numbers on the games' ladders
([below](#normalisation)): `native` (already bytes / percent), `linear` (with `min`, `max`),
or `relative` (the default). The `meaning` strings are for people (`describe` prints them).

### `list` — one JSON object per line

```json
{"key": "r2", "name": "Fix the login bug", "size": 9, "values": {"priority": 9}, "grade": 1.0, "group": "red"}
{"key": "4123@Thu-Nov-12-09:01:02-2026", "name": "node", "size": 734003200, "load": 140, "values": {"relevance": 142, "mem": 734003200, "cpu": 140.2, "age": 3600}, "grade": 0.97, "parent": "4000@...", "background": false}
```

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `key` | string | yes | The plugin's own stable identity for the item. 1 to 1024 characters, no NUL, must not start with `-`. Unique; later duplicates are dropped. |
| `name` | string | no | Shown above the monster (Summoner trims it). Default: the key. |
| `grade` | number 0..1 | no | Where the item sits on the requested primary dimension (1 = the best end of its direction). Clamped. It picks the monster. Missing: Summoner computes it from `values[primary]`, else from size, load or position. |
| `values` | object | no | The item's raw value per numeric dimension id (for a missing grade, and for people reading `describe`). |
| `group` | string | no | The item's group when the host asked for `group=`; Summoner trims it like a label. |
| `pid` | integer > 1 | no | The local process the item stands for. Each `get` drops items whose pid is in its own calling chain (fail-closed: if the chain cannot be read, no item with a pid is served). Must be a JSON integer; a string is ignored. |
| `size` | number ≥ 0 | no | The poll's `weight` (bytes, or scaled). If no item has a size, the position decides (first = biggest). |
| `load` | number ≥ 0 | no | The poll's `load`. |
| `parent` | string | no | Key of a parent item (fork-bomb grouping). Unknown parents become `0`. |
| `background` | bool | no | `true` sets the poll's `flags` bit 0. |

Unknown fields are ignored, so a plugin can carry extra data for its own use.

### Results of `nudge`, `remove`, `restore`

The last non-empty stdout line, a JSON object:

```json
{"ok": true, "message": "row deleted", "undo": {"op": "insert", "key": "r2", "index": 1, "row": ["r2", "Fix bug", "9"]}}
```

`undo` is opaque to Summoner. It is stored in the audit log and handed back to `restore`
on stdin. `null` means "not undoable".

### Why JSON Lines, not the poll's TAB format

Plugin names come from spreadsheets, issue titles and agent prompts, and contain TABs,
newlines and `=` signs. The poll format cannot carry those; JSON escapes anything. Every
language parses it without a library (`json` in Python, `jq` or `python3 -c` in shell, built
in for Node). Typed numbers and booleans avoid "is `1e3` a weight?" questions. Summoner
does the one translation to the poll format, so the TAB framing rules live in one place.

## What Summoner adds

### Stable ids

psDoom needs integer ids that never name two things. Summoner keeps one map for the whole
root, `<root>/ids.json` (`{"version": 1, "next": N, "keys": {"<plugin>/<key>": {"id": n, "seen":
t}}}`), so ids are unique across every plugin in it. `<plugin>` is the plugin's file name
(`plugins/<plugin>.json`, or `ps`), which cannot contain `/`, never the free-text `plugin.name`
inside the config, so two configs copied from one example cannot share ids. A named plugin's
state is always `<root>/state/<plugin>/` (a `state_dir` in its config is refused), so the
fail-closed check below sees every plugin. A bare `--config` file keeps its own map in its
`state_dir`.

- A new key gets `next`, then `next` increments. The counter **never goes back**, so an id is
  never reused for a different key, even after the key is forgotten. Ids start at 2 (0 and 1
  are reserved by the contract).
- A key that comes back gets its old id.
- At most 100 000 keys are remembered across the root (least recently seen dropped); the
  counter is kept. A plugin whose keys churn (`ps`) can push out an idle plugin's keys, which
  then get new ids (new monsters) when you switch back: safe, never a reused id.
- If `ids.json` is missing (while a cache exists) or damaged, refresh **fails closed**
  rather than restart the counter, because a restarted counter could give an id the game
  still holds to a different item. To start over, delete the root's `state/` and `ids.json`
  (or a `--config` file's whole `state_dir`).
- Writes are atomic (temp file + rename) under an `flock` (`<root>/ids.json.lock`).

Rejected: hashing the key to an id. It needs no state, but two keys can collide, and the contract says
an id must never name two entities at once.

The processes plugin keys a process as `<pid>@<start time>`, so a pid the OS reuses becomes a
new key, a new id and a new monster, and the plugin re-checks the start time before it acts.

### Order: the plugin curates

Summoner **never re-sorts**. The plugin's order is the poll's order, and psDoom keeps it
([Trust](script-backend.md#trust)), so the first items become the monsters. `max_items` and the
host's `limit` cut the tail. `rank` is no longer read.

A host asks for an order with request words: `summon get sort=cpu:desc,mem group=app
limit=64`. Summoner checks each id against `describe`. An unknown id, a wrong kind or a bad
limit is dropped with a `# summon: ignored …` line, and `default_sort` takes the sort's
place. The poll never fails because of a request. The checked words reach the plugin as
`list sort=cpu:desc,mem:desc group=app limit=64`. The output starts with `# dim` lines for
every dimension and `# sort` / `# group` lines for what was applied; items carry `group=`.
**The cache is keyed by the request**, so a new request refreshes at once (waiting up to
`first_poll_wait_ms`) instead of serving an answer to the old question.

### Normalisation

The ladders (from [script-backend.md](script-backend.md)):

- size: `0`, 16 MB, 64 MB, 192 MB, 512 MB, 1.5 GB, 4 GB (7 tiers);
- load: `0`, 5, 25, 60, 120, 250, 500 percent.

| scale | weight / load emitted |
| --- | --- |
| `native` | the value itself, clamped to `[0, ladder[max_tier]]` |
| `linear` | `(v − min) / (max − min)` of the declared range, rounded to a tier, emitted as that tier's threshold |
| `relative` | as `linear`, with `min`/`max` taken from the current listing (all equal → bottom tier) |

`max_tier` (default 6) caps the size ladder, for example at 5 to keep Cyberdemons out, or
lower for the shareware WAD. Relative and linear values always land exactly on a rung.

**Grade.** Each item also gets a `grade` from 0 to 1, so neither side needs to
know the other's scale: the plugin (or Summoner for it) maps onto 0..1, and the game maps
0..1 onto however many monsters it can draw (`min(floor(grade × N), N − 1)`). The grade is
the plugin's own `grade` if it sent one (both shipped plugins do: the item's place on the
primary sort among the listed items); else, from the primary dimension's `values`, relative
over the listing in the requested direction; else the item's size (or position) on the
declared `linear` or `relative` scale, else its load. A `native` size scale with no other
source gives **no grade**, and the game's *Monster size by* option decides. Below 6, `max_tier`
scales every grade by (`max_tier` + 0.5) / 7: on the full ladder the top grade lands exactly on
the capped monster, and on a WAD with fewer drawable tiers it never lands above it (it may land
a tier lower). At the default 6, grades pass through unchanged.
When a plugin sends only a grade, `weight` and `load` are set from it too.

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
| Host `mode` | `dryrun` | Sent by the game with every event: `dryrun` runs nothing, `post-only` runs `post` and refuses `delete`, `delete` allows both. Missing or unknown = `dryrun`. Audited with `mode` and `reason: host mode`. |
| `dry_run` | `true` | A per-plugin restriction on top of the host's mode: no plugin action runs. Audited `dry-run`, `reason: plugin config`. |
| `plugin.allow_remove` | `false` | Also on top of the mode: `delete` is audited as `blocked`; only `post` (nudge) runs. The effective action is the **stricter** of the host's mode and these two. |
| `rate_limit` | 10 actions / 60 s | Executed actions over the limit are audited as `rate-limited` and skipped. |
| Listed-only | always | `post` / `delete` act only on an id in the current cache, and pass the plugin the key it listed. Unknown ids are `unknown-id` no-ops. |
| Unique ids | always | Named plugins share one id map per root, so after `use` switches plugin, the old ids are no longer listed and psDoom's checked actions send nothing for them. |
| Label check | always | When the game sends `label=`, it must equal the item's current label, or the call is a `label-mismatch` no-op. psDoom sends the label from its own latest poll, so this guards a stale or mixed-up snapshot; it does not stand in for unique ids. An item renamed between psDoom's poll and the hit is refused until psDoom polls again (safe). With no label (an empty name), there is nothing to check. |
| Verb map | always | `post` → `nudge`, `delete` → `remove`. Any other word is an `ignored-verb` no-op that exits 0 (forward compatibility). A verb the plugin does not list is `unsupported`. |
| `amount` | clamped 1..100 | psDoom sends `amount=4`. |
| No shell | always | Plugins get an argument vector; keys and labels are data. |
| Audit log | always | `state_dir/audit.jsonl`: one JSON line per call, with outcome, key, name, amount, action id, plugin message and undo record. JSON escaping keeps hostile text on one line. |
| Undo | where the plugin lists `restore` | `summon undo <action-id>` feeds the stored record to `restore`, once (under a lock). A record holds the value *before* that action, so undo several actions on one item newest first. |
| State location | always | A named plugin's state is always `<root>/state/<name>` (a `state_dir` in its config is refused); a bare `--config` file must name one, absolute and not `/`. |
| Private root and configs | always | The root, and every config file, must be owned by you and not writable by group or others: a config names a command to run. |
| Private state | always | `state_dir` must be a real directory (not a symlink) owned by you and not writable by group or others; it is created `0700`. Otherwise every command refuses, because `respond` trusts the cache in it to map ids to keys. |

`post` and `delete` always exit 0 (the game does not read it); the audit log is the record.
`get` always prints a valid poll, with a `#` comment on error.

### Trust

**The plugin is trusted.** psDoom does not filter or rank what Summoner lists
([script-backend.md](script-backend.md#trust)), and Summoner does not either: what is safe to
list is the plugin's job. The game only reports which entity got `post` or `delete`, and its
mode; Summoner decides what runs.

So the `ps` plugin leaves out, itself, everything psDoom's native backend would:

- psDoom's protected, session-critical names (loginwindow, WindowServer, Dock, Finder,
  SystemUIServer, launchd, kernel_task), the games and the usual terminal apps;
- pid 0 and 1;
- itself and its own parent chain (the Summoner refresher running it);
- the **host chain**: `summon get` walks its own parent chain (its shell, psDoom, psDoom's
  launchers) and hands it to the detached refresher, which passes it to the plugin in
  `SUMMONER_HOST_PIDS`. That is psDoom's native launcher-chain guard, moved into the plugin.

It lists only processes whose effective and real uid are both yours (`ps -x` selects by
real uid, so a setuid `login` or `sudo` would otherwise show), and leaves out session
plumbing (login, sudo, su, sshd, ssh-agent, tmux, screen, iTermServer-*, common terminal
apps). Each item carries its `pid`, and **every `summon get` drops items in its own calling
chain before printing**, so a listing made for another caller (a second psDoom, a `summon get`
from a terminal) can never hand psDoom its own chain. That serve-time filter is the guard
that holds: psDoom double-forks its responder, so at `post`/`delete` time Summoner no longer
sees psDoom among its ancestors (the plugin still re-checks the chain it does see). Its
pid-reuse check compares a start time with 1-second resolution, so a pid reused within the
same second would pass it. `relevance` uses resident memory (`ps` has no footprint), so it
approximates psDoom's native score, which uses the physical footprint.

## Configuration

```
summon [--root DIR] [--plugin NAME | --config FILE] COMMAND ...
```

| Command | What it does |
| --- | --- |
| `get [sort=ID[:asc\|desc][,ID..]] [group=ID] [limit=N]` | Print the poll output (what the game reads), in the plugin's order for that request. |
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
<root>/ids.json              one id map for every plugin in the root
<root>/state/<name>/         its cache, audit log and rate limit
```

It is `--root DIR`, else `$SUMMONER_ROOT`, else `~/.config/summoner`. `--root` beats the
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
    "command": ["python3", "${SUMMONER_DIR}/plugins/csv_rows.py",
                "--file", "/Users/you/todo.csv", "--priority-column", "priority"],
    "allow_remove": false,
    "timeout_s": 10
  }
}
```

`${SUMMONER_DIR}` in `plugin.command` expands to the `summoner/` directory. `summoner/examples/`
has starting points (they carry a `state_dir` placeholder for `--config` use; remove that line
for a file under `<root>/plugins/`, where it is refused).

**Wire it to psDoom** (put `summoner/` on PATH, or use the launcher's full path; paths must not
contain spaces, `--root` included, because the respond command is split on whitespace):

```sh
export PSDOOM_POLL_CMD="summon get"
export PSDOOM_RESPOND_CMD="summon"          # psDoom appends post|delete <id> ...
```

### No-kill run against psDoom (dry run, your processes)

```sh
cd ~/Source/psDoom-plugin-harness
export PATH="$PWD/summoner:$PATH"        # or: ln -s "$PWD/summoner/summon" ~/bin/
summon use ps                       # remembered in ~/.config/summoner/current
summon plugins                      # * ps   built-in
summon get | head                   # what psDoom will see (the first call may be empty)
export PSDOOM_POLL_CMD="summon get"
export PSDOOM_RESPOND_CMD="summon"
build/psDoom.app/Contents/MacOS/psDoom  # run from this shell so it inherits PATH and env
```

Every shot is then audited in `~/.config/summoner/state/ps/audit.jsonl` as `dry-run`, and
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
4. **Be quick in `list`** if you can, but correctness first: Summoner caches.
5. **Declare only the verbs you implement.** A read-only plugin lists `["list"]`.

## The examples

- **Processes** (`plugins/processes.py`, shipped, built in as `ps`). Key `pid@start`.
  Dimensions: `relevance` (default), `mem`, `cpu`, `age`; groups `app`, `parent`.
  `relevance` is psDoom's native score: base 100, +50 with a controlling terminal, −80 for a
  background-noise name (psDoom's list), +1 per 32 MiB up to +120, the newer pid winning a
  tie. After sorting, the 2nd and 3rd process of one name move below every first-of-its-name,
  further ones are dropped, and noise names move to the end. It grades by place on the
  primary sort. `nudge` = raise nice by `amount` (cumulative, capped at 20), `remove` =
  SIGTERM. Not undoable: an unprivileged user cannot lower nice again, and a stopped process
  cannot be restarted. Exclusions: [Trust](#trust).
- **Spreadsheet rows** (`plugins/csv_rows.py`, shipped). Every other column is a dimension:
  numeric columns sort, text columns group (ids are the column names folded to
  `[a-z0-9_]`). Key = a key column; with a priority
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

- The serve-time drop of the caller's own chain happens after `limit`, grading and grouping,
  so a listing made for another caller can reach the game one item short, with a gap in its
  grades.
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

**Done** (now in [script-backend.md](script-backend.md)): the 0..1 `grade` field, and the
neutral response verbs `post` / `delete`, which replaced `wound` / `kill`; the host's sort
request with `# dim` declarations and `group=`; and `mode=` on every event, with the plugin
trusted to curate. There is one contract;
the repo's scripts were ported to it.

**Proposals only.** The script contract is a reserved surface; none of the following is
implemented, and each needs the project owner's go before any change to
`docs/script-backend.md` or to how psDoom or psQuake call scripts.

1. **String ids / `key=`.** Let a poll line carry the plugin's string key (or replace the
   integer id), echoed back on respond, so Summoner would not need its id map to act.
2. **`kind=` poll field.** A coarse category (`process`, `row`, `agent`) a game could theme by.
3. **A result channel.** Have the game read a one-line status from the responder (or a
   status file), so the HUD can show "dry run", "blocked" or "rate limited" instead of a
   monster that silently comes back.
4. **Done: the plugin's order.** The game now keeps the plugin's order directly
   instead of only `flags` bit 0.
5. **Declared capabilities.** A `# capabilities: post delete undo` first line, so the game can
   grey out actions a backend does not support, and offer an in-game "resurrect" (undo).
6. **`dry=1` respond extra** from a game-side no-kill toggle, so the safe mode can be set in
   the menu rather than in each config.
7. **Longer labels / UTF-8**, if the renderer ever draws them; Summoner would then raise
   its 31-byte cut.
