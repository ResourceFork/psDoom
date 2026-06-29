# examples/ — wiring psDoom to other things

The [script backend](../docs/script-backend.md) lets psDoom represent anything you can
enumerate and act on from a script, not just OS processes. These are illustrative wirings; the
canonical process pair lives in [`../scripts/`](../scripts/).

| Example | What becomes a monster | Dependencies |
|---|---|---|
| [`demo/`](demo/) | synthetic entities (varying load + a periodic fork burst) | none — pure `sh`/`awk` |
| [`docker/`](docker/) | running Docker containers (keyed by main PID) | `docker` |

Each directory has a `poll-*.sh` (enumerate → stdout) and a `respond-*.sh` (wound/kill). Wire a
pair via the env vars, then launch psDoom:

```sh
# Safe, dependency-free — great for a first look or screenshots:
export PSDOOM_POLL_CMD="$PWD/examples/demo/poll-demo.sh"
export PSDOOM_RESPOND_CMD="$PWD/examples/demo/respond-demo.sh"
open build/psDoom.app
```

```sh
# Containers as monsters (wound = throttle CPU shares, kill = docker kill):
export PSDOOM_POLL_CMD="$PWD/examples/docker/poll-containers.sh"
export PSDOOM_RESPOND_CMD="$PWD/examples/docker/respond-containers.sh"
```

## Notes

- The `demo` responder has **no real side effects** — it just logs each action to
  `$TMPDIR/psdoom-demo-actions.log` so you can watch the response contract fire.
- A poller that prints nothing (e.g. `docker` not installed) is fine: psDoom simply shows no
  monsters that cycle.
- Reminder: in script mode the script owns safety — the native protected-process filter does not
  apply to arbitrary entities. See [Security model](../docs/script-backend.md#security-model).
- Writing your own? Copy a pair, keep the [contract](../docs/script-backend.md): poll lines are
  TAB-separated `key=value` (`id` required, `> 1`); responses are `<verb> <id> [key=value…]`.
