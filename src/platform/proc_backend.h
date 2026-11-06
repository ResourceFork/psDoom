/*
 * psDoom -- process backend interface.
 *
 * The OS layer is a five-operation contract: enumerate processes, report the
 * current uid, renice a pid, kill a pid, and re-read one pid. Exposing it as a
 * vtable lets the rest of psDoom (the triage layer and the game) stay
 * backend-agnostic, and lets a test swap in a deterministic fake backend -- or
 * a future Linux backend drop in -- with no changes above this interface.
 *
 *     proc_backend (this)  <-  proc_macos / proc_fake / proc_linux ...
 *            ^
 *            |  proc_backend()->list(...), psd_proc_*_checked(...)
 *     proc_select, psdoom
 *
 * The game never calls the raw renice/kill ops directly: it goes through
 * psd_proc_renice_checked / psd_proc_kill_checked, which re-read the pid and
 * act only if it still belongs to the same process (pid + start token + name)
 * that the monster was spawned for.
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

    /* Lower `pid`'s scheduling priority by `nice_delta`. Raw: no identity
     * check. Call psd_proc_renice_checked instead. */
    void         (*renice)(int pid, int nice_delta);

    /* Ask `pid` to terminate. Returns 1 on success, 0 on failure. Raw: no
     * identity check. Call psd_proc_kill_checked instead. */
    int          (*kill)(int pid);

    /* Re-read `pid` as it is now. Fills at least pid, start_token and name of
     * `out` and returns 1 if a live process (or entity) has that pid, 0 if
     * none does. */
    int          (*lookup)(int pid, psd_proc_t *out);
} proc_backend_t;

/* The backend in effect. Never NULL on a supported platform. */
const proc_backend_t *proc_backend(void);

/* Override the active backend (tests / a future replay backend). Passing NULL
 * restores the platform default. */
void proc_backend_set(const proc_backend_t *backend);

/* Outcome of a checked action. Only PSD_ACT_DONE and PSD_ACT_FAILED made the
 * backend call; every other result made none. */
typedef enum
{
    PSD_ACT_DONE = 0,   /* identity verified; the action was made            */
    PSD_ACT_FAILED,     /* identity verified; the backend reported a failure */
    PSD_ACT_GONE,       /* no process has that pid any more                  */
    PSD_ACT_REUSED,     /* the pid now belongs to a different process        */
    PSD_ACT_REFUSED,    /* invalid request (pid <= 1, no backend, no lookup) */
} psd_act_result_t;

/* True if `cur` is the process identified by (pid, start_token, name). The
 * name is compared over its first PSD_PROC_IDENT_NAME_LEN bytes, and only
 * when start_token != 0: an entity without a start token (script backend) is
 * identified by its id alone, as docs/script-backend.md defines. */
int psd_proc_same(const psd_proc_t *cur, int pid,
                  unsigned long long start_token, const char *name);

/* Re-read `pid` and, only if it is still the process identified by
 * (start_token, name), renice it by `nice_delta` / send it SIGTERM. */
psd_act_result_t psd_proc_renice_checked(int pid, unsigned long long start_token,
                                         const char *name, int nice_delta);
psd_act_result_t psd_proc_kill_checked(int pid, unsigned long long start_token,
                                       const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_BACKEND_H */
