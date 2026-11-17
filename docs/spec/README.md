# psDoom specifications

Normative summaries. The full text is in the linked docs; where they differ, the code and its
tests decide, and the doc is the bug.

| Spec | What it fixes | Full text | Enforced by |
| --- | --- | --- | --- |
| [Separation of concerns](separation-of-concerns.md) | Who owns what: the game hosts, Summoner's core is plumbing, the plugin curates and guards. Read it before any change | this spec | `summoner/tests/` (what `get` returns), `tests/no_native_process_access.cmake`, `tests/no_effect_jargon.cmake` |
| [Script contract](script-contract.md) | What a ps-app reads from a poll and sends to a responder | [script-backend.md](../script-backend.md) | `tests/script_grade_test.c`, `tests/script_request_test.c`, `tests/proc_select_test.c` |
| [Summoner](summoner.md) | The bundled command, its plugins, trust and safety | [summoner.md](../summoner.md) | `summoner/tests/`, `tests/summoner_e2e_test.c`, `tests/summoner_bundle_test.c` |
| [Process access](process-access.md) | psDoom never touches a process itself | this spec | `tests/no_native_process_access.cmake`, `tests/app_bundle_test.sh` |
