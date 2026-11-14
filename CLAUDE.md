# CLAUDE.md

Follow [AGENTS.md](AGENTS.md): the gate, the safety rules for any run, and where the specs
live ([docs/spec/](docs/spec/README.md)).

Key rule: psDoom never touches a process itself. Summoner, bundled in the app, is the only
path to real processes. Tests never act on a real process, and every Summoner run uses a
scratch `--root` and `HOME`.
