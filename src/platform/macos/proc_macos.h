/*
 * psDoom -- native macOS process backend.
 *
 * Pure OS layer (no engine dependencies): enumerate processes, renice, kill.
 * The game-side logic in src/psdoom/psdoom.c calls into here.
 */

#ifndef PSDOOM_PROC_MACOS_H
#define PSDOOM_PROC_MACOS_H

#define PSD_PROC_NAME_MAX 32

typedef struct
{
    int                pid;
    int                ppid;               /* parent pid (for safety filter)  */
    unsigned int       uid;
    int                is_daemon;          /* 1 if it has no controlling tty  */
    unsigned long long footprint;          /* resident memory, bytes (0 if n/a) */
    int                cpu_percent;        /* recent CPU load; 100 = one core  */
    char               name[PSD_PROC_NAME_MAX]; /* short process name          */
} psd_proc_t;

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

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_MACOS_H */
