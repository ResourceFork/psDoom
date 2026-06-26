/*
 * psDoom -- process-management layer (game-side API).
 *
 * Bridges the Doom engine to OS process management: each process is represented
 * by a monster; wounding a monster renices its process, killing it signals it.
 *
 * This header is intentionally independent of the engine's headers (it only
 * forward-declares the monster type), so engine call-sites include it cheaply.
 */

#ifndef PSDOOM_H
#define PSDOOM_H

/*
 * The engine defines `typedef struct mobj_s mobj_t;`. We take `struct mobj_s *`
 * so this header needs none of the engine's headers.
 */
struct mobj_s;

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup at startup: open the process backend, load the allow/deny
 * filter, default to the current UID's processes. */
void psdoom_init(void);

/* Periodic reconcile of the live process list with in-game monsters. Called
 * from the game tick; must be cheap and self-throttling. */
void psdoom_sync(void);

/* A process-monster took non-fatal damage -> lower its process priority
 * (renice). No-op if `target` is not a process-monster. */
void psdoom_wound(struct mobj_s *target);

/* A process-monster died -> signal its process (kill). No-op if `target` is
 * not a process-monster. */
void psdoom_kill(struct mobj_s *target);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_H */
