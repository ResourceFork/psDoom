# Spec: Summoner (`summon`)

Full text: [summoner.md](../summoner.md). The code lives in `summoner/` (self-contained,
Python 3.9+ standard library).

## Commands

The command line is `summon [--root DIR] [--plugin NAME | --config FILE] COMMAND`. The
commands:

- `get [sort=ID[:dir],..] [group=ID] [limit=N]`
- `post <id> …` and `delete <id> …`
- `use <name>`, `plugins`, `refresh`, `describe`, `undo <action-id>`

Any other word is an audited no-op that exits 0.

Where things live:

- **Root:** `--root`, else `$SUMMONER_ROOT`, else `~/.config/summoner`. The root,
  `plugins/`, configs and state must be yours and not group- or world-writable.
- **Plugin:** `--config`, else `--plugin`, else `<root>/current`, else `ps`, which is built
  in, dry run, with remove off.
- **Ids:** one map per root, never reused, failing closed if lost.

## Plugins

- A plugin is an executable answering `describe` (verbs, dimensions, `default_sort`),
  `list [sort= group= limit=]` (JSON Lines), `nudge <key> amount=N`, `remove <key>` and
  optionally `restore`. It is run as an argv, never through a shell, with a timeout.
- **The plugin is trusted and curates.** It sorts, groups, trims, grades and excludes.
  Summoner keeps its order; `max_items` keeps the head of the list.
- Summoner validates the request against the dimensions. An unknown id is dropped with a
  `# summon:` note and replaced by the default sort; the poll never fails.

## Safety

- The effective action is the stricter of the host `mode` and the plugin's `dry_run` /
  `allow_remove`.
- Other guards: a rate limit, a label check, acting on listed ids only, and an audit line
  for every call, including skipped ones.
- The `ps` plugin leaves out:
  - psDoom's protected names and session plumbing;
  - processes whose effective or real uid is not yours;
  - itself and its chain;
  - the host chain (`SUMMONER_HOST_PIDS`).
- Items carry their `pid`, and every `get` drops its own calling chain at serve time,
  serving no pid item if that chain cannot be read.
