/*
 * psDoom tests -- wait for the script backend's poll.
 *
 * The script backend polls on a worker thread: list() and
 * psd_select_collect() return -1 ("not yet") while the poll runs, and the
 * game calls again on a later tic. A test that wants the poll's result calls
 * these instead: they call again every 5 ms, as the game would every tic,
 * for up to 5 s, and return the first result (or -1 if none came).
 */

#ifndef PSD_TEST_WAIT_H
#define PSD_TEST_WAIT_H

#include "proc_backend.h"
#include "proc_select.h"

#include <unistd.h>   /* usleep */

static inline int psd_test_list_wait(psd_proc_t *out, int max)
{
    int n;
    int waited;

    for (waited = 0; (n = proc_backend()->list(out, max)) < 0 && waited < 5000;
         waited += 5)
    {
        usleep(5000);
    }
    return n;
}

static inline int psd_test_collect_wait(psd_proc_t *out, int max)
{
    int n;
    int waited;

    for (waited = 0; (n = psd_select_collect(out, max)) < 0 && waited < 5000;
         waited += 5)
    {
        usleep(5000);
    }
    return n;
}

#endif /* PSD_TEST_WAIT_H */
