---
inclusion: always
---

# Summoner and process access

- psDoom never lists, renices or signals a process itself. The bundled Summoner
  (`summoner/`, copied to `psDoom.app/Contents/Resources/summoner`) is the only path to
  real processes. Specs: `docs/spec/`.
- Its plugins are trusted: they sort, group, trim, grade and exclude. The game keeps their
  order, filters nothing, and sends `post` / `delete` with `mode=dryrun|post-only|delete`.
- The effective action is the stricter of the host mode and the plugin config. The built-in
  `ps` plugin follows the host mode with no config; a `ps.json` can only restrict it. The
  game's menus say Off / Wounds only / Wounds and kills, never dry run / post / delete.
- Tests: never act on a real process. Use scratch `--root` and `HOME`, never
  `~/.config/summoner`. Gate and rules: `AGENTS.md`.
