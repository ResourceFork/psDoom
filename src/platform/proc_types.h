/*
 * psDoom -- backend-neutral process record.
 *
 * The process snapshot type shared by every process backend (macOS, a future
 * Linux backend, the test fake) and by the engine-free triage layer. Kept in
 * its own header -- with no OS or engine dependencies -- so nothing above the
 * backend needs to include a platform-specific header just for the type.
 */

#ifndef PSDOOM_PROC_TYPES_H
#define PSDOOM_PROC_TYPES_H

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

#endif /* PSDOOM_PROC_TYPES_H */
