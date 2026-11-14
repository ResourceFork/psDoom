/*
 * psDoom -- backend selection.
 *
 * Holds the pointer to the active backend. psDoom has no native process
 * backend: the only path to real processes is Summoner, through the script
 * backend (proc_script.c). Until one is installed -- or if Summoner cannot
 * run -- the default is an inert backend that lists nothing and acts on
 * nothing. Tests call proc_backend_set() to swap in the fake backend.
 */

#include "proc_backend.h"

#include <stddef.h>   /* NULL    */
#include <string.h>   /* strncmp */
#include <unistd.h>   /* getuid  */

static int          none_list(psd_proc_t *out, int max)
{
    (void) out;
    (void) max;
    return 0;
}
static unsigned int none_current_uid(void) { return (unsigned int) getuid(); }
static void         none_renice(int pid, int nice_delta) { (void) pid; (void) nice_delta; }
static int          none_kill(int pid) { (void) pid; return 0; }
static int          none_lookup(int pid, psd_proc_t *out) { (void) pid; (void) out; return 0; }

/* Lists nothing, acts on nothing: no process monsters, no process effects. */
const proc_backend_t proc_none_backend =
{
    none_list,
    none_current_uid,
    none_renice,
    none_kill,
    none_lookup,
    0,
};

static const proc_backend_t *g_default_backend = &proc_none_backend;

static const proc_backend_t *g_backend; /* NULL => use the default */

const proc_backend_t *proc_backend(void)
{
    return g_backend != NULL ? g_backend : g_default_backend;
}

void proc_backend_set(const proc_backend_t *backend)
{
    g_backend = backend;
}

int psd_proc_same(const psd_proc_t *cur, int pid,
                  unsigned long long start_token, const char *name)
{
    if (cur == NULL || name == NULL)
    {
        return 0;
    }
    if (cur->pid != pid || cur->start_token != start_token)
    {
        return 0;
    }
    /* With no start token (script entities) the contract's identity is the id
     * alone, and a label may change; with one (OS processes) the name must
     * also match, so an exec into another program is not acted on. */
    return start_token == 0
        || strncmp(cur->name, name, PSD_PROC_IDENT_NAME_LEN) == 0;
}

/*
 * Re-read `pid` and confirm it is still the process identified by
 * (start_token, name). PSD_ACT_DONE means "verified: go ahead".
 *
 * The caller acts immediately after this returns, so the only window left is
 * the few microseconds between the lookup and the action: kill(2) for a kill;
 * getpriority(2) then setpriority(2) for a renice. If the process exits AND
 * the OS hands its pid to a new process inside that window, the new process
 * is acted on. macOS hands out pids in sequence up to PID_MAX (99999), so that
 * needs a full wrap within microseconds; it has no pid handle (no pidfd) to
 * close the window, and psDoom is not the parent, so it cannot hold the pid as
 * a zombie. For the script backend "now" is the latest poll, so its window is
 * up to one sync interval, and the responder itself acts later still (see
 * docs/architecture.md, safety model).
 */
static psd_act_result_t psd_proc_verify(const proc_backend_t *b, int pid,
                                        unsigned long long start_token,
                                        const char *name)
{
    psd_proc_t cur;

    if (b == NULL || b->lookup == NULL || pid <= 1 || name == NULL)
    {
        return PSD_ACT_REFUSED;   /* cannot verify: fail closed */
    }
    memset(&cur, 0, sizeof(cur));
    if (!b->lookup(pid, &cur))
    {
        return PSD_ACT_GONE;
    }
    if (!psd_proc_same(&cur, pid, start_token, name))
    {
        return PSD_ACT_REUSED;
    }
    return PSD_ACT_DONE;
}

psd_act_result_t psd_proc_renice_checked(int pid, unsigned long long start_token,
                                         const char *name, int nice_delta)
{
    const proc_backend_t *b = proc_backend();
    psd_act_result_t      r = psd_proc_verify(b, pid, start_token, name);

    if (r != PSD_ACT_DONE)
    {
        return r;
    }
    b->renice(pid, nice_delta);
    return PSD_ACT_DONE;
}

psd_act_result_t psd_proc_kill_checked(int pid, unsigned long long start_token,
                                       const char *name)
{
    const proc_backend_t *b = proc_backend();
    psd_act_result_t      r = psd_proc_verify(b, pid, start_token, name);

    if (r != PSD_ACT_DONE)
    {
        return r;
    }
    return b->kill(pid) ? PSD_ACT_DONE : PSD_ACT_FAILED;
}
