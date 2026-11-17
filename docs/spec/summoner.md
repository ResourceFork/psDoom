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
  in and follows the host `mode` with no config file. A `<root>/plugins/ps.json` is laid
  over it and can only restrict it: `dry_run` only on, `allow_remove` only off, the rate
  limit only tighter; it cannot change the command, name or state directory. A plugin that
  is not built in is dry run, with remove off, unless its config says otherwise.
- **Ids:** one map per root, never reused, failing closed if lost.

## Plugins

- A plugin is an executable answering `describe` (verbs, dimensions, `default_sort`, and
  optionally `nudge.effect` / `remove.effect`: plain words for what a wound and a kill do,
  which `get` passes on as `# effect` header lines),
  `list [sort= group= limit=]` (JSON Lines), `nudge <key> amount=N`, `remove <key>` and
  optionally `restore`. It is run as an argv, never through a shell, with a timeout.
- An item may carry `children`, its direct child count over everything the plugin saw; `get`
  passes it on as `children=N`. The built-in `ps` sends it.
- What the plugin owns, and what Summoner's core must not do, is in
  [separation of concerns](separation-of-concerns.md). `max_items` keeps the head of the
  plugin's list.
- `get` lists at most `limit` items, the head of the plugin's order. The cache is keyed by
  the request: a changed request refreshes, but a lower `limit` with the same sort and group
  is answered at once by the head of the entry made for more, with that entry's grades and
  notes until the refresh lands. The caller's own chain is dropped before the cut.
- Summoner validates the request against the dimensions. An unknown id is dropped with a
  `# summon:` note and replaced by the default sort; the poll never fails.

## Safety

- The effective action is the stricter of the host `mode` and the plugin's `dry_run` /
  `allow_remove`. For the built-in `ps` with no config, that is the host `mode` alone.
- Other guards: a rate limit, a label check, acting on listed ids only, and an audit line
  for every call, including skipped ones.
- The `ps` plugin acts by default, so its exclusions are what keep the game off what must
  never be touched. It leaves out:
  - psDoom's protected names and session plumbing;
  - processes whose effective or real uid is not yours;
  - itself and its chain;
  - the host chain (`SUMMONER_HOST_PIDS`).
- Items carry their `pid`, and every `get` drops its own calling chain at serve time,
  serving no pid item if that chain cannot be read.
- At action time the `ps` plugin rechecks the pid (same process, and still passing the
  listing's exclusions: protected names, which include the games, effective and real uid,
  and its own calling chain) before it renices or signals it. The game's launcher chain
  is dropped at serve time; a respond does not carry it, so at action time the games are
  protected by name.
