/*
 * psDoom -- process selection / triage layer.
 *
 * Sits between the raw OS backend (proc_macos) and the game (psdoom):
 *
 *     proc_macos   ->   proc_select   ->   psdoom
 *     (read OS)         (filter/rank/        (turn the collection
 *                        truncate)            into monsters)
 *
 * It acquires the live process list and reduces it to the set of process
 * objects most worth showing the player as monsters. It has no engine
 * dependencies and holds no game state, so the triage policy (what to keep,
 * how to rank, how many to show) can be reasoned about -- and tested -- in
 * isolation from both the OS and the renderer.
 *
 * The process object is `psd_proc_t` (see proc_macos.h).
 */

#ifndef PSDOOM_PROC_SELECT_H
#define PSDOOM_PROC_SELECT_H

#include "proc_macos.h"   /* psd_proc_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Capture the identity used for filtering: the current uid and the game's own
 * pid (also the root of the launcher chain). Call once at startup before
 * psd_select_collect. */
void psd_select_init(void);

/*
 * Acquire the live process list and reduce it to the processes the player
 * should see, writing up to `max` of them into `out` in descending order of
 * relevance. Returns the count written.
 *
 * Excluded entirely: other users' processes, pid <= 1, the game itself,
 * session-critical processes (loginwindow / Dock / Finder / ...) and the
 * game's launcher chain (shell / terminal, discovered via ppid). What remains
 * is ranked so the most relevant processes survive the truncation to `max`.
 */
int psd_select_collect(psd_proc_t *out, int max);

/*
 * Pure triage core: filter, rank and truncate an already-acquired snapshot.
 * Exposed separately from the OS read (and free of any global/engine state) so
 * the policy can be exercised directly in isolation.
 *
 *   raw / n_raw : the input snapshot
 *   uid         : keep only processes owned by this uid (unless all_users)
 *   self_pid    : the game's pid -- excluded, and the root of the launcher
 *                 chain that is walked (via ppid) and excluded
 *   all_users   : if nonzero, do not filter by uid (show every user's procs)
 *   out / max   : destination (descending relevance) and its capacity
 *
 * Returns the count written to `out`.
 */
int psd_select_triage(const psd_proc_t *raw, int n_raw,
                      unsigned int uid, int self_pid, int all_users,
                      psd_proc_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_SELECT_H */
