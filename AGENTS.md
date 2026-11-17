# Agent guide: psDoom

## Where a change belongs

Before writing any code, in psDoom and psQuake alike, read
[docs/spec/separation-of-concerns.md](docs/spec/separation-of-concerns.md): it is the
authority on which layer owns what. Name the owning layer from it, fix and test the change
there, never patch around it in another layer, and ask when the layer is unclear.

## Read first

- [docs/spec/](docs/spec/README.md) holds the normative specs: separation of concerns,
  the script contract, Summoner, and process access.
- psDoom never touches processes itself. Summoner (`summoner/`, bundled in the app) is the
  only path to them (see [process-access](docs/spec/process-access.md)). Do not add
  `kill`, `setpriority`, `sysctl`/`proc_pid*` reads or a native backend to `src/`:
  `tests/no_native_process_access.cmake` is a tripwire for the obvious calls, not a
  proof.

## Gate

```sh
tools/fetch-freedoom.sh              # once per checkout (git-ignored wads/)
cmake -S . -B build && cmake --build build -j \
  && cmake --build build --target psdoom_tests psdoom_window_state_tests \
  && ctest --test-dir build --output-on-failure
python3 -I summoner/tests/run_tests.py         # Summoner alone, no CMake
```

## Safety rules for any run

- Never signal or renice a real process in a test. The built-in `ps` plugin acts by
  default (it follows the host mode), so it may only be used read-only (`describe`,
  `list`, `get`), with `mode=dryrun`, or with its command swapped for a fake in the test's
  own process. Real actions are tested through the CSV plugin on a CSV the test created.
- Every Summoner run sets an explicit scratch root (`--root <dir under build/>`) and a
  scratch `HOME`. Never use the real `~/.config/summoner`, and never trust an inherited
  `SUMMONER_ROOT`.
- Responders under test are stubbed copies (see `tests/reference_scripts_test.sh`).
- Do not launch `psDoom.app` on the user's screen.

## Changing the contract

The script contract (`docs/script-backend.md`) and Summoner's plugin contract are shared
with psQuake (through `psproc/` drift). Changing either needs the owner's go. Update the
spec, the doc and the tests together.
