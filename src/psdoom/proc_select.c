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

static psd_proc_t select_raw[PSD_RAW_MAX];   /* raw snapshot scratch          */
static int        select_raw_count;          /* size of the last raw snapshot */

void psd_select_init(void)
{
    select_raw_count = 0;
}

int psd_select_collect(psd_proc_t *out, int max)
{
    int n;
    int i;

    if (out == NULL || max <= 0)
    {
        return 0;
    }

    /* Zero the scratch first: a backend fills only the fields it knows, so
     * nothing may carry over from an earlier list or another backend. */
    memset(select_raw, 0, sizeof(select_raw));
    n = proc_backend()->list(select_raw, PSD_RAW_MAX);
    if (n < 0)
    {
        n = 0;
    }
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
