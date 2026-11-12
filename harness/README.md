# harness/ — psDoom plugin harness

A v1 poll/respond pair ([`../docs/script-backend.md`](../docs/script-backend.md)) that hands
each call to a small *plugin* with four subcommands (`describe`, `list`, `nudge`, `remove`,
plus optional `restore` for undo). It adds stable ids, ladder scaling, 31-byte labels,
background caching, and a safety layer: dry run by default, remove off by default, a rate
limit, an audit log and undo. Design and plugin contract:
[`../docs/plugin-harness.md`](../docs/plugin-harness.md).

| Path | What |
| --- | --- |
| [`psdoom_harness.py`](psdoom_harness.py) | the harness (Python 3.10+, standard library only) |
| [`plugins/processes.py`](plugins/processes.py) | your own processes, like the native backend |
| [`plugins/csv_rows.py`](plugins/csv_rows.py) | rows of a CSV file, with undo |
| [`examples/`](examples/) | config files; replace the `state_dir` placeholder first |
| [`tests/`](tests/) | `python3 -I harness/tests/run_tests.py` (no build needed); psDoom's `ctest` also runs them as `psdoom_harness_tests` |

This folder is self-contained: nothing in it imports or reads files outside `harness/`,
and its tests need only Python, so it can move to its own repository unchanged. The two links
above point into the psDoom repository that hosts it today.

## One contract for the apps

The harness's poll output and responder calls are exactly the v1 script-backend contract. Any
ps-app (psDoom, psQuake, later ones) that speaks v1 works with every plugin behind the
harness, and plugins never see the app. Plugins are tested against the harness alone
(`tests/`); each app proves its side with its own conformance test (psDoom:
`tests/harness_e2e_test.c`).

## Quick start (no-kill: dry run)

```sh
mkdir -p "$HOME/psdoom-harness"
sed "s|REPLACE-WITH-AN-ABSOLUTE-PATH|$HOME/psdoom-harness/processes|" \
    harness/examples/processes.json > "$HOME/psdoom-harness/processes.json"
CFG="$HOME/psdoom-harness/processes.json"
python3 harness/psdoom_harness.py --config "$CFG" refresh
python3 harness/psdoom_harness.py --config "$CFG" poll | head
export PSDOOM_POLL_CMD="python3 $PWD/harness/psdoom_harness.py --config $CFG poll"
export PSDOOM_RESPOND_CMD="python3 $PWD/harness/psdoom_harness.py --config $CFG respond"
```

Every shot is then written to `$HOME/psdoom-harness/processes/audit.jsonl` as `dry-run`, and
nothing is signalled. Paths must not contain spaces (v1 splits the respond command on
whitespace).
