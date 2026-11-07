/*
 * psDoom -- proc_select / backend host tests.
 *
 * Engine-free: links proc_select, proc_backend, the fake and script backends
 * and, on macOS, the native backend for READ-ONLY listing/lookup checks (it
 * never kills anything, and its renice reaches only this binary's
 * getpriority/setpriority seam, never the OS), so the triage policy, the
 * pid-reuse guard and the backend plumbing are exercised deterministically
 * off-machine (no Doom engine). The script tests spawn only their own poll and
 * respond commands and never signal anything. proc_select calls one
 * option getter (psdoom_all_users_enabled); we stub it here rather than link
 * the engine-coupled options module.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "proc_select.h"
#include "proc_backend.h"
#include "proc_fake.h"
#include "proc_script.h"
#ifdef __APPLE__
#include "proc_macos.h"   /* read-only listing/lookup check */
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h> /* getpriority / setpriority (link-time seam) */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>     /* waitpid (reap our own zombie child) */

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

/* ---- script backend: contract parser + a real subprocess round-trip ------ */

/* The poll parser (docs/script-backend.md): tab-separated key=value, comments
 * and blanks skipped, unknown keys ignored, id>1 required, last-wins, spaces
 * preserved in values. */
static void test_script_parse(void)
{
    const char *buf =
        "#!psdoom v1\n"
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
    stub_all_users = 0;
    psd_select_init();   /* picks up uid via the script backend */

    n = psd_select_collect(out, 16);
    CHECK(n == 3);                       /* all three poll entities survive */
    CHECK(index_of(out, n, 4123) >= 0);
    CHECK(index_of(out, n, 4124) >= 0);
    CHECK(index_of(out, n, 4000) >= 0);

    /* kill -> respond `kill 4124` (no label: that entity emitted none). */
    (void) proc_backend()->kill(4124);
    CHECK(wait_for_contains(result, "kill 4124", 3000));

    /* wound -> respond `wound 4123 amount=4 label=web frontend`. */
    proc_backend()->renice(4123, 4);
    CHECK(wait_for_contains(result, "wound 4123 amount=4", 3000));

    /* Checked actions in script mode: "now" is the latest poll. An id the poll
     * no longer lists gets no response at all; the id alone is the identity
     * (the contract's), so a label that differs does not block the action. */
    CHECK(psd_proc_kill_checked(4999, 0, "") == PSD_ACT_GONE);
    CHECK(psd_proc_renice_checked(4123, 0, "renamed", 4) == PSD_ACT_DONE);
    CHECK(psd_proc_kill_checked(4000, 0, "supervisor") == PSD_ACT_DONE);
    CHECK(wait_for_contains(result, "kill 4000", 3000));
    {
        int waited = 0;   /* the raw wound above, plus the checked one */
        while (file_count(result, "wound 4123") < 2 && waited < 3000)
        {
            usleep(10000);
            waited += 10;
        }
        CHECK(file_count(result, "wound 4123") == 2);
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

/* An id outside 2..INT_MAX, or anything but plain decimal digits, drops the
 * line: it must never become an entity whose id wraps (4294967295 -> -1 would
 * turn a responder's `kill "$id"` into `kill -1`). */
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
    (void) proc_backend()->list(out, 4);

    CHECK(proc_backend()->kill(-1) == 0);
    CHECK(proc_backend()->kill(0) == 0);
    CHECK(proc_backend()->kill(1) == 0);
    proc_backend()->renice(-1, 4);
    proc_backend()->renice(1, 4);
    (void) proc_backend()->kill(4242);           /* control: dispatched */
    CHECK(wait_for_contains(result, "<kill 4242", 3000));
    usleep(300000);   /* give any wrongly dispatched responder time to land */
    CHECK(!file_contains(result, "<kill -1"));
    CHECK(!file_contains(result, "<kill 0"));
    CHECK(!file_contains(result, "<kill 1>"));
    CHECK(!file_contains(result, "<wound -1"));
    CHECK(!file_contains(result, "<wound 1 "));

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
    n = proc_backend()->list(out, 4);
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
    n = proc_backend()->list(out, 4);
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
    n = proc_backend()->list(out, 4);
    CHECK(n == 1);
    CHECK(n >= 1 && out[0].pid == 7);
    CHECK(index_of(out, n, 42) < 0);
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
    n = proc_backend()->list(out, 4);
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
    n = proc_backend()->list(out, 4);

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
    stub_all_users = 0;
    proc_fake_push_snapshot(p, 1);
    psd_select_init();
    (void) psd_select_collect(out, 4);
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

#ifdef __APPLE__
/* The native backend, exercised READ-ONLY: listing and lookup capture a start
 * time that agrees between the two. Never renices or kills anything. */
static void test_macos_start_token_readonly(void)
{
    static psd_proc_t all[8192];
    psd_proc_t        cur;
    int               self = (int) getpid();
    int               n    = proc_macos_list(all, 8192);
    int               i    = index_of(all, n, self);

    CHECK(n > 0);
    CHECK(i >= 0);
    CHECK(proc_macos_lookup(self, &cur) == 1);
    CHECK(cur.start_token != 0);
    if (i >= 0)
    {
        CHECK(all[i].start_token == cur.start_token);
        CHECK(psd_proc_same(&cur, self, all[i].start_token, all[i].name));
    }
    CHECK(proc_macos_lookup((int) getppid(), &cur) == 1);
    CHECK(proc_macos_lookup(999999, &cur) == 0);   /* above macOS PID_MAX */
    CHECK(proc_macos_lookup(-1, &cur) == 0);
}

/* list() and lookup() agree about an unreaped zombie (our own child, which
 * exits at once and is reaped here; nothing is signalled): it is still the
 * same process until reaped, then it is gone. */
static void test_macos_zombie_readonly(void)
{
    static psd_proc_t all[8192];
    psd_proc_t        cur;
    pid_t             child = fork();
    int               n;
    int               i;

    if (child == 0)
    {
        _exit(0);
    }
    CHECK(child > 0);
    if (child <= 0)
    {
        return;
    }
    usleep(200000);   /* let it exit; we have not reaped it, so it is a zombie */

    n = proc_macos_list(all, 8192);
    i = index_of(all, n, (int) child);
    CHECK(i >= 0);
    CHECK(proc_macos_lookup((int) child, &cur) == 1);
    if (i >= 0)
    {
        CHECK(psd_proc_same(&cur, (int) child, all[i].start_token, all[i].name));
    }

    waitpid(child, NULL, 0);
    CHECK(proc_macos_lookup((int) child, &cur) == 0);
}

/* Link-time seam: these definitions take the place of libSystem's
 * getpriority/setpriority for this test binary only, so the native renice is
 * observed without changing the priority of any real process. Nothing else in
 * this binary calls them. */
static int   seam_setpriority_calls;
static id_t  seam_setpriority_who;

int getpriority(int which, id_t who)
{
    (void) which;
    (void) who;
    errno = 0;
    return 0;
}

int setpriority(int which, id_t who, int prio)
{
    (void) which;
    (void) prio;
    seam_setpriority_calls++;
    seam_setpriority_who = who;
    return 0;
}

extern const proc_backend_t proc_macos_backend;

/* The native renice refuses pid <= 1, as the native kill does, both raw and
 * through the checked wrapper. */
static void test_macos_renice_refuses_reserved(void)
{
    static const int bad[] = { 1, 0, -1, INT_MIN };
    size_t           i;

    /* Control first: a valid pid (our own, delta 0) must reach the seam. If
     * it does not, the seam is not in effect, and renicing pid 1 below could
     * reach the OS, so stop here. */
    seam_setpriority_calls = 0;
    proc_macos_renice((int) getpid(), 0);
    CHECK(seam_setpriority_calls == 1);
    CHECK(seam_setpriority_who == (id_t) getpid());
    if (seam_setpriority_calls != 1)
    {
        fprintf(stderr, "  setpriority seam not in effect; skipping pid <= 1 calls\n");
        return;
    }

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        seam_setpriority_calls = 0;
        proc_macos_renice(bad[i], 4);
        CHECK(seam_setpriority_calls == 0);
        if (seam_setpriority_calls != 0)
        {
            fprintf(stderr, "  native renice reached setpriority for pid %d\n", bad[i]);
        }
    }

    proc_backend_set(&proc_macos_backend);
    seam_setpriority_calls = 0;
    CHECK(psd_proc_renice_checked(1, 0ULL, "launchd", 4) == PSD_ACT_REFUSED);
    CHECK(psd_proc_renice_checked(0, 0ULL, "kernel_task", 4) == PSD_ACT_REFUSED);
    CHECK(psd_proc_renice_checked(-1, 0ULL, "", 4) == PSD_ACT_REFUSED);
    CHECK(seam_setpriority_calls == 0);
    proc_backend_set(NULL);
}
#endif

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
    if (argc >= 1 && realpath(argv[0], g_self) == NULL)
    {
        g_self[0] = '\0';
    }

    test_triage_filters_and_ranking();
    test_triage_all_users();
    test_triage_truncation();
    test_collect_and_action_log();
    test_child_count();
    test_script_parse();
    test_script_parse_truncates();
    test_script_backend_roundtrip();
    test_script_parse_rejects_bad_ids();
    test_script_respond_refuses_reserved_ids();
    test_script_poll_timeout_leaves_no_child();
    test_script_children_inherit_no_fds();
    test_script_parse_drops_overlong_line();
    test_script_poll_timeout_drops_partial_line();
    test_script_children_stdin_is_devnull();
    test_script_children_inherit_no_fds_above_limit();
    test_guard_same_process_acts();
    test_guard_reused_pid_refused();
    test_guard_vanished_pid_refused();
    test_guard_reserved_pids_refused();
    test_identity_name_truncation();
    test_identity_without_start_token();
#ifdef __APPLE__
    test_macos_start_token_readonly();
    test_macos_zombie_readonly();
    test_macos_renice_refuses_reserved();
#endif

    printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
