/*
 * psDoom -- proc_select / backend host tests.
 *
 * Engine-free: links only proc_select, proc_backend and the fake backend, so
 * the triage policy and the backend plumbing are exercised deterministically
 * off-machine (no live process list, no Doom engine). proc_select calls one
 * option getter (psdoom_all_users_enabled); we stub it here rather than link
 * the engine-coupled options module.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "proc_select.h"
#include "proc_backend.h"
#include "proc_fake.h"

#include <stdio.h>
#include <string.h>

/* ---- stub for the one option getter proc_select references --------------- */
static int stub_all_users;
int psdoom_all_users_enabled(void) { return stub_all_users; }

/* ---- tiny check harness -------------------------------------------------- */
static int checks_run;
static int checks_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* Find the index of `pid` in a collection, or -1. */
static int index_of(const psd_proc_t *p, int n, int pid)
{
    int i;
    for (i = 0; i < n; i++)
    {
        if (p[i].pid == pid)
        {
            return i;
        }
    }
    return -1;
}

static psd_proc_t mkproc(int pid, int ppid, unsigned int uid, int daemon,
                         unsigned long long footprint, const char *name)
{
    psd_proc_t p;
    memset(&p, 0, sizeof(p));
    p.pid       = pid;
    p.ppid      = ppid;
    p.uid       = uid;
    p.is_daemon = daemon;
    p.footprint = footprint;
    strncpy(p.name, name, PSD_PROC_NAME_MAX - 1);
    return p;
}

/* ---- tests --------------------------------------------------------------- */

/* The pure triage core: ownership, self, pid<=1, protected names and the
 * launcher chain are excluded; the survivors are ranked by relevance. */
static void test_triage_filters_and_ranking(void)
{
    /* self = pid 100, launched by zsh(90) under Terminal(80). */
    psd_proc_t raw[] = {
        mkproc(1,   0,   501, 1, 0,            "launchd"),    /* pid<=1 + protected */
        mkproc(100, 90,  501, 0, 0,            "psDoom"),     /* self               */
        mkproc(90,  80,  501, 1, 0,            "zsh"),        /* launcher chain     */
        mkproc(80,  1,   501, 1, 0,            "Terminal"),   /* launcher chain     */
        mkproc(200, 80,  501, 0, 64ULL << 20, "vim"),        /* interactive: keep  */
        mkproc(201, 1,   501, 1, 0,            "syslogd"),    /* daemon: keep       */
        mkproc(202, 1,   999, 0, 0,            "otherapp"),   /* other user         */
        mkproc(203, 1,   501, 0, 0,            "Dock"),       /* protected          */
    };
    int n_raw = (int) (sizeof(raw) / sizeof(raw[0]));
    psd_proc_t out[16];
    int n;

    n = psd_select_triage(raw, n_raw, /*uid*/501, /*self*/100, /*all_users*/0,
                          out, 16);

    /* Exactly the two keepers survive. */
    CHECK(n == 2);
    CHECK(index_of(out, n, 200) >= 0);   /* vim kept     */
    CHECK(index_of(out, n, 201) >= 0);   /* syslogd kept */

    /* Everything else excluded. */
    CHECK(index_of(out, n, 1)   < 0);    /* launchd / pid<=1 */
    CHECK(index_of(out, n, 100) < 0);    /* self             */
    CHECK(index_of(out, n, 90)  < 0);    /* launcher: zsh    */
    CHECK(index_of(out, n, 80)  < 0);    /* launcher: term   */
    CHECK(index_of(out, n, 202) < 0);    /* other user       */
    CHECK(index_of(out, n, 203) < 0);    /* protected: Dock  */

    /* Interactive (vim) outranks the daemon (syslogd). */
    CHECK(index_of(out, n, 200) < index_of(out, n, 201));
}

/* all_users=1 keeps other users' processes (but still drops self / pid<=1 /
 * protected). */
static void test_triage_all_users(void)
{
    psd_proc_t raw[] = {
        mkproc(200, 80, 501, 0, 0, "vim"),
        mkproc(202, 1,  999, 0, 0, "otherapp"),
    };
    psd_proc_t out[8];
    int n = psd_select_triage(raw, 2, 501, 100, /*all_users*/1, out, 8);

    CHECK(n == 2);
    CHECK(index_of(out, n, 202) >= 0);   /* other user now kept */
}

/* Truncation honors `max`, keeping the most relevant. */
static void test_triage_truncation(void)
{
    psd_proc_t raw[] = {
        mkproc(200, 1, 501, 0, 0, "vim"),       /* interactive (higher) */
        mkproc(201, 1, 501, 1, 0, "syslogd"),   /* daemon    (lower)    */
    };
    psd_proc_t out[1];
    int n = psd_select_triage(raw, 2, 501, 100, 0, out, 1);

    CHECK(n == 1);
    CHECK(out[0].pid == 200);            /* the interactive one survives */
}

/* psd_select_collect pulls through the active (fake) backend, and the fake's
 * action log records renice/kill exactly as the game would drive them. */
static void test_collect_and_action_log(void)
{
    psd_proc_t snap[] = {
        mkproc(300, 1, 501, 0, 0, "firefox"),
        mkproc(301, 1, 999, 0, 0, "rootthing"),   /* other user: excluded */
        mkproc(302, 1, 501, 0, 0, "bash"),
    };
    psd_proc_t out[16];
    int n;

    proc_fake_install(/*uid*/501);
    stub_all_users = 0;
    proc_fake_push_snapshot(snap, 3);
    psd_select_init();                   /* picks up uid 501 from the fake */

    n = psd_select_collect(out, 16);
    CHECK(n == 2);                        /* firefox + bash; rootthing dropped */
    CHECK(index_of(out, n, 300) >= 0);
    CHECK(index_of(out, n, 302) >= 0);
    CHECK(index_of(out, n, 301) < 0);

    /* The action ops the game uses are logged by the fake. */
    proc_backend()->renice(300, 4);
    proc_backend()->kill(302);
    CHECK(proc_fake_renice_count() == 1);
    CHECK(proc_fake_last_renice_pid() == 300);
    CHECK(proc_fake_last_renice_delta() == 4);
    CHECK(proc_fake_kill_count() == 1);
    CHECK(proc_fake_last_kill_pid() == 302);
}

/* psd_select_child_count reports per-parent child counts over the last raw
 * (unfiltered) snapshot -- the signal the fork-bomb detector keys on. */
static void test_child_count(void)
{
    psd_proc_t snap[] = {
        mkproc(400, 1,   501, 0, 0, "parent"),
        mkproc(401, 400, 501, 0, 0, "child1"),
        mkproc(402, 400, 501, 0, 0, "child2"),
        mkproc(403, 400, 501, 0, 0, "child3"),
        mkproc(404, 1,   501, 0, 0, "unrelated"),
    };
    psd_proc_t out[16];

    proc_fake_install(501);
    stub_all_users = 0;
    proc_fake_push_snapshot(snap, 5);
    psd_select_init();
    (void) psd_select_collect(out, 16);   /* populates the raw snapshot */

    CHECK(psd_select_child_count(400) == 3);   /* three children          */
    CHECK(psd_select_child_count(404) == 0);   /* a leaf                  */
    CHECK(psd_select_child_count(1)   == 2);   /* 400 + 404 parented to 1 */
    CHECK(psd_select_child_count(0)   == 0);   /* guard                   */
}

int main(void)
{
    test_triage_filters_and_ranking();
    test_triage_all_users();
    test_triage_truncation();
    test_collect_and_action_log();
    test_child_count();

    printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
