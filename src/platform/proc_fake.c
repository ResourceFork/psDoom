/*
 * psDoom -- fake process backend (implementation). See proc_fake.h.
 */

#include "proc_fake.h"

#include <string.h>

#define FAKE_MAX_SNAPSHOTS 64
#define FAKE_MAX_PROCS     256

typedef struct
{
    psd_proc_t procs[FAKE_MAX_PROCS];
    int        count;
} fake_snapshot_t;

static fake_snapshot_t fake_snaps[FAKE_MAX_SNAPSHOTS];
static int             fake_snap_count;   /* queued snapshots          */
static int             fake_snap_next;    /* next to serve             */
static unsigned int    fake_uid;

static int fake_renice_count;
static int fake_kill_count;
static int fake_last_renice_pid;
static int fake_last_renice_delta;
static int fake_last_kill_pid;

/* ------------------------------------------------------------- vtable ops */

static int fake_list(psd_proc_t *out, int max)
{
    const fake_snapshot_t *s;
    int n;
    int i;

    if (out == NULL || max <= 0 || fake_snap_count == 0)
    {
        return 0;
    }

    /* Serve queued snapshots in order; once drained, repeat the last one (so a
     * steady state can be held without re-pushing every sync). */
    if (fake_snap_next < fake_snap_count)
    {
        s = &fake_snaps[fake_snap_next++];
    }
    else
    {
        s = &fake_snaps[fake_snap_count - 1];
    }

    n = (s->count < max) ? s->count : max;
    for (i = 0; i < n; i++)
    {
        out[i] = s->procs[i];
    }
    return n;
}

static unsigned int fake_current_uid(void)
{
    return fake_uid;
}

static void fake_renice(int pid, int nice_delta)
{
    fake_renice_count++;
    fake_last_renice_pid   = pid;
    fake_last_renice_delta = nice_delta;
}

static int fake_kill(int pid)
{
    fake_kill_count++;
    fake_last_kill_pid = pid;
    return 1;
}

static const proc_backend_t fake_backend =
{
    fake_list,
    fake_current_uid,
    fake_renice,
    fake_kill,
};

/* ----------------------------------------------------------------- public */

void proc_fake_reset(void)
{
    fake_snap_count        = 0;
    fake_snap_next         = 0;
    fake_renice_count      = 0;
    fake_kill_count        = 0;
    fake_last_renice_pid   = 0;
    fake_last_renice_delta = 0;
    fake_last_kill_pid     = 0;
}

void proc_fake_install(unsigned int uid)
{
    proc_fake_reset();
    fake_uid = uid;
    proc_backend_set(&fake_backend);
}

void proc_fake_push_snapshot(const psd_proc_t *procs, int n)
{
    fake_snapshot_t *s;
    int i;

    if (fake_snap_count >= FAKE_MAX_SNAPSHOTS)
    {
        return;
    }
    if (n < 0)
    {
        n = 0;
    }
    if (n > FAKE_MAX_PROCS)
    {
        n = FAKE_MAX_PROCS;
    }

    s = &fake_snaps[fake_snap_count++];
    s->count = n;
    for (i = 0; i < n && procs != NULL; i++)
    {
        s->procs[i] = procs[i];
    }
}

int proc_fake_renice_count(void)     { return fake_renice_count; }
int proc_fake_kill_count(void)       { return fake_kill_count; }
int proc_fake_last_renice_pid(void)  { return fake_last_renice_pid; }
int proc_fake_last_renice_delta(void){ return fake_last_renice_delta; }
int proc_fake_last_kill_pid(void)    { return fake_last_kill_pid; }
