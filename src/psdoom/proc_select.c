/*
 * psDoom -- process selection / triage layer (implementation).
 *
 * Engine-free policy: acquire (via proc_macos), filter, rank and truncate the
 * live process list into the collection the monster creator consumes. See
 * proc_select.h for the layering and the contract.
 */

#include "proc_select.h"
#include "proc_backend.h"
#include "psdoom_options.h"   /* psdoom_all_users_enabled() */

#include <stdlib.h>   /* qsort         */
#include <string.h>   /* strcmp/strstr */
#include <unistd.h>   /* getpid        */

/* ------------------------------------------------------------------ config */

/* Upper bound on the raw snapshot. A busy desktop runs well over 512 processes;
 * sysctl returns them in an order that would otherwise drop arbitrary entries
 * (including the session-critical ones we must see in order to exclude them),
 * so keep plenty of headroom. */
#define PSD_RAW_MAX       4096

/* Depth of launcher-chain protection (self -> shell -> terminal -> ...). */
#define PSD_MAX_ANCESTORS 16

/* Relevance scoring. Higher score survives truncation. These are deliberately
 * simple and live here so the ranking can be tuned in one isolated place. */
#define PSD_SCORE_BASE        100
#define PSD_SCORE_INTERACTIVE  50   /* bonus: has a controlling tty           */
#define PSD_SCORE_NOISE        80   /* penalty: looks like a system helper     */
#define PSD_SCORE_MEM_MAX     120   /* cap on the memory-footprint bonus       */

/* ------------------------------------------------------------------- state */

static psd_proc_t   select_raw[PSD_RAW_MAX];   /* raw snapshot scratch        */
static int          select_raw_count;          /* size of the last raw snapshot */
static psd_proc_t   select_work[PSD_RAW_MAX];  /* survivors, pre-sort scratch */

static unsigned int select_uid;
static int          select_self_pid;

/*
 * Session-critical, UID-owned processes that must never become monsters.
 * Killing any of these would disrupt or end the GUI login session. Matched
 * against the kernel short name (p_comm, truncated to ~15 chars).
 * NULL-terminated; extend as needed.
 */
static const char *const select_protected_names[] =
{
    "loginwindow",      /* killing this logs the user out                    */
    "WindowServer",     /* the display server (usually _windowserver-owned)  */
    "Dock",             /* Dock + Mission Control                            */
    "Finder",
    "SystemUIServer",   /* menu-bar extras                                   */
    "launchd",          /* pid 1 (defensive; also excluded by pid <= 1)      */
    "kernel_task",      /* defensive                                         */
    NULL,
};

/*
 * Substrings marking low-relevance background processes (Apple helpers, XPC
 * services, indexers, synthesizers, ...). Presence drops a process's score so
 * real user apps win the truncation. Matched case-insensitively against the
 * kernel short name (p_comm). Heuristic and tunable -- this is the one place to
 * adjust what counts as "noise".
 */
static const char *const select_noise_substr[] =
{
    "com.apple", "mdworker", "cfprefsd", "distnoted", "trustd",
    "nsurlsession", "analyticsd", "knowledge", "spotlight", "coreaudiod",
    "diagnostic", "crashpad", "mtlcompiler", "gputools", "assetsd",
    "synthesizer", "agent", "helper", "service", "daemon", "xpc", "ausp",
    NULL,
};

/* ------------------------------------------------------------------ policy */

static int select_is_protected_name(const char *name)
{
    int i;

    for (i = 0; select_protected_names[i] != NULL; i++)
    {
        if (strcmp(name, select_protected_names[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int select_is_noise_name(const char *name)
{
    int i;

    for (i = 0; select_noise_substr[i] != NULL; i++)
    {
        if (strcasestr(name, select_noise_substr[i]) != NULL)
        {
            return 1;
        }
    }
    return 0;
}

/* Relevance score for a candidate (already past the keep filter). */
static int select_score(const psd_proc_t *p)
{
    int score = PSD_SCORE_BASE;
    int mem_bonus;

    if (!p->is_daemon)                  /* interactive / tty-owning: user is here */
    {
        score += PSD_SCORE_INTERACTIVE;
    }
    if (select_is_noise_name(p->name))  /* background system noise               */
    {
        score -= PSD_SCORE_NOISE;
    }

    /* Heavier processes are more interesting (and become bigger monsters), so
     * give them a bounded relevance bonus: ~+1 per 32 MB, capped. Keeps a
     * memory hog from being truncated away before it can be classified. */
    mem_bonus = (int) (p->footprint >> 25);   /* bytes / 32 MiB */
    if (mem_bonus > PSD_SCORE_MEM_MAX)
    {
        mem_bonus = PSD_SCORE_MEM_MAX;
    }
    score += mem_bonus;

    return score;
}

/* Parent pid of `pid` in `raw`, or 0 if absent. */
static int select_ppid_of(const psd_proc_t *raw, int n, int pid)
{
    int i;

    for (i = 0; i < n; i++)
    {
        if (raw[i].pid == pid)
        {
            return raw[i].ppid;
        }
    }
    return 0;
}

/* Fill `out` with the launcher chain above `self_pid` (shell, terminal, ...),
 * walking ppid up `raw`. Returns the count (<= max). */
static int select_build_ancestors(const psd_proc_t *raw, int n, int self_pid,
                                  int *out, int max)
{
    int pid = self_pid;
    int count = 0;
    int guard;

    for (guard = 0; guard < max; guard++)
    {
        int ppid = select_ppid_of(raw, n, pid);

        if (ppid <= 1)
        {
            break;  /* reached launchd / kernel */
        }

        out[count++] = ppid;
        pid = ppid;
    }
    return count;
}

static int select_pid_in(const int *pids, int n, int pid)
{
    int i;

    for (i = 0; i < n; i++)
    {
        if (pids[i] == pid)
        {
            return 1;
        }
    }
    return 0;
}

/* qsort comparator: descending score, then newer (higher) pid as a stable,
 * deterministic tiebreak. */
static int select_cmp(const void *a, const void *b)
{
    const psd_proc_t *pa = (const psd_proc_t *) a;
    const psd_proc_t *pb = (const psd_proc_t *) b;
    int sa = select_score(pa);
    int sb = select_score(pb);

    if (sa != sb)
    {
        return sb - sa;
    }
    return pb->pid - pa->pid;
}

/* --------------------------------------------------------------- public API */

void psd_select_init(void)
{
    select_uid      = proc_backend()->current_uid();
    select_self_pid = (int) getpid();
}

int psd_select_triage(const psd_proc_t *raw, int n_raw,
                      unsigned int uid, int self_pid, int all_users,
                      psd_proc_t *out, int max)
{
    int ancestors[PSD_MAX_ANCESTORS];
    int n_ancestors;
    int kept = 0;
    int n;
    int i;

    if (raw == NULL || out == NULL || max <= 0 || n_raw <= 0)
    {
        return 0;
    }

    n_ancestors = select_build_ancestors(raw, n_raw, self_pid,
                                         ancestors, PSD_MAX_ANCESTORS);

    /* Filter: keep only processes the player may act on. */
    for (i = 0; i < n_raw && kept < PSD_RAW_MAX; i++)
    {
        const psd_proc_t *p = &raw[i];

        if (!all_users && p->uid != uid)          continue; /* not ours        */
        if (p->pid == self_pid)                   continue; /* the game itself  */
        if (p->pid <= 1)                          continue; /* kernel/launchd   */
        if (select_is_protected_name(p->name))    continue; /* session-critical */
        if (select_pid_in(ancestors, n_ancestors, p->pid)) continue; /* launcher */

        select_work[kept++] = *p;
    }

    /* Rank by relevance, then truncate to the caller's capacity. */
    qsort(select_work, (size_t) kept, sizeof(psd_proc_t), select_cmp);

    n = (kept < max) ? kept : max;
    for (i = 0; i < n; i++)
    {
        out[i] = select_work[i];
    }
    return n;
}

int psd_select_collect(psd_proc_t *out, int max)
{
    int n_raw = proc_backend()->list(select_raw, PSD_RAW_MAX);

    select_raw_count = n_raw;   /* remember for psd_select_child_count() */

    return psd_select_triage(select_raw, n_raw,
                             select_uid, select_self_pid,
                             psdoom_all_users_enabled(), out, max);
}

int psd_select_child_count(int pid)
{
    int count = 0;
    int i;

    if (pid <= 0)
    {
        return 0;
    }
    for (i = 0; i < select_raw_count; i++)
    {
        if (select_raw[i].ppid == pid)
        {
            count++;
        }
    }
    return count;
}
