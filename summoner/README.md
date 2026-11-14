# summoner/ — Summoner (`summon`)

`summon` is a poll command and a responder for the ps-apps' script contract (psDoom's
`docs/script-backend.md`). It hands each call to a small *plugin* (`describe`, `list`,
`nudge`, `remove`, plus optional `restore` for undo) and adds stable ids, a 0..1 grade, ladder
scaling, 31-byte labels, background caching, and a safety layer: dry run by default, remove off
by default, a rate limit, an audit log and undo. Design and plugin contract: psDoom's
`docs/summoner.md`.

| Path | What |
| --- | --- |
| [`summon`](summon) | the command (a thin launcher; put this folder on PATH or symlink it) |
| [`summoner.py`](summoner.py) | Summoner (Python 3.10+, standard library only) |
| [`plugins/processes.py`](plugins/processes.py) | `ps`: your own processes, like the native backend |
| [`plugins/csv_rows.py`](plugins/csv_rows.py) | rows of a CSV file, with undo |
| [`examples/`](examples/) | config files to start from |
| [`tests/`](tests/) | `python3 -I summoner/tests/run_tests.py` (no build needed); psDoom's `ctest` also runs them as `psdoom_summoner_tests` |

This folder is self-contained: nothing in it imports or reads files outside `summoner/`, and its
tests need only Python, so it can move to its own repository unchanged.

## Commands

```
summon [--root DIR] [--plugin NAME | --config FILE] COMMAND ...

get [sort=ID[:dir],..] [group=ID] [limit=N]
                            the poll output, in the plugin's order for that request
post <id> [key=value ...]   nudge it (lower its priority)
delete <id> [key=value ...] remove it
                            post/delete act only as far as mode= allows (dryrun,
                            post-only, delete; missing = dryrun) and the plugin's
                            config allows (dry_run, allow_remove): the stricter wins
use <name>                  set the current plugin, remembered
plugins                     list known plugins; * marks the current one
refresh | describe | undo <action-id>
```

- **Root**: `--root DIR`, else `$SUMMONER_ROOT`, else `~/.config/summoner`. It holds
  `current`, `plugins/<name>.json`, `ids.json` (one id map for all plugins) and
  `state/<name>/` (cache, `audit.jsonl`).
- **Plugin**: `--config FILE`, else `--plugin NAME`, else the current one, else `ps`. `ps` is
  built in: dry run, remove off. Any other name needs `<root>/plugins/<name>.json`, with no
  `state_dir` in it (named plugins always keep state under `<root>/state/`).
- Any other word exits 0 and does nothing (it is audited as `ignored-verb`).
- **The plugin curates and is trusted.** Summoner keeps its order and the game filters
  nothing, so the plugin leaves out what must never be touched. `ps` drops psDoom's
  protected names, itself, and the host chain (`summon get`, the game and its launchers).

## Put it on PATH

```sh
export PATH="$PWD/summoner:$PATH"                 # this shell only
# or, once:  mkdir -p ~/bin && ln -s "$PWD/summoner/summon" ~/bin/summon
```

The symlink is followed back here, so the plugins are found next to the real file. Nothing is
installed for you.

## No-kill run against psDoom (dry run, your processes)

```sh
cd ~/Source/psDoom-plugin-harness
export PATH="$PWD/summoner:$PATH"
summon use ps
summon get | head            # the first call may be empty while the cache fills
export PSDOOM_POLL_CMD="summon get"
export PSDOOM_RESPOND_CMD="summon"
build/psDoom.app/Contents/MacOS/psDoom
```

Every shot is then written to `~/.config/summoner/state/ps/audit.jsonl` as `dry-run`, and
nothing is signalled. Start psDoom from that shell: Finder and `open` give it a minimal PATH.
Paths, a `--root` included, must not contain spaces (the respond command is split on whitespace).

## The script contract in brief (what the apps read)

The full contract is psDoom's `docs/script-backend.md`; this is the part Summoner speaks:

- **Poll** (`PSDOOM_POLL_CMD`, run via `sh`, killed with its process group after 1 s by
  default): stdout, one entity per line of TAB-separated `key=value` fields: `id` (integer
  2..2147483647, stable, never reused), `parent` (an id or 0), `weight` (bytes; ladder 0, 16M,
  64M, 192M, 512M, 1.5G, 4G), `load` (percent; ladder 0, 5, 25, 60, 120, 250, 500), `flags`
  (bit 0 = background), `label` (at most 31 bytes drawn, no TAB or newline), `grade` (0..1,
  clamped), which picks the monster over the tiers the game can draw
  (`min(floor(grade × N), N − 1)`), `group`. Lines starting with `#` are comments; a
  leading `# dim` block declares the sort and group dimensions, and then the game appends
  `limit=64` and its chosen `sort=<id>` to the poll command. The game keeps the order and
  filters nothing.
- **Respond** (`PSDOOM_RESPOND_CMD`, split on whitespace, exec'd without a shell, detached,
  exit code ignored): `<cmd> <verb> <id> [key=value...]`: `post` (with `amount=N`) for a
  wound, `delete` for a kill; optional `label=`; always `mode=dryrun|post-only|delete`,
  which the responder must honour. Unknown verbs and keys must be no-ops.

## One contract for the apps

Summoner's poll output and responder calls are exactly that script contract. Any ps-app
(psDoom, psQuake, later ones) that speaks it works with every plugin behind Summoner, and
plugins never see the app. Plugins are tested against Summoner alone (`tests/`); each app
proves its side with its own conformance test (psDoom: `tests/summoner_e2e_test.c`).
