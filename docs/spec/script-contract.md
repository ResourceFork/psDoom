# Spec: the script contract

Full text: [script-backend.md](../script-backend.md).

## Poll (`get`)

- stdout, one entity per line, TAB-separated `key=value`; `#` lines are comments.
- `id` is required: plain decimal 2..2147483647, stable, and never names two entities.
- The optional fields are `parent`, `label` (31 bytes drawn), `weight`, `load`, `flags`
  (bit 0 = background), `group`, and `grade`. `grade` is a finite decimal, clamped to 0..1;
  anything else is ignored.
- `grade` picks the monster: tier `min(floor(grade × N), N − 1)` over the N tiers the WAD
  draws. Without `grade`, *Monster size by* (weight or load) decides.
- A leading block of `# dim id= kind= dir= roles= unit= label=` lines declares the
  dimensions, and `# sort id= dir=` reports the sort applied.
- `# effect verb=post|delete text=<words>` lines in that block say, in plain words of at
  most 40 bytes, what a wound (`post`) and a kill (`delete`) do (ps: "lowers its
  priority", "quits the process"). The host shows them on the effect setting's help line,
  as printable ASCII, and never acts on them.
- Once dimensions are declared, the host appends `limit=<n>` and, if the player chose one,
  `sort=<id>` to the poll command. Only `[a-z][a-z0-9_]{0,15}` ids and digits are appended.
- The host **keeps the order and filters nothing**: the script is trusted.
- The host polls about once a second, one poll at a time, off its game thread: a frame never
  waits for a poll. Until a poll ends, the last result stands; the request words are those
  in effect when the poll starts.

## Respond (`post` / `delete`)

- psDoom execs the responder, never through a shell, detached, and ignores its exit code.
- argv: `<cmd> post|delete <id> [amount=<n>] [label=<text>] mode=<dryrun|post-only|delete>`.
- The mode follows psDoom's "Fighting monsters effects" setting: No effect → `dryrun`, Wounds only → `post-only`,
  Wounds and kills → `delete`. It is sent with every event. The protocol's words
  (`dryrun`, `post`, `delete`) and the name Summoner never appear in player-facing
  text (menus, help, warnings, prompts, on-screen messages); they stay in the protocol,
  the CLI, the specs and the logs.
- A responder **must honour `mode`**: missing or unknown means `dryrun`, and `post-only`
  never deletes. Unknown verbs and keys are no-ops that exit 0.
- There is no version header, and the contract grows only by adding keys and verbs.
