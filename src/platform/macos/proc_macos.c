/*
 * psDoom -- native macOS process backend (implementation).
 *
 * Enumeration uses sysctl(KERN_PROC_ALL), which yields pid, owning uid, short
 * name and controlling-tty in a single call -- everything the original psDoom
 * scraped from `ps`. Renice/kill use the POSIX syscalls directly (the original
 * shelled out to `renice`/`kill`).
 */

#include "proc_macos.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int proc_macos_list(psd_proc_t *out, int max)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t len = 0;
    struct kinfo_proc *procs;
    int total;
    int count = 0;
    int i;

    if (out == NULL || max <= 0)
    {
        return 0;
    }

    /* Ask how big the table is, then fetch it. */
    if (sysctl(mib, 4, NULL, &len, NULL, 0) < 0 || len == 0)
    {
        return 0;
    }

    procs = (struct kinfo_proc *) malloc(len);
    if (procs == NULL)
    {
        return 0;
    }

    if (sysctl(mib, 4, procs, &len, NULL, 0) < 0)
    {
        free(procs);
        return 0;
    }

    total = (int) (len / sizeof(struct kinfo_proc));
    for (i = 0; i < total && count < max; i++)
    {
        struct kinfo_proc *kp = &procs[i];
        int pid = kp->kp_proc.p_pid;

        if (pid <= 0)
        {
            continue;
        }

        out[count].pid       = pid;
        out[count].ppid      = kp->kp_eproc.e_ppid;
        out[count].uid       = (unsigned int) kp->kp_eproc.e_ucred.cr_uid;
        out[count].is_daemon = (kp->kp_eproc.e_tdev == NODEV) ? 1 : 0;
        strncpy(out[count].name, kp->kp_proc.p_comm, PSD_PROC_NAME_MAX - 1);
        out[count].name[PSD_PROC_NAME_MAX - 1] = '\0';
        count++;
    }

    free(procs);
    return count;
}

unsigned int proc_macos_current_uid(void)
{
    return (unsigned int) getuid();
}

void proc_macos_renice(int pid, int nice_delta)
{
    int cur;
    int target;

    if (pid <= 0)
    {
        return;
    }

    /* getpriority can legitimately return -1, so disambiguate via errno. */
    errno = 0;
    cur = getpriority(PRIO_PROCESS, pid);
    if (cur == -1 && errno != 0)
    {
        cur = 0;
    }

    target = cur + nice_delta;
    if (target > PRIO_MAX)              /* PRIO_MAX == 20 */
    {
        target = PRIO_MAX;
    }

    setpriority(PRIO_PROCESS, pid, target);
}

int proc_macos_kill(int pid)
{
    if (pid <= 1)                       /* never signal kernel/launchd */
    {
        return 0;
    }

    return (kill(pid, SIGTERM) == 0) ? 1 : 0;
}
