/*
 * psDoom -- entity selection layer.
 *
 * Sits between the backend and the game (psdoom):
 *
 *     Summoner (summon get)  ->  proc_script  ->  proc_select  ->  psdoom
 *     (trusted plugin:            (backend)        (cut to the       (monsters)
 *      lists, orders, excludes)                     candidate cap)
 *
 * psDoom does not read, filter or rank real processes itself any more: the
 * only path to them is Summoner, and its plugin curates
 * (docs/script-backend.md, "Trust"). This layer keeps the backend's order
 * and only cuts it to the caller's capacity. Engine-free and host-testable.
 *
 * The entity record is `psd_proc_t` (see proc_types.h).
 */

#ifndef PSDOOM_PROC_SELECT_H
#define PSDOOM_PROC_SELECT_H

#include "proc_types.h"   /* psd_proc_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Reset the selection state. Call once at startup. */
void psd_select_init(void);

/*
 * List the active backend's entities and write the first `max` of them into
 * `out`, in the backend's order. Returns the count written. Nothing is
 * filtered or re-ranked here. Returns -1, writing nothing and keeping the
 * last listing (and its child counts), while the backend has no new list
 * yet (the script backend's poll is still running): call again later.
 */
int psd_select_collect(psd_proc_t *out, int max);

/*
 * Number of entities in the last listing whose parent is `pid` (the whole
 * listing, before the cut). Lets the game spot a fork burst (a parent whose
 * child count jumps between syncs). Valid after psd_select_collect();
 * returns 0 before the first call.
 */
int psd_select_child_count(int pid);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_SELECT_H */
