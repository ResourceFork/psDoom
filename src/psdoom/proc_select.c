/*
 * psDoom -- process selection layer (implementation).
 *
 * Engine-free: acquire the entity list from the active backend and cut it to
 * the caller's capacity, in the order the backend gave. psDoom no longer
 * reads, filters or ranks real processes itself: Summoner and its trusted
 * plugin own what is listed and in what order (docs/script-backend.md,
 * "Trust"). See proc_select.h.
 */

#include "proc_select.h"
#include "proc_backend.h"

#include <string.h>   /* memset */

/* Upper bound on one listing (the script contract's per-poll limit). */
#define PSD_RAW_MAX 4096

/* Two raw buffers: the next listing goes into the spare one, so a backend
 * that has no new list yet leaves the last one (and its child counts) whole. */
static psd_proc_t  select_bufs[2][PSD_RAW_MAX];
static psd_proc_t *select_raw = select_bufs[0];  /* the last raw snapshot    */
static int         select_raw_count;             /* its size                 */
/* 1 while the spare buffer is still all zero: a backend that returns -1
 * writes nothing, so it need not be cleared again for the next call. */
static int         select_next_clean;

void psd_select_init(void)
{
    select_raw_count = 0;
}

int psd_select_collect(psd_proc_t *out, int max)
{
    psd_proc_t *next;
    int         n;
    int         i;

    if (out == NULL || max <= 0)
    {
        return 0;
    }

    /* Zero the scratch first: a backend fills only the fields it knows, so
     * nothing may carry over from an earlier list or another backend. */
    next = (select_raw == select_bufs[0]) ? select_bufs[1] : select_bufs[0];
    if (!select_next_clean)
    {
        memset(next, 0, sizeof(select_bufs[0]));
    }
    n = proc_backend()->list(next, PSD_RAW_MAX);
    if (n < 0)
    {
        select_next_clean = 1;   /* written nothing: still clean */
        return -1;               /* no new list yet: keep the last one */
    }
    select_next_clean = 0;
    if (n > PSD_RAW_MAX)
    {
        n = PSD_RAW_MAX;
    }
    select_raw       = next;
    select_raw_count = n;   /* remember for psd_select_child_count() */

    /* The backend's order is the order to show; only the cut is ours. */
    if (n > max)
    {
        n = max;
    }
    for (i = 0; i < n; i++)
    {
        out[i] = select_raw[i];
    }
    return n;
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
