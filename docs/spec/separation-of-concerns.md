# Spec: separation of concerns

The single source of truth for which layer owns what, in psDoom, psQuake and any later
ps-app. Every change starts here (see the rule at the end). The other specs describe each
layer's interface; where they touch ownership, they point here.

## The game is only a host

- It MUST request `get` with `sort` (from its Sort by setting), `group`, and `limit` set to
  its monster count.
- It MUST show what comes back as monsters, in that order, mapping each `grade` onto its own
  monster tiers.
- It MUST report which entity got a wound or a kill, with its mode: No effect, Wounds only,
  or Wounds and kills.
- When an entry disappears from the list, its monster MUST teleport out. That one path covers
  a process that exited, a lowered count and a changed sort.
- It MUST NOT list, rank, filter, exclude or touch a process, and MUST NOT fall back to doing
  so. If Summoner cannot run, there are no monsters ([process access](process-access.md)).
- Player-facing text MUST use player words, never Summoner's name or the protocol's words.

## Summoner's core is generic plumbing

It MUST NOT know anything about processes or games.

- It loads the plugin, validates the request against the plugin's declared dimensions, and
  passes it through unchanged.
- It MUST keep the plugin's order exactly.
- It decides from the mode whether an event runs the plugin's wound action, its kill action,
  or nothing. The user's optional config MAY only restrict that further.
- It owns the ids, the request-keyed cache and the audit log.
- It owns the inventory of killed ids: every `delete` the game sends for a listed item is
  a kill in the game, whatever the mode lets the plugin do. That id is never sent again,
  and the plugin is asked for one more item per remembered kill, so the top N still fills.

## The plugin (for example `ps`) is trusted and owns its domain

- It lists the items and declares the dimensions, plus plain words for what a wound and a
  kill do.
- It sorts, trims to the limit and grades.
- It curates, for example by demoting duplicates and noise.
- It MUST hold every safety exclusion: session-critical entities, Summoner's own chain and
  the host, with a recheck at action time.
- It performs the actual effect.

## Where tests go

- Anything about what `get` returns MUST be tested in Summoner's own suite
  (`summoner/tests/`), against the plugin.
- A game test checks only the request it sends and how it reacts to the list.

## Worked example: the monster count

The game sends its monster count as `limit`. Summoner and the plugin return the top N,
tested in Summoner as `get limit=10`, then `get limit=3`. The game's existing retire-missing
path prunes the rest. Earlier wrong turns, not to repeat: sorting in Summoner's core, and
pruning in the game.

## Rule

Before any change, name the owning layer from this file. Fix and test it there. Never patch
around it in another layer. If the owning layer is unclear, ask before building.
