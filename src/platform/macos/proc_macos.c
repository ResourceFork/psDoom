/*
 * psDoom -- native macOS process backend (implementation).
 *
 * Enumeration uses sysctl(KERN_PROC_ALL), which yields pid, owning uid, short
 * name and controlling-tty in a single call -- everything the original psDoom
 * scraped from `ps`. Renice/kill use the POSIX syscalls directly (the original
 * shelled out to `renice`/`kill`).
 */

#include "proc_macos.h"
#include "proc_backend.h"   /* proc_backend_t (vtable registration) */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <libproc.h>      /* proc_pid_rusage (per-process memory + CPU time) */
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>         /* clock_gettime (monotonic clock for CPU load)    */
#include <mach/mach_time.h> /* mach_timebase_info (rusage CPU-time units)     */

/* proc_pid_rusage reports ri_user_time/ri_system_time in mach absolute time
 * units, not nanoseconds. On Apple Silicon one unit is ~41.67 ns (timebase
 * 125/3); on Intel it is 1 ns (timebase 1/1). Our wall clock uses
 * clock_gettime (true nanoseconds), so we must convert the CPU time to ns
 * before comparing -- otherwise a busy process reads ~40x too low. */
static unsigned long long proc_macos_mach_to_ns(unsigned long long mach_units)
{
    static mach_timebase_info_data_t tb;

    if (tb.denom == 0)
    {
        mach_timebase_info(&tb);
    }
    return mach_units * tb.numer / tb.denom;
}

/* Read `pid`'s resident memory footprint (bytes) and cumulative CPU time
 * (nanoseconds, user + system) in one call. Both 0 if it can't be read
 * (proc_pid_rusage only succeeds for our own processes -- the set we care
 * about). ri_phys_footprint matches Activity Monitor's "Memory" column. */
static void proc_macos_rusage(int pid, unsigned long long *footprint,
                              unsigned long long *cpu_ns)
{
    struct rusage_info_v2 ri;

    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *) &ri) == 0)
    {
        *footprint = ri.ri_phys_footprint;
        *cpu_ns    = proc_macos_mach_to_ns(ri.ri_user_time
                                         + ri.ri_system_time);
    }
    else
    {
        *footprint = 0;
        *cpu_ns    = 0;
    }
}

/* ---- CPU load: the rate of cumulative CPU time between successive list()
 * calls. We remember each process's cumulative CPU time and a monotonic
 * timestamp; the next call divides the delta by the elapsed wall time.
 * 100 = one full core busy for the whole interval (can exceed 100 for a
 * multi-threaded process). */
#define PROC_HIST_MAX 4096

typedef struct
{
    int                pid;
    unsigned long long cpu_ns;
} cpu_sample_t;

static cpu_sample_t       cpu_hist[PROC_HIST_MAX];
static int                cpu_hist_count;
static unsigned long long cpu_hist_ns;        /* when cpu_hist was taken      */

static unsigned long long proc_macos_mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000000ULL
         + (unsigned long long) ts.tv_nsec;
}

/* Previous cumulative CPU time for `pid`, with *found cleared if unseen. */
static unsigned long long proc_macos_prev_cpu(int pid, int *found)
{
    int i;

    for (i = 0; i < cpu_hist_count; i++)
    {
        if (cpu_hist[i].pid == pid)
        {
            *found = 1;
            return cpu_hist[i].cpu_ns;
        }
    }
    *found = 0;
    return 0;
}

/* The kernel's record of when the process started (kp_proc.p_starttime), as
 * microseconds since the epoch. A pid is reused only after its process has
 * exited, and the new process gets a new start time, so (pid, start) names
 * one process lifetime. */
static unsigned long long proc_macos_start_token(const struct kinfo_proc *kp)
{
    return (unsigned long long) kp->kp_proc.p_starttime.tv_sec * 1000000ULL
         + (unsigned long long) kp->kp_proc.p_starttime.tv_usec;
}

/* Fill the identity and ownership fields of `out` from `kp` (metrics are the
 * caller's business). */
static void proc_macos_fill(const struct kinfo_proc *kp, psd_proc_t *out)
{
    out->pid         = kp->kp_proc.p_pid;
    out->start_token = proc_macos_start_token(kp);
    out->ppid        = kp->kp_eproc.e_ppid;
    out->uid         = (unsigned int) kp->kp_eproc.e_ucred.cr_uid;
    out->is_daemon   = (kp->kp_eproc.e_tdev == NODEV) ? 1 : 0;

    strncpy(out->name, kp->kp_proc.p_comm, PSD_PROC_NAME_MAX - 1);
    out->name[PSD_PROC_NAME_MAX - 1] = '\0';
}

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
    {
        static cpu_sample_t newhist[PROC_HIST_MAX];
        int newcount = 0;
        unsigned long long now = proc_macos_mono_ns();
        unsigned long long dwall = (cpu_hist_ns != 0 && now > cpu_hist_ns)
                                 ? now - cpu_hist_ns : 0;

        for (i = 0; i < total && count < max; i++)
        {
            struct kinfo_proc *kp = &procs[i];
            int pid = kp->kp_proc.p_pid;
            unsigned long long fp;
            unsigned long long cpu;
            unsigned long long prev;
            int found;

            if (pid <= 0)
            {
                continue;
            }

            proc_macos_rusage(pid, &fp, &cpu);
            prev = proc_macos_prev_cpu(pid, &found);

            proc_macos_fill(kp, &out[count]);
            out[count].footprint = fp;

            /* CPU load over the interval; guard the cold start (no prior
             * sample / no elapsed time) and pid reuse (cpu < prev). */
            if (found && dwall > 0 && cpu >= prev)
            {
                out[count].cpu_percent = (int) (((cpu - prev) * 100ULL) / dwall);
            }
            else
            {
                out[count].cpu_percent = 0;
            }

            if (newcount < PROC_HIST_MAX)
            {
                newhist[newcount].pid    = pid;
                newhist[newcount].cpu_ns = cpu;
                newcount++;
            }
            count++;
        }

        /* Commit this snapshot as the baseline for the next call. */
        memcpy(cpu_hist, newhist, (size_t) newcount * sizeof(cpu_sample_t));
        cpu_hist_count = newcount;
        cpu_hist_ns    = now;
    }

    free(procs);
    return count;
}

int proc_macos_lookup(int pid, psd_proc_t *out)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    struct kinfo_proc kp;
    size_t len = sizeof(kp);

    if (pid <= 0 || out == NULL)
    {
        return 0;
    }

    /* For a pid with no process, sysctl succeeds with len == 0. A zombie is
     * reported like list() reports it: its pid cannot be reused until its
     * parent reaps it, so it is still the same process (and signalling it is
     * harmless). */
    memset(&kp, 0, sizeof(kp));
    if (sysctl(mib, 4, &kp, &len, NULL, 0) < 0 || len < sizeof(kp)
        || kp.kp_proc.p_pid != pid)
    {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    proc_macos_fill(&kp, out);
    return 1;
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

/* The native macOS backend, registered as the default in proc_backend.c. */
const proc_backend_t proc_macos_backend =
{
    proc_macos_list,
    proc_macos_current_uid,
    proc_macos_renice,
    proc_macos_kill,
    proc_macos_lookup,
};
