/*
 * proc_macos.c -- native macOS process backend for psDoom.
 *
 * Uses libproc to enumerate processes and read per-process info, setpriority(2)
 * to renice, and kill(2) to terminate. Replaces the original psDoom's shelling
 * out to `ps` / `renice` / `kill`.
 */

#include "proc_macos.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

#include <libproc.h>
#include <sys/proc_info.h>

/* How much a single wound lowers a process's scheduling priority. */
#define PSD_RENICE_STEP 5

int psd_proc_list(psd_proc_t *out, int max, int current_uid_only)
{
    if (out == NULL || max <= 0)
    {
        return 0;
    }

    const pid_t self = getpid();
    const uid_t me = getuid();

    /* First call sizes the buffer, second call fills it. */
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0)
    {
        return 0;
    }

    int cap = bytes / (int)sizeof(pid_t);
    pid_t *pids = (pid_t *)calloc((size_t)cap, sizeof(pid_t));
    if (pids == NULL)
    {
        return 0;
    }

    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)(cap * (int)sizeof(pid_t)));
    int npids = bytes / (int)sizeof(pid_t);

    int count = 0;
    for (int i = 0; i < npids && count < max; i++)
    {
        pid_t pid = pids[i];
        if (pid <= 0 || pid == self)
        {
            continue;
        }

        struct proc_bsdinfo bi;
        int r = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, PROC_PIDTBSDINFO_SIZE);
        if (r != (int)PROC_PIDTBSDINFO_SIZE)
        {
            continue;   /* process exited, or we lack permission to inspect it */
        }

        if (current_uid_only && bi.pbi_ruid != me)
        {
            continue;
        }

        out[count].pid = pid;
        out[count].uid = bi.pbi_ruid;
        /* No controlling terminal (e_tdev == NODEV) => daemon. */
        out[count].is_daemon = (bi.e_tdev == (uint32_t)-1) ? 1 : 0;

        const char *nm = (bi.pbi_name[0] != '\0') ? bi.pbi_name : bi.pbi_comm;
        strncpy(out[count].name, nm, sizeof(out[count].name) - 1);
        out[count].name[sizeof(out[count].name) - 1] = '\0';

        count++;
    }

    free(pids);
    return count;
}

void psd_proc_renice(pid_t pid)
{
    errno = 0;
    int cur = getpriority(PRIO_PROCESS, pid);
    if (cur == -1 && errno != 0)
    {
        return;   /* couldn't read current priority (gone / no permission) */
    }

    int next = cur + PSD_RENICE_STEP;
    if (next > PRIO_MAX)
    {
        next = PRIO_MAX;
    }
    setpriority(PRIO_PROCESS, pid, next);
}

void psd_proc_kill(pid_t pid)
{
    kill(pid, SIGKILL);
}
