/*
 * proc_macos.h -- native macOS process backend for psDoom.
 *
 * The original psDoom shelled out to `ps`, `kill -9`, and `renice`. This is the
 * native equivalent: enumerate via libproc, signal via kill(2), renice via
 * setpriority(2). No engine headers here -- pure OS layer.
 */

#ifndef PSDOOM_PROC_MACOS_H
#define PSDOOM_PROC_MACOS_H

#include <sys/types.h>

/* One enumerated process. */
typedef struct
{
    pid_t pid;
    uid_t uid;          /* real uid of the process owner */
    int   is_daemon;    /* 1 if the process has no controlling terminal */
    char  name[32];     /* process (accounting) name, NUL-terminated */
} psd_proc_t;

/*
 * Fill `out` (capacity `max`) with the live processes; returns the count.
 * If `current_uid_only` is nonzero, only processes whose real uid matches the
 * caller are returned. The calling process is always excluded (so the game
 * never spawns a monster for itself).
 */
int psd_proc_list(psd_proc_t *out, int max, int current_uid_only);

/* Lower a process's scheduling priority by a step (renice). Best-effort. */
void psd_proc_renice(pid_t pid);

/* Kill a process with SIGKILL (the original's `kill -9`). Best-effort. */
void psd_proc_kill(pid_t pid);

#endif /* PSDOOM_PROC_MACOS_H */
