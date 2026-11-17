/*
 * psDoom -- proc_select / backend host tests.
 *
 * Engine-free: links proc_select, proc_backend, the fake and script backends
 * (psDoom has no native process backend: Summoner is the only path to real
 * processes), so selection, the identity guard and the backend plumbing are
 * exercised deterministically off-machine (no Doom engine), and so are the
 * process-monster lifecycle rules (psdoom_lifecycle: what a save keeps, how
 * a monster leaves). The script tests spawn only their own poll and respond
 * commands and never signal anything. The launcher's IWAD selection tests
 * (iwad_select_test.c) run from the same binary.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "proc_select.h"
#include "psd_test_wait.h"
#include "proc_backend.h"
#include "proc_fake.h"
#include "proc_script.h"
#include "psdoom_lifecycle.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h> /* RLIMIT_NOFILE (the fd tests) */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>     /* waitpid (reap our own zombie child) */
#include <signal.h>       /* sigprocmask (the signal-mask probe) */


void iwad_select_tests(int *checks_run, int *checks_failed); /* iwad_select_test.c */
void summoner_e2e_tests(int *checks_run, int *checks_failed); /* summoner_e2e_test.c */
void script_grade_tests(int *checks_run, int *checks_failed); /* script_grade_test.c */
void script_request_tests(int *checks_run, int *checks_failed); /* script_request_test.c */
void summoner_bundle_tests(int *checks_run, int *checks_failed); /* summoner_bundle_test.c */
void poll_limit_tests(int *checks_run, int *checks_failed); /* poll_limit_test.c */

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

/* psd_select_collect keeps the backend's order and filters nothing (psDoom
 * trusts Summoner's plugin), and the fake's action log records renice/kill
 * exactly as the game would drive them. */
static void test_collect_and_action_log(void)
{
    psd_proc_t snap[] = {
        mkproc(300, 1, 501, 0, 0, "firefox"),
        mkproc(301, 1, 999, 0, 0, "rootthing"),   /* another uid: kept */
        mkproc(302, 1, 501, 0, 0, "Dock"),        /* protected name: kept */
        mkproc(303, 1, 501, 0, 1ULL << 33, "big"),
    };
    psd_proc_t out[16];
    int n;

    proc_fake_install(/*uid*/501);
    proc_fake_push_snapshot(snap, 4);
    psd_select_init();

    n = psd_test_collect_wait(out, 16);
    CHECK(n == 4);
    CHECK(n == 4 && out[0].pid == 300 && out[1].pid == 301
          && out[2].pid == 302 && out[3].pid == 303);   /* no re-rank */
    proc_fake_push_snapshot(snap, 4);
    n = psd_test_collect_wait(out, 2);
    CHECK(n == 2 && out[0].pid == 300 && out[1].pid == 301);   /* the head */

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
    proc_fake_push_snapshot(snap, 5);
    psd_select_init();
    (void) psd_test_collect_wait(out, 16);   /* populates the raw snapshot */

    CHECK(psd_select_child_count(400) == 3);   /* three children          */
    CHECK(psd_select_child_count(404) == 0);   /* a leaf                  */
    CHECK(psd_select_child_count(1)   == 2);   /* 400 + 404 parented to 1 */
    CHECK(psd_select_child_count(0)   == 0);   /* guard                   */
}

/* A backend that reports a child count (`children=`) is believed over the
 * listed children: with the list cut to the monster count, a forking
 * parent's children are mostly not listed. Without one, the listed children
 * are counted as before. */
static void test_child_count_reported(void)
{
    psd_proc_t snap[] = {
        mkproc(410, 1,   501, 0, 0, "forker"),
        mkproc(411, 410, 501, 0, 0, "one listed child"),
        mkproc(412, 1,   501, 0, 0, "counted"),
    };
    psd_proc_t out[16];
    int        n;

    snap[0].has_children = 1;
    snap[0].children     = 40;
    proc_fake_install(501);
    proc_fake_push_snapshot(snap, 3);
    psd_select_init();
    (void) psd_test_collect_wait(out, 16);
    CHECK(psd_select_child_count(410) == 40);  /* reported, not 1 listed */
    CHECK(psd_select_child_count(412) == 0);   /* not reported: counted  */

    /* The script contract carries it: children=N, a whole number >= 0. */
    n = psd_script_parse("id=7\tchildren=12\tlabel=a\n"
                         "id=8\tchildren=-3\n"
                         "id=9\tchildren=x\n"
                         "id=10\n", out, 16);
    CHECK(n == 4);
    CHECK(n == 4 && out[0].has_children == 1 && out[0].children == 12);
    CHECK(n == 4 && out[1].has_children == 0);
    CHECK(n == 4 && out[2].has_children == 0);
    CHECK(n == 4 && out[3].has_children == 0);
}

/* ---- script backend: contract parser + a real subprocess round-trip ------ */

/* The poll parser (docs/script-backend.md): tab-separated key=value, comments
 * and blanks skipped, unknown keys ignored, id>1 required, last-wins, spaces
 * preserved in values. */
static void test_script_parse(void)
{
    const char *buf =
        "# a comment\n"
        "\n"
        "id=4123\tlabel=web frontend\tweight=734003200\tload=140\tparent=4000\n"
        "id=4124\tweight=64\tflags=1\textra=ignored\n"  /* unknown key ignored */
        "label=orphan\tweight=10\n"                       /* no id -> skipped    */
        "id=1\tlabel=reserved\n"                           /* id reserved -> skip */
        "id=5000\tload=10\tload=20\n";                     /* repeated: last wins */
    psd_proc_t out[16];
    int n = psd_script_parse(buf, out, 16);

    CHECK(n == 3);

    CHECK(out[0].has_grade == 0);       /* no grade field: weight/load */
    CHECK(out[0].pid == 4123);
    CHECK(out[0].ppid == 4000);
    CHECK(out[0].footprint == 734003200ULL);
    CHECK(out[0].cpu_percent == 140);
    CHECK(strcmp(out[0].name, "web frontend") == 0);   /* spaces preserved */
    CHECK(out[0].is_daemon == 0);

    CHECK(out[1].pid == 4124);
    CHECK(out[1].ppid == 0);            /* default        */
    CHECK(out[1].is_daemon == 1);       /* flags bit 0    */
    CHECK(out[1].name[0] == '\0');      /* default label  */

    CHECK(out[2].pid == 5000);
    CHECK(out[2].cpu_percent == 20);    /* last value won */
}

static void test_script_parse_truncates(void)
{
    const char *buf = "id=10\nid=11\nid=12\n";
    psd_proc_t out[2];
    int n = psd_script_parse(buf, out, 2);

    CHECK(n == 2);
    CHECK(out[0].pid == 10);
    CHECK(out[1].pid == 11);
}

static void write_executable(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f != NULL)
    {
        fputs(content, f);
        fclose(f);
    }
    chmod(path, 0755);
}

static int file_contains(const char *path, const char *needle)
{
    char  buf[8192];
    FILE *f = fopen(path, "r");
    size_t n;

    if (f == NULL)
    {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

/* Number of times `needle` occurs in the file at `path`. */
static int file_count(const char *path, const char *needle)
{
    char        buf[8192];
    FILE       *f = fopen(path, "r");
    size_t      n;
    const char *s;
    int         count = 0;

    if (f == NULL)
    {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    for (s = strstr(buf, needle); s != NULL; s = strstr(s + 1, needle))
    {
        count++;
    }
    return count;
}

/* The response command is async (double-forked); wait briefly for its side
 * effect to land. */
static int wait_for_contains(const char *path, const char *needle, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms)
    {
        if (file_contains(path, needle))
        {
            return 1;
        }
        usleep(10000);
        waited += 10;
    }
    return file_contains(path, needle);
}

/* End-to-end through the installed script backend: a real poll subprocess feeds
 * psd_select_collect, and a real respond subprocess records the verb/id/extras
 * psDoom hands it -- exercising the documented contract over actual fork/exec. */
static void test_script_backend_roundtrip(void)
{
    const char *td = getenv("TMPDIR");
    char poll[512];
    char respond[512];
    char result[512];
    char respond_content[700];
    psd_proc_t out[16];
    int n;

    if (td == NULL || td[0] == '\0')
    {
        td = "/tmp";
    }
    snprintf(poll,    sizeof(poll),    "%s/psd_poll_%d.sh",   td, (int) getpid());
    snprintf(respond, sizeof(respond), "%s/psd_resp_%d.sh",   td, (int) getpid());
    snprintf(result,  sizeof(result),  "%s/psd_result_%d.txt", td, (int) getpid());
    unlink(result);

    write_executable(poll,
        "#!/bin/sh\n"
        "printf 'id=4123\\tlabel=web frontend\\tweight=734003200\\tload=140\\tparent=4000\\n'\n"
        "printf 'id=4124\\tweight=64\\tflags=1\\n'\n"
        "printf 'id=4000\\tlabel=supervisor\\tflags=1\\n'\n");

    snprintf(respond_content, sizeof(respond_content),
             "#!/bin/sh\necho \"$@\" >> '%s'\n", result);
    write_executable(respond, respond_content);

    proc_script_install(poll, respond, 1000);
    psd_select_init();   /* picks up uid via the script backend */

    n = psd_test_collect_wait(out, 16);
    CHECK(n == 3);                       /* all three poll entities survive */
    CHECK(index_of(out, n, 4123) >= 0);
    CHECK(index_of(out, n, 4124) >= 0);
    CHECK(index_of(out, n, 4000) >= 0);

    /* kill -> respond `delete 4124` (no label: that entity emitted none). */
    (void) proc_backend()->kill(4124);
    CHECK(wait_for_contains(result, "delete 4124", 3000));

    /* wound -> respond `post 4123 amount=4 label=web frontend`. */
    proc_backend()->renice(4123, 4);
    CHECK(wait_for_contains(result, "post 4123 amount=4", 3000));

    /* Checked actions in script mode: "now" is the latest poll. An id the poll
     * no longer lists gets no response at all; the id alone is the identity
     * (the contract's), so a label that differs does not block the action. */
    CHECK(psd_proc_kill_checked(4999, 0, "") == PSD_ACT_GONE);
    CHECK(psd_proc_renice_checked(4123, 0, "renamed", 4) == PSD_ACT_DONE);
    CHECK(psd_proc_kill_checked(4000, 0, "supervisor") == PSD_ACT_DONE);
    CHECK(wait_for_contains(result, "delete 4000", 3000));
    {
        int waited = 0;   /* the raw wound above, plus the checked one */
        while (file_count(result, "post 4123") < 2 && waited < 3000)
        {
            usleep(10000);
            waited += 10;
        }
        CHECK(file_count(result, "post 4123") == 2);
    }
    usleep(500000);   /* give any wrongly dispatched responder time to land */
    CHECK(!file_contains(result, "4999"));

    unlink(poll);
    unlink(respond);
    unlink(result);
}

/* ---- script backend hardening -------------------------------------------- */

static char g_self[1024];   /* absolute path of this binary (the fd probe) */

static unsigned long long test_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000ULL
         + (unsigned long long) ts.tv_nsec / 1000000ULL;
}

static const char *test_tmpdir(void)
{
    const char *td = getenv("TMPDIR");

    return (td != NULL && td[0] != '\0') ? td : "/tmp";
}

/* The poll never holds the caller: a slow poll (sleep 0.2) runs while list()
 * returns at once, saying "not yet" (-1), and the next call after the poll
 * ends gets its list. The game thread calls list() once a tic until then. */
static void test_script_poll_does_not_block_caller(void)
{
    psd_proc_t         out[4];
    unsigned long long t0;
    unsigned long long worst = 0;
    int                n;
    int                calls = 0;

    proc_script_install("sleep 0.2; printf 'id=5\\tlabel=slow\\n'", NULL, 2000);
    do
    {
        unsigned long long took;

        t0   = test_mono_ms();
        n    = proc_backend()->list(out, 4);
        took = test_mono_ms() - t0;
        worst = took > worst ? took : worst;
        calls++;
        if (n < 0)
        {
            usleep(5000);
        }
    } while (n < 0 && calls < 1000);
    CHECK(worst < 50);       /* no call held the caller for the poll's 200 ms */
    if (worst >= 50)
    {
        fprintf(stderr, "  list() held the caller %llu ms\n", worst);
    }
    CHECK(calls > 1);        /* the first call only started the poll */
    CHECK(n == 1 && out[0].pid == 5 && strcmp(out[0].name, "slow") == 0);

    /* While a poll runs, the selection layer keeps the last list (child
     * counts included) rather than an empty one. */
    proc_script_install("printf 'id=7\\nid=8\\tparent=7\\n'", NULL, 2000);
    CHECK(psd_test_collect_wait(out, 4) == 2);
    proc_script_install("sleep 0.2; printf 'id=9\\n'", NULL, 2000);
    CHECK(psd_select_collect(out, 4) < 0);
    CHECK(psd_select_child_count(7) == 1);
    CHECK(psd_test_collect_wait(out, 4) == 1 && out[0].pid == 9);
    CHECK(psd_select_child_count(7) == 0);
}

/* A poll still running when the backend is reinstalled belongs to the old
 * configuration: its result is dropped, never served as the new one's. */
static void test_script_poll_result_of_old_install_dropped(void)
{
    psd_proc_t out[4];
    int        n;

    proc_script_install("sleep 0.2; printf 'id=11\\n'", NULL, 2000);
    (void) proc_backend()->list(out, 4);         /* starts the old poll */
    proc_script_install("printf 'id=12\\n'", NULL, 2000);
    n = psd_test_list_wait(out, 4);
    CHECK(n == 1 && out[0].pid == 12);
}

/* proc_script_prefetch starts the next poll without waiting, at most one at
 * a time, so the next list() finds its result ready. */
static void test_script_prefetch(void)
{
    psd_proc_t         out[4];
    unsigned long long t0;
    unsigned long long took;
    int                n;

    proc_backend_set(NULL);
    proc_script_prefetch();                       /* not the script backend */
    CHECK(proc_backend()->list(out, 4) == 0);

    proc_script_install("sleep 0.2; printf 'id=31\\n'", NULL, 2000);
    t0 = test_mono_ms();
    proc_script_prefetch();
    proc_script_prefetch();                       /* one running: a no-op */
    took = test_mono_ms() - t0;
    CHECK(took < 50);
    usleep(1500000);
    n = proc_backend()->list(out, 4);             /* ready: no "not yet" */
    CHECK(n == 1 && out[0].pid == 31);
    proc_script_prefetch();                       /* consumed: starts another */
    CHECK(proc_backend()->list(out, 4) < 0);
    CHECK(psd_test_list_wait(out, 4) == 1);
}

/* How many of the ordinary signals (TERM, INT, HUP, PIPE, ALRM) this process
 * has blocked. */
static int sigmask_blocked_count(void)
{
    static const int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGALRM };
    sigset_t         cur;
    int              i;
    int              n = 0;

    if (sigprocmask(SIG_BLOCK, NULL, &cur) != 0)
    {
        return -1;
    }
    for (i = 0; i < (int) (sizeof(sigs) / sizeof(sigs[0])); i++)
    {
        n += sigismember(&cur, sigs[i]) == 1;
    }
    return n;
}

/* The poll and respond commands start with no signal blocked, whichever
 * thread forked them: a poll's detached helpers must still die of SIGTERM,
 * and a pipeline writer of SIGPIPE. */
static void test_script_children_signal_mask_clear(void)
{
    char       poll[1200];
    char       respond[1200];
    char       result[512];
    psd_proc_t out[4];
    int        n;

    if (g_self[0] == '\0' || strpbrk(g_self, " \t'") != NULL)
    {
        CHECK(0);
        return;
    }
    snprintf(result, sizeof(result), "%s/psd_mask_%d.txt", test_tmpdir(), (int) getpid());
    unlink(result);
    snprintf(poll, sizeof(poll), "'%s' --sigmask-probe", g_self);
    snprintf(respond, sizeof(respond), "%s --sigmask-probe-file %s", g_self, result);
    proc_script_install(poll, respond, 2000);
    n = psd_test_list_wait(out, 4);
    CHECK(n == 1 && strcmp(out[0].name, "mask0") == 0);
    if (n == 1 && strcmp(out[0].name, "mask0") != 0)
    {
        fprintf(stderr, "  poll child started with %s signals blocked\n", out[0].name);
    }
    (void) proc_backend()->kill(2);
    CHECK(wait_for_contains(result, "\n", 3000));
    CHECK(file_contains(result, "mask0\n"));
    unlink(result);
}

/* A poll that counts its runs: each prints id=<100 + run>. */
static void counting_poll(char *cmd, size_t cap, const char *counter)
{
    unlink(counter);
    snprintf(cmd, cap,
             "printf x >> '%s'; printf 'id=%%d\\n' $(( $(wc -c < '%s') + 100 ))",
             counter, counter);
}

/* A finished poll result does not keep: one older than the timeout plus a
 * second (the game was paused, or between levels) is dropped and polled
 * again, and proc_script_discard drops the one in flight. */
static void test_script_stale_result_dropped(void)
{
    char       counter[512];
    char       cmd[1400];
    psd_proc_t out[4];
    int        n;

    snprintf(counter, sizeof(counter), "%s/psd_count_%d.txt", test_tmpdir(), (int) getpid());
    counting_poll(cmd, sizeof(cmd), counter);
    proc_script_install(cmd, NULL, 200);
    (void) proc_backend()->list(out, 4);          /* run 1 starts */
    usleep(1700000);                              /* its result goes stale */
    CHECK(proc_backend()->list(out, 4) < 0);      /* dropped; run 2 starts */
    n = psd_test_list_wait(out, 4);
    CHECK(n == 1 && out[0].pid == 102);

    counting_poll(cmd, sizeof(cmd), counter);
    proc_script_install(cmd, NULL, 2000);
    (void) proc_backend()->list(out, 4);          /* run 1 starts */
    proc_script_discard();                        /* a new level */
    n = psd_test_list_wait(out, 4);
    CHECK(n == 1 && out[0].pid == 102);
    unlink(counter);
}

/* A poll still running when the game exits is stopped with its group: the
 * background child it started does not outlive the game. The child is a
 * `sleep 5`, so on a build that leaks it, it exits by itself; this test
 * never signals anything. The "game" is this binary in --exit-mid-poll
 * mode, run as a poll command (so the test itself execs nothing). */
static void test_script_exit_stops_running_poll(void)
{
    char       pidfile[512];
    char       poll[1200];
    psd_proc_t out[4];
    long       child = 0;
    long       shell = 0;
    int        alive = 1;
    int        waited;
    FILE      *f;

    if (g_self[0] == '\0' || strpbrk(g_self, " \t'") != NULL)
    {
        CHECK(0);
        return;
    }
    snprintf(pidfile, sizeof(pidfile), "%s/psd_exit_%d.txt", test_tmpdir(), (int) getpid());
    unlink(pidfile);
    snprintf(poll, sizeof(poll), "'%s' --exit-mid-poll '%s'", g_self, pidfile);
    proc_script_install(poll, NULL, 5000);
    CHECK(psd_test_list_wait(out, 4) == 0);   /* it exited 0, listing nothing */
    f = fopen(pidfile, "r");
    CHECK(f != NULL);
    if (f != NULL)
    {
        CHECK(fscanf(f, "%ld %ld", &child, &shell) == 2);
        fclose(f);
    }
    if (child <= 1)
    {
        CHECK(0);
        unlink(pidfile);
        return;
    }
    for (waited = 0; waited <= 2000; waited += 20)
    {
        pid_t g = getpgid((pid_t) child);

        if ((g < 0 && errno == ESRCH)
            || (g >= 0 && g != (pid_t) shell && g != getpgrp()))
        {
            alive = 0;
            break;
        }
        usleep(20000);
    }
    CHECK(!alive);
    if (alive)
    {
        fprintf(stderr, "  poll child %ld outlived the game's exit\n", child);
    }
    unlink(pidfile);
}

/* A responder named without a path is found along PATH before the fork (the
 * child execs a resolved path; it never searches PATH itself). */
static void test_script_respond_bare_name_resolved(void)
{
    char       script[512];
    char       result[512];
    char       respond[1200];
    char       content[700];
    psd_proc_t out[4];

    snprintf(script, sizeof(script), "%s/psd_bresp_%d.sh", test_tmpdir(), (int) getpid());
    snprintf(result, sizeof(result), "%s/psd_bres_%d.txt", test_tmpdir(), (int) getpid());
    unlink(result);
    snprintf(content, sizeof(content), "echo \"$*\" >> '%s'\n", result);
    write_executable(script, content);
    snprintf(respond, sizeof(respond), "sh %s", script);   /* sh: from PATH */
    proc_script_install("printf 'id=61\\n'", respond, 2000);
    CHECK(psd_test_list_wait(out, 4) == 1);
    CHECK(proc_backend()->kill(61) == 1);
    CHECK(wait_for_contains(result, "delete 61 mode=dryrun", 3000));

    /* A name found nowhere: nothing is dispatched, and nothing breaks. */
    proc_script_install("printf 'id=62\\n'", "psd-no-such-responder-xyz", 2000);
    CHECK(psd_test_list_wait(out, 4) == 1);
    CHECK(proc_backend()->kill(62) == 1);
    unlink(script);
    unlink(result);
}

/* A post or delete never holds the caller either: the responder (here one
 * that sleeps 0.3 s first) runs detached. */
static void test_script_respond_does_not_block_caller(void)
{
    char               respond[512];
    char               result[512];
    char               content[700];
    psd_proc_t         out[4];
    unsigned long long t0;
    unsigned long long took;

    snprintf(respond, sizeof(respond), "%s/psd_sresp_%d.sh", test_tmpdir(), (int) getpid());
    snprintf(result,  sizeof(result),  "%s/psd_sres_%d.txt", test_tmpdir(), (int) getpid());
    unlink(result);
    snprintf(content, sizeof(content),
             "#!/bin/sh\nsleep 0.3\necho \"$*\" >> '%s'\n", result);
    write_executable(respond, content);

    proc_script_install("printf 'id=21\\n'", respond, 2000);
    CHECK(psd_test_list_wait(out, 4) == 1);
    t0 = test_mono_ms();
    CHECK(proc_backend()->kill(21) == 1);
    proc_backend()->renice(21, 4);
    took = test_mono_ms() - t0;
    CHECK(took < 50);
    if (took >= 50)
    {
        fprintf(stderr, "  respond held the caller %llu ms\n", took);
    }
    CHECK(wait_for_contains(result, "delete 21", 3000));
    CHECK(wait_for_contains(result, "post 21 amount=4", 3000));
    unlink(respond);
    unlink(result);
}

/* An id outside 2..INT_MAX, or anything but plain decimal digits, drops the
 * line: it must never become an entity whose id wraps (4294967295 -> -1 would
 * turn a responder's `kill "$id"` for a `delete` into `kill -1`). */
static void test_script_parse_rejects_bad_ids(void)
{
    static const char *bad[] =
    {
        "id=4294967295\tlabel=wraps-to-minus-1\n",
        "id=4294967297\tlabel=wraps-to-1\n",
        "id=2147483648\tlabel=int-max-plus-1\n",
        "id=99999999999999999999999\tlabel=huge\n",
        "id=-1\tlabel=minus-one\n",
        "id=-2147483648\tlabel=int-min\n",
        "id=0\tlabel=zero\n",
        "id=1\tlabel=one\n",
        "id=12abc\tlabel=trailing-junk\n",
        "id=\tlabel=empty\n",
        "id= 42\tlabel=leading-space\n",
        "id=+42\tlabel=plus-sign\n",
        "id=0x10\tlabel=hex\n",
        "id=42\tid=junk\tlabel=last-value-invalid\n",
    };
    psd_proc_t out[2];
    size_t     i;
    int        n;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        n = psd_script_parse(bad[i], out, 2);
        CHECK(n == 0);
        if (n != 0)
        {
            fprintf(stderr, "  accepted id %d from: %s", out[0].pid, bad[i]);
        }
    }

    n = psd_script_parse("id=2147483647\tlabel=max\n", out, 2);
    CHECK(n == 1 && out[0].pid == INT_MAX);
    n = psd_script_parse("id=2\tlabel=min\n", out, 2);
    CHECK(n == 1 && out[0].pid == 2);
    n = psd_script_parse("id=0042\n", out, 2);
    CHECK(n == 1 && out[0].pid == 42);
    /* A parent that would wrap is dropped to "no parent", not kept wrapped. */
    n = psd_script_parse("id=7\tparent=4294967297\n", out, 2);
    CHECK(n == 1 && out[0].ppid == 0);
}

/* Defense in depth: even a raw backend call never hands an id <= 1 to the
 * response command (the recording responder only writes its argv). */
static void test_script_respond_refuses_reserved_ids(void)
{
    char poll[512];
    char respond[512];
    char result[512];
    char respond_content[700];
    psd_proc_t out[4];

    snprintf(poll,    sizeof(poll),    "%s/psd_rpoll_%d.sh", test_tmpdir(), (int) getpid());
    snprintf(respond, sizeof(respond), "%s/psd_rresp_%d.sh", test_tmpdir(), (int) getpid());
    snprintf(result,  sizeof(result),  "%s/psd_rres_%d.txt", test_tmpdir(), (int) getpid());
    unlink(result);
    write_executable(poll, "#!/bin/sh\nprintf 'id=4242\\tlabel=control\\n'\n");
    snprintf(respond_content, sizeof(respond_content),
             "#!/bin/sh\necho \"<$*>\" >> '%s'\n", result);
    write_executable(respond, respond_content);

    proc_script_install(poll, respond, 1000);
    (void) psd_test_list_wait(out, 4);

    CHECK(proc_backend()->kill(-1) == 0);
    CHECK(proc_backend()->kill(0) == 0);
    CHECK(proc_backend()->kill(1) == 0);
    proc_backend()->renice(-1, 4);
    proc_backend()->renice(1, 4);
    (void) proc_backend()->kill(4242);           /* control: dispatched */
    CHECK(wait_for_contains(result, "<delete 4242", 3000));
    usleep(300000);   /* give any wrongly dispatched responder time to land */
    CHECK(!file_contains(result, "<delete -1"));
    CHECK(!file_contains(result, "<delete 0"));
    CHECK(!file_contains(result, "<delete 1>"));
    CHECK(!file_contains(result, "<post -1"));
    CHECK(!file_contains(result, "<post 1 "));

    unlink(poll);
    unlink(respond);
    unlink(result);
}

/* A poll that overruns its timeout is stopped as a whole process group: a
 * background child it started does not outlive the timeout. The child is a
 * `sleep 5`, so on a build that leaks it, it exits by itself; this test never
 * signals anything. */
static void test_script_poll_timeout_leaves_no_child(void)
{
    char               pidfile[512];
    char               poll[1024];
    psd_proc_t         out[4];
    unsigned long long t0;
    unsigned long long elapsed;
    long               child = 0;
    long               shell = 0;
    int                alive = 1;
    int                waited;
    int                n;
    FILE              *f;

    snprintf(pidfile, sizeof(pidfile), "%s/psd_leak_%d.txt", test_tmpdir(), (int) getpid());
    unlink(pidfile);
    snprintf(poll, sizeof(poll),
             "sleep 5 & echo \"$! $$\" > '%s'; wait", pidfile);

    proc_script_install(poll, NULL, 300);
    t0 = test_mono_ms();
    n = psd_test_list_wait(out, 4);
    elapsed = test_mono_ms() - t0;
    CHECK(n == 0);
    CHECK(elapsed < 2000);   /* the game was not held past the timeout */

    f = fopen(pidfile, "r");
    CHECK(f != NULL);
    if (f != NULL)
    {
        CHECK(fscanf(f, "%ld %ld", &child, &shell) == 2);
        fclose(f);
    }
    CHECK(child > 1 && shell > 1);
    if (child <= 1)
    {
        unlink(pidfile);
        return;
    }

    /* Read-only check (getpgid sends nothing): the child is gone once no
     * process with its pid is in the poll's group or in ours. A pid reused by
     * an unrelated process sits in another group and also counts as gone. */
    for (waited = 0; waited <= 2000; waited += 20)
    {
        pid_t g = getpgid((pid_t) child);

        if ((g < 0 && errno == ESRCH)
            || (g >= 0 && g != (pid_t) shell && g != getpgrp()))
        {
            alive = 0;
            break;
        }
        usleep(20000);
    }
    CHECK(!alive);
    if (alive)
    {
        fprintf(stderr, "  poll child %ld outlived the poll timeout\n", child);
    }
    unlink(pidfile);
}

/* Descriptors open in the game (a file, a socket, a high-numbered one, none of
 * them close-on-exec) must not reach the poll or the respond command: each
 * child sees only stdin, stdout and stderr. The probe is this binary run with
 * --fd-probe / --fd-probe-file (see main). */
static void test_script_children_inherit_no_fds(void)
{
    char       poll[1200];
    char       respond[1200];
    char       result[512];
    psd_proc_t out[4];
    int        file_fd;
    int        sock_fd;
    int        high_fd;
    int        n;

    if (g_self[0] == '\0' || strpbrk(g_self, " \t'") != NULL)
    {
        fprintf(stderr, "FAIL: fd probe path unusable: '%s'\n", g_self);
        CHECK(0);
        return;
    }

    snprintf(result, sizeof(result), "%s/psd_fds_%d.txt", test_tmpdir(), (int) getpid());
    unlink(result);
    snprintf(poll, sizeof(poll), "'%s' --fd-probe", g_self);
    snprintf(respond, sizeof(respond), "%s --fd-probe-file %s", g_self, result);

    file_fd = open("/dev/null", O_RDONLY);
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    high_fd = dup2(file_fd, 200);
    CHECK(file_fd >= 3 && sock_fd >= 3 && high_fd == 200);

    proc_script_install(poll, respond, 2000);
    n = psd_test_list_wait(out, 4);
    CHECK(n == 1);
    if (n == 1)
    {
        CHECK(strcmp(out[0].name, "none") == 0);
        if (strcmp(out[0].name, "none") != 0)
        {
            fprintf(stderr, "  poll child inherited fds: %s\n", out[0].name);
        }
    }

    (void) proc_backend()->kill(2);
    CHECK(wait_for_contains(result, "\n", 3000));
    CHECK(file_contains(result, "none\n"));
    if (!file_contains(result, "none\n"))
    {
        fprintf(stderr, "  respond child inherited fds (see %s)\n", result);
    }
    else
    {
        unlink(result);
    }

    close(high_fd);
    close(sock_fd);
    close(file_fd);
}

/* A line too long to parse whole is dropped, never clipped: clipping an id
 * field that crosses the limit would make a shorter, different id. */
static void test_script_parse_drops_overlong_line(void)
{
    static char buf[4096];
    psd_proc_t  out[2];
    size_t      len;
    int         n;

    len = (size_t) snprintf(buf, sizeof(buf), "label=");
    memset(buf + len, 'x', 2040 - len);
    len = 2040;
    snprintf(buf + len, sizeof(buf) - len, "\tid=123456\nid=9\n");
    n = psd_script_parse(buf, out, 2);
    CHECK(n == 1 && out[0].pid == 9);
    if (n >= 1 && out[0].pid != 9)
    {
        fprintf(stderr, "  over-long line gave id %d\n", out[0].pid);
    }
}

/* A poll cut off by its timeout keeps only its complete lines: the line it
 * was writing (here `id=42`, on its way to 4242) is not parsed. */
static void test_script_poll_timeout_drops_partial_line(void)
{
    psd_proc_t out[4];
    int        n;

    proc_script_install("printf 'id=7\\nid=42'; sleep 2; printf '42\\n'", NULL, 300);
    n = psd_test_list_wait(out, 4);
    CHECK(n == 1);
    CHECK(n >= 1 && out[0].pid == 7);
    CHECK(index_of(out, n, 42) < 0);
}

/* A poll that times out without a line keeps the last good list: list()
 * says "no new list yet" (-1), so the game keeps its monsters, and polls
 * again. Only a streak of timeouts (Summoner truly not answering) turns
 * into "lists nothing", the spec's fail-safe. Here the first poll answers
 * and every later one hangs past its 300 ms timeout. */
static void test_script_poll_timeout_keeps_last_list(void)
{
    char               flag[512];
    char               poll[1200];
    psd_proc_t         out[4];
    unsigned long long t0;
    unsigned long long first_zero = 0;
    int                n;
    int                positive_after = 0;

    snprintf(flag, sizeof(flag), "%s/psd_answered_%d", test_tmpdir(), (int) getpid());
    unlink(flag);
    snprintf(poll, sizeof(poll),
             "if [ -e '%s' ]; then sleep 5; else : > '%s'; "
             "printf 'id=7\\nid=8\\n'; fi", flag, flag);
    proc_script_install(poll, NULL, 300);
    n = psd_test_list_wait(out, 4);
    CHECK(n == 2);

    t0 = test_mono_ms();
    while (test_mono_ms() - t0 < 6000)
    {
        n = proc_backend()->list(out, 4);
        if (n > 0)
        {
            positive_after++;
        }
        if (n == 0)
        {
            first_zero = test_mono_ms() - t0;
            break;
        }
        usleep(5000);
    }
    /* Never a new list while it hangs; "nothing" only after the streak
     * (5 timeouts of 300 ms), never on the first timeout. */
    CHECK(positive_after == 0);
    CHECK(first_zero >= 1200);
    CHECK(proc_script_status() != NULL);
    unlink(flag);
}

/* The commands run in a background process group, so they get /dev/null as
 * stdin rather than the game's (a terminal read would stop them with
 * SIGTTIN). Here the game's stdin is a pipe nobody writes: `cat` would block
 * on it until the timeout. */
static void test_script_children_stdin_is_devnull(void)
{
    int                p[2];
    int                saved;
    unsigned long long t0;
    unsigned long long elapsed;
    psd_proc_t         out[4];
    int                n;

    saved = dup(STDIN_FILENO);
    CHECK(saved >= 0 && pipe(p) == 0);
    if (saved < 0)
    {
        return;
    }
    dup2(p[0], STDIN_FILENO);

    proc_script_install("cat >/dev/null; printf 'id=3\\n'", NULL, 1500);
    t0 = test_mono_ms();
    n = psd_test_list_wait(out, 4);
    elapsed = test_mono_ms() - t0;

    dup2(saved, STDIN_FILENO);
    close(saved);
    close(p[0]);
    close(p[1]);

    CHECK(n == 1 && out[0].pid == 3);
    CHECK(elapsed < 1000);
}

/* A descriptor numbered above the current soft RLIMIT_NOFILE (the limit was
 * lowered after it was opened) is not inherited either. */
static void test_script_children_inherit_no_fds_above_limit(void)
{
    struct rlimit saved;
    struct rlimit lowered;
    char          poll[1200];
    psd_proc_t    out[4];
    int           high_fd;
    int           n;

    if (g_self[0] == '\0' || strpbrk(g_self, " \t'") != NULL
        || getrlimit(RLIMIT_NOFILE, &saved) != 0)
    {
        CHECK(0);
        return;
    }
    high_fd = dup2(STDERR_FILENO, 300);
    CHECK(high_fd == 300);
    lowered         = saved;
    lowered.rlim_cur = 200;
    CHECK(setrlimit(RLIMIT_NOFILE, &lowered) == 0);

    snprintf(poll, sizeof(poll), "'%s' --fd-probe", g_self);
    proc_script_install(poll, NULL, 2000);
    n = psd_test_list_wait(out, 4);

    CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    close(high_fd);

    CHECK(n == 1);
    if (n == 1)
    {
        CHECK(strcmp(out[0].name, "none") == 0);
        if (strcmp(out[0].name, "none") != 0)
        {
            fprintf(stderr, "  poll child inherited fds: %s\n", out[0].name);
        }
    }
}

/* The fd probe: list this process's open descriptors above stderr, up to at
 * least 1024 (an fd can sit above a lowered soft limit). */
static void fd_probe_list(char *buf, size_t cap)
{
    int    limit = getdtablesize() > 1024 ? getdtablesize() : 1024;
    int    fd;
    size_t len = 0;

    buf[0] = '\0';
    for (fd = 3; fd < limit; fd++)
    {
        if (fcntl(fd, F_GETFD) != -1 && len + 12 < cap)
        {
            len += (size_t) snprintf(buf + len, cap - len, "%s%d",
                                     len == 0 ? "" : ",", fd);
        }
    }
    if (len == 0)
    {
        snprintf(buf, cap, "none");
    }
}

/* ---- pid-reuse guard: act only on the process the monster was spawned for -- */

static psd_proc_t mkident(int pid, unsigned long long start, const char *name)
{
    psd_proc_t p = mkproc(pid, 1, 501, 0, 0, name);
    p.start_token = start;
    return p;
}

/* Install the fake and list one snapshot, as a sync would before the player
 * wounds or kills the monster spawned from it. */
static void list_one(const psd_proc_t *p)
{
    psd_proc_t out[4];

    proc_fake_install(501);
    proc_fake_push_snapshot(p, 1);
    psd_select_init();
    (void) psd_test_collect_wait(out, 4);
}

/* The normal path still acts: the pid is still the listed process. */
static void test_guard_same_process_acts(void)
{
    psd_proc_t listed = mkident(300, 1000001ULL, "firefox");

    list_one(&listed);
    CHECK(psd_proc_renice_checked(300, 1000001ULL, "firefox", 4) == PSD_ACT_DONE);
    CHECK(proc_fake_renice_count() == 1);
    CHECK(proc_fake_last_renice_pid() == 300);
    CHECK(proc_fake_last_renice_delta() == 4);
    CHECK(psd_proc_kill_checked(300, 1000001ULL, "firefox") == PSD_ACT_DONE);
    CHECK(proc_fake_kill_count() == 1);
    CHECK(proc_fake_last_kill_pid() == 300);
}

/* The listed process exits and the OS hands its pid to an unrelated process
 * before the player acts: no renice, no kill, and a distinct result. */
static void test_guard_reused_pid_refused(void)
{
    psd_proc_t listed  = mkident(300, 1000001ULL, "firefox");
    psd_proc_t reused  = mkident(300, 2000002ULL, "bash");     /* new process */
    psd_proc_t samenam = mkident(300, 2000002ULL, "firefox");  /* relaunch    */
    psd_proc_t execd   = mkident(300, 1000001ULL, "bash");     /* name moved  */

    list_one(&listed);
    proc_fake_set_live(&reused, 1);
    CHECK(psd_proc_renice_checked(300, 1000001ULL, "firefox", 4) == PSD_ACT_REUSED);
    CHECK(psd_proc_kill_checked(300, 1000001ULL, "firefox") == PSD_ACT_REUSED);

    /* Same name, new start time: still a different process. */
    proc_fake_set_live(&samenam, 1);
    CHECK(psd_proc_renice_checked(300, 1000001ULL, "firefox", 4) == PSD_ACT_REUSED);
    CHECK(psd_proc_kill_checked(300, 1000001ULL, "firefox") == PSD_ACT_REUSED);

    /* Same start time, different name: not the process the label shows. */
    proc_fake_set_live(&execd, 1);
    CHECK(psd_proc_renice_checked(300, 1000001ULL, "firefox", 4) == PSD_ACT_REUSED);
    CHECK(psd_proc_kill_checked(300, 1000001ULL, "firefox") == PSD_ACT_REUSED);

    CHECK(proc_fake_renice_count() == 0);
    CHECK(proc_fake_kill_count() == 0);
}

/* The listed process exits and nothing has its pid yet. */
static void test_guard_vanished_pid_refused(void)
{
    psd_proc_t listed = mkident(300, 1000001ULL, "firefox");

    list_one(&listed);
    proc_fake_set_live(NULL, 0);
    CHECK(psd_proc_renice_checked(300, 1000001ULL, "firefox", 4) == PSD_ACT_GONE);
    CHECK(psd_proc_kill_checked(300, 1000001ULL, "firefox") == PSD_ACT_GONE);
    CHECK(proc_fake_renice_count() == 0);
    CHECK(proc_fake_kill_count() == 0);
}

/* pid <= 1 is refused outright, as before. */
static void test_guard_reserved_pids_refused(void)
{
    psd_proc_t listed = mkident(300, 1000001ULL, "firefox");

    list_one(&listed);
    CHECK(psd_proc_renice_checked(1, 0, "launchd", 4) == PSD_ACT_REFUSED);
    CHECK(psd_proc_kill_checked(0, 0, "kernel_task") == PSD_ACT_REFUSED);
    CHECK(proc_fake_renice_count() == 0);
    CHECK(proc_fake_kill_count() == 0);
}

/* A monster stores 15 bytes of the name, so identity compares that much. */
static void test_identity_name_truncation(void)
{
    psd_proc_t cur = mkident(300, 7ULL, "com.example.verylongname");

    CHECK(psd_proc_same(&cur, 300, 7ULL, "com.example.ver"));   /* 15 bytes */
    CHECK(!psd_proc_same(&cur, 300, 7ULL, "com.example.ve"));   /* shorter  */
    CHECK(!psd_proc_same(&cur, 301, 7ULL, "com.example.ver"));
    CHECK(!psd_proc_same(&cur, 300, 8ULL, "com.example.ver"));
    CHECK(!psd_proc_same(NULL, 300, 7ULL, "com.example.ver"));
}

/* Without a start token (script entities) the id alone is the identity, as the
 * script contract defines; with one, the name must match too. */
static void test_identity_without_start_token(void)
{
    psd_proc_t entity = mkident(4123, 0ULL, "web frontend");

    CHECK(psd_proc_same(&entity, 4123, 0ULL, "web frontend"));
    CHECK(psd_proc_same(&entity, 4123, 0ULL, "relabelled"));
    CHECK(!psd_proc_same(&entity, 4124, 0ULL, "web frontend"));
    CHECK(!psd_proc_same(&entity, 4123, 5ULL, "web frontend"));
}

/* Process monsters are ethereal: a save writes ordinary mobjs as is, writes a
 * map-placed monster that carried a process identity as a plain map monster,
 * and never writes a monster spawned on demand for the process list. */
static void test_save_disposition(void)
{
    /* Ordinary mobjs, map-placed or not (a teleport fog, a missile). */
    CHECK(psd_save_disposition(0, 1) == PSD_SAVE_KEEP);
    CHECK(psd_save_disposition(0, 0) == PSD_SAVE_KEEP);

    /* Spawned on demand: a process monster, a swarm soul, a gone monster. */
    CHECK(psd_save_disposition(4123, 0) == PSD_SAVE_SKIP);
    CHECK(psd_save_disposition(2, 0) == PSD_SAVE_SKIP);
    CHECK(psd_save_disposition(PSD_SWARM_PID, 0) == PSD_SAVE_SKIP);
    CHECK(psd_save_disposition(PSD_GONE_PID, 0) == PSD_SAVE_SKIP);

    /* Map-placed with a process identity: kept, identity stripped. */
    CHECK(psd_save_disposition(4123, 1) == PSD_SAVE_STRIP);
    CHECK(psd_save_disposition(PSD_GONE_PID, 1) == PSD_SAVE_STRIP);
    CHECK(psd_save_disposition(PSD_SWARM_PID, 1) == PSD_SAVE_STRIP);
}

/* Leaving the process list, by exit or by an identity mismatch, teleports a
 * monster out; a monster killed in the game dies normally. */
static void test_end_action(void)
{
    CHECK(psd_end_action(PSD_END_LEFT_LIST) == PSD_END_TELEPORT_OUT);
    CHECK(psd_end_action(PSD_END_IDENTITY_LOST) == PSD_END_TELEPORT_OUT);
    CHECK(psd_end_action(PSD_END_KILLED) == PSD_END_DIE);
}

/* The sync's routing: a live monster that left the list or lost its identity
 * ends (and teleports out); a corpse always counts as killed, even when its
 * process has gone or a checked kill found the pid reused, so a killed
 * monster never teleports out; anything still listed, or not a process
 * monster, stays. */
static void test_end_reason(void)
{
    /* Alive. */
    CHECK(psd_end_reason(4123, 0, 1) == PSD_END_STAYS);
    CHECK(psd_end_reason(4123, 0, 0) == PSD_END_LEFT_LIST);
    CHECK(psd_end_reason(PSD_GONE_PID, 0, 0) == PSD_END_IDENTITY_LOST);
    CHECK(psd_end_action(psd_end_reason(4123, 0, 0)) == PSD_END_TELEPORT_OUT);

    /* Dead: still listed stays (it blocks a respawn); otherwise killed. */
    CHECK(psd_end_reason(4123, 1, 1) == PSD_END_STAYS);
    CHECK(psd_end_reason(4123, 1, 0) == PSD_END_KILLED);
    CHECK(psd_end_reason(PSD_GONE_PID, 1, 0) == PSD_END_KILLED);
    CHECK(psd_end_action(psd_end_reason(4123, 1, 0)) == PSD_END_DIE);
    CHECK(psd_end_action(psd_end_reason(PSD_GONE_PID, 1, 0)) == PSD_END_DIE);

    /* Not process monsters: ordinary, swarm souls, kept corpses. */
    CHECK(psd_end_reason(0, 0, 0) == PSD_END_STAYS);
    CHECK(psd_end_reason(PSD_SWARM_PID, 0, 0) == PSD_END_STAYS);
    CHECK(psd_end_reason(PSD_CORPSE_PID, 1, 0) == PSD_END_STAYS);

    /* A kept corpse was spawned on demand, so a save skips it too. */
    CHECK(psd_save_disposition(PSD_CORPSE_PID, 0) == PSD_SAVE_SKIP);
}

/* Older saves wrote process monsters as plain mobjs; a load drops exactly
 * those: on demand, a kill-counting type, the flag cleared, not a player. */
static void test_legacy_process_monster(void)
{
    CHECK(psd_legacy_process_monster(0, 0, 1, 0));     /* old process monster */
    CHECK(!psd_legacy_process_monster(0, 1, 1, 1));    /* map monster          */
    CHECK(!psd_legacy_process_monster(0, 1, 1, 0));    /* map-placed           */
    CHECK(!psd_legacy_process_monster(0, 0, 1, 1));    /* engine-spawned (brain spit) */
    CHECK(!psd_legacy_process_monster(0, 0, 0, 0));    /* fog, missile, pickup */
    CHECK(!psd_legacy_process_monster(1, 0, 0, 0));    /* a player             */
}

int main(int argc, char **argv)
{
    /* fd probe mode: run as the poll command (prints one entity whose label
     * lists the inherited fds) or as the respond command (appends the list to
     * a file). The list is taken before anything else is opened. */
    if (argc >= 2 && strcmp(argv[1], "--fd-probe") == 0)
    {
        char fds[256];

        fd_probe_list(fds, sizeof(fds));
        printf("id=2\tlabel=%s\n", fds);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--fd-probe-file") == 0)
    {
        char  fds[256];
        FILE *f;

        fd_probe_list(fds, sizeof(fds));
        f = fopen(argv[2], "a");
        if (f != NULL)
        {
            fprintf(f, "%s\n", fds);
            fclose(f);
        }
        return 0;
    }
    /* signal-mask probe: as the poll command, one entity whose label counts
     * the ordinary signals it was started with blocked ("mask0": none). */
    if (argc >= 2 && strcmp(argv[1], "--sigmask-probe") == 0)
    {
        printf("id=2\tlabel=mask%d\n", sigmask_blocked_count());
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--sigmask-probe-file") == 0)
    {
        FILE *f = fopen(argv[2], "a");

        if (f != NULL)
        {
            fprintf(f, "mask%d\n", sigmask_blocked_count());
            fclose(f);
        }
        return 0;
    }
    /* quit-mid-poll mode: start a poll that leaves a background child, wait
     * until it has written its pids, then exit as the game would. */
    if (argc >= 3 && strcmp(argv[1], "--exit-mid-poll") == 0)
    {
        char       poll[1024];
        psd_proc_t out[4];
        int        waited;

        snprintf(poll, sizeof(poll),
                 "sleep 5 & echo \"$! $$\" > '%s.tmp' && mv '%s.tmp' '%s'; wait",
                 argv[2], argv[2], argv[2]);
        proc_script_install(poll, NULL, 10000);
        (void) proc_backend()->list(out, 4);
        for (waited = 0; waited < 3000 && access(argv[2], F_OK) != 0; waited += 10)
        {
            usleep(10000);
        }
        exit(0);
    }
    if (argc >= 1 && realpath(argv[0], g_self) == NULL)
    {
        g_self[0] = '\0';
    }

    test_collect_and_action_log();
    test_child_count();
    test_child_count_reported();
    test_script_parse();
    test_script_parse_truncates();
    test_script_backend_roundtrip();
    test_script_parse_rejects_bad_ids();
    test_script_respond_refuses_reserved_ids();
    test_script_poll_timeout_leaves_no_child();
    test_script_children_inherit_no_fds();
    test_script_parse_drops_overlong_line();
    test_script_poll_timeout_drops_partial_line();
    test_script_poll_timeout_keeps_last_list();
    test_script_children_stdin_is_devnull();
    test_script_children_inherit_no_fds_above_limit();
    test_script_poll_does_not_block_caller();
    test_script_poll_result_of_old_install_dropped();
    test_script_respond_does_not_block_caller();
    test_script_prefetch();
    test_script_children_signal_mask_clear();
    test_script_stale_result_dropped();
    test_script_exit_stops_running_poll();
    test_script_respond_bare_name_resolved();
    test_guard_same_process_acts();
    test_guard_reused_pid_refused();
    test_guard_vanished_pid_refused();
    test_guard_reserved_pids_refused();
    test_identity_name_truncation();
    test_identity_without_start_token();
    test_save_disposition();
    test_end_action();
    test_end_reason();
    test_legacy_process_monster();
    script_grade_tests(&checks_run, &checks_failed);
    script_request_tests(&checks_run, &checks_failed);
    poll_limit_tests(&checks_run, &checks_failed);
    summoner_bundle_tests(&checks_run, &checks_failed);
    summoner_e2e_tests(&checks_run, &checks_failed);
    iwad_select_tests(&checks_run, &checks_failed);

    printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
