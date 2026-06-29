/*
 * psDoom -- process backend interface.
 *
 * The OS layer is a four-operation contract: enumerate processes, report the
 * current uid, renice a pid, kill a pid. Exposing it as a vtable lets the rest
 * of psDoom (the triage layer and the game) stay backend-agnostic, and lets a
 * test swap in a deterministic fake backend -- or a future Linux backend drop
 * in -- with no changes above this interface.
 *
 *     proc_backend (this)  <-  proc_macos / proc_fake / proc_linux ...
 *            ^
 *            |  proc_backend()->list(...) etc.
 *     proc_select, psdoom
 */

#ifndef PSDOOM_PROC_BACKEND_H
#define PSDOOM_PROC_BACKEND_H

#include "proc_types.h"   /* psd_proc_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /* Fill `out` (capacity `max`) with the live processes; returns the count. */
    int          (*list)(psd_proc_t *out, int max);

    /* The current (real) user id. */
    unsigned int (*current_uid)(void);

    /* Lower `pid`'s scheduling priority by `nice_delta`. */
    void         (*renice)(int pid, int nice_delta);

    /* Ask `pid` to terminate. Returns 1 on success, 0 on failure. */
    int          (*kill)(int pid);
} proc_backend_t;

/* The backend in effect. Never NULL on a supported platform. */
const proc_backend_t *proc_backend(void);

/* Override the active backend (tests / a future replay backend). Passing NULL
 * restores the platform default. */
void proc_backend_set(const proc_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_BACKEND_H */
