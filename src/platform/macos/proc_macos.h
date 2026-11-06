/*
 * psDoom -- native macOS process backend.
 *
 * Pure OS layer (no engine dependencies): enumerate processes, renice, kill.
 * The game-side logic in src/psdoom/psdoom.c calls into here.
 */

#ifndef PSDOOM_PROC_MACOS_H
#define PSDOOM_PROC_MACOS_H

#include "proc_types.h"   /* psd_proc_t, PSD_PROC_NAME_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `out` (capacity `max`) with the live processes; returns the count. */
int          proc_macos_list(psd_proc_t *out, int max);

/* The current (real) user id. */
unsigned int proc_macos_current_uid(void);

/* Lower `pid`'s scheduling priority by `nice_delta` (clamped to +20). */
void         proc_macos_renice(int pid, int nice_delta);

/* Ask `pid` to terminate (SIGTERM). Returns 1 on success, 0 on failure. */
int          proc_macos_kill(int pid);

/* Re-read one pid now (sysctl KERN_PROC_PID): fills identity and ownership
 * (pid, start_token, ppid, uid, is_daemon, name; metrics 0) and returns 1, or
 * returns 0 if no process has that pid. Agrees with proc_macos_list (an
 * unreaped zombie is reported, as list() reports it). */
int          proc_macos_lookup(int pid, psd_proc_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_MACOS_H */
