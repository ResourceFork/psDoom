/*
 * psDoom -- script contract host tests: the `grade` field, the grade ->
 * drawable-tier mapping (including the shareware degrade), and the response
 * verbs (post for a wound, delete for a kill).
 *
 * The respond checks run only this test's own recorder script under the
 * build-tree scratch directory (PSD_E2E_SCRATCH); nothing is signalled.
 * Registered from proc_select_test.c's main (script_grade_tests()).
 */

#include "proc_backend.h"
#include "proc_script.h"
#include "proc_select.h"
#include "psd_test_wait.h"
#include "psdoom_grade.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PSD_E2E_SCRATCH
#error "PSD_E2E_SCRATCH must be defined (CMakeLists.txt)"
#endif

static int *gr_run;
static int *gr_failed;

#define GR_CHECK(cond)                                                       \
    do {                                                                     \
        (*gr_run)++;                                                         \
        if (!(cond)) {                                                       \
            (*gr_failed)++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* ---- grade parsing ------------------------------------------------------- */

static int parse_one(const char *buf, psd_proc_t *p)
{
    return psd_script_parse(buf, p, 1);
}

static void test_grade_parse(void)
{
    static const struct { const char *val; double want; } ok[] = {
        { "0", 0.0 }, { "1", 1.0 }, { "0.5", 0.5 }, { "0.25", 0.25 },
        { "1.5", 1.0 }, { "-0.2", 0.0 }, { "1e-3", 0.001 }, { "+0.75", 0.75 },
    };
    static const char *const bad[] = {
        "", "abc", "0.5x", " 0.5", "nan", "NaN", "inf", "-inf", "1e999",
        "0x1p-1", "0X0.8",
    };
    psd_proc_t p;
    char       buf[128];
    size_t     i;

    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
    {
        snprintf(buf, sizeof(buf), "id=7\tgrade=%s\n", ok[i].val);
        GR_CHECK(parse_one(buf, &p) == 1);
        GR_CHECK(p.has_grade == 1);
        GR_CHECK(fabs(p.grade - ok[i].want) < 1e-9);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        snprintf(buf, sizeof(buf), "id=7\tgrade=%s\tweight=5\n", bad[i]);
        GR_CHECK(parse_one(buf, &p) == 1);              /* entity kept */
        GR_CHECK(p.has_grade == 0);                     /* grade ignored */
        GR_CHECK(p.footprint == 5);
    }
    /* The last valid grade wins; a later malformed one does not erase it. */
    GR_CHECK(parse_one("id=7\tgrade=0.2\tgrade=0.9\n", &p) == 1);
    GR_CHECK(fabs(p.grade - 0.9) < 1e-9);
    GR_CHECK(parse_one("id=7\tgrade=0.2\tgrade=junk\n", &p) == 1);
    GR_CHECK(p.has_grade == 1 && fabs(p.grade - 0.2) < 1e-9);
    /* No grade: weight/load decide. A comment line is still a comment. */
    GR_CHECK(parse_one("id=7\tweight=5\n", &p) == 1);
    GR_CHECK(p.has_grade == 0);
    GR_CHECK(parse_one("# id=7\tgrade=1\n", &p) == 0);

    /* grade sits beside every other field and rule. */
    GR_CHECK(parse_one("id=9\tparent=4\tweight=64\tload=30"
                       "\tflags=1\tlabel=x y\n", &p) == 1);
    GR_CHECK(p.pid == 9 && p.ppid == 4 && p.footprint == 64);
    GR_CHECK(p.cpu_percent == 30 && p.is_daemon == 1);
    GR_CHECK(strcmp(p.name, "x y") == 0 && p.has_grade == 0);
    GR_CHECK(parse_one("id=1\tgrade=1\n", &p) == 0);   /* id 1 reserved */
}

/* ---- grade -> tier ------------------------------------------------------ */

static void test_grade_tier(void)
{
    /* tier = min(floor(grade * n), n - 1) */
    GR_CHECK(psd_grade_tier(0.0, 7) == 0);
    GR_CHECK(psd_grade_tier(1.0, 7) == 6);
    GR_CHECK(psd_grade_tier(0.9999, 7) == 6);
    GR_CHECK(psd_grade_tier(1.0 / 7.0 - 1e-9, 7) == 0);
    GR_CHECK(psd_grade_tier(1.0 / 7.0 + 1e-9, 7) == 1);
    GR_CHECK(psd_grade_tier(0.5, 7) == 3);
    GR_CHECK(psd_grade_tier(0.5, 2) == 1);
    GR_CHECK(psd_grade_tier(0.49, 2) == 0);
    GR_CHECK(psd_grade_tier(-1.0, 7) == 0);
    GR_CHECK(psd_grade_tier(2.0, 7) == 6);
    GR_CHECK(psd_grade_tier(NAN, 7) == 0);
    GR_CHECK(psd_grade_tier(1.0, 1) == 0);
    GR_CHECK(psd_grade_tier(1.0, 0) == 0);
}

static void test_grade_pick_degrade(void)
{
    /* Ladder: Zombieman, Shotgun Guy, Imp, Pinky, Hell Knight, Baron, Cyber. */
    static const int full[7]      = { 1, 1, 1, 1, 1, 1, 1 };
    static const int freedoom[7]  = { 1, 1, 1, 1, 0, 1, 1 }; /* no Knight   */
    static const int shareware[7] = { 1, 1, 1, 1, 0, 1, 0 }; /* + no Cyber  */
    static const int none[7]      = { 0, 0, 0, 0, 0, 0, 0 };

    GR_CHECK(psd_grade_pick(1.0, full, 7) == 6);
    GR_CHECK(psd_grade_pick(0.0, full, 7) == 0);
    GR_CHECK(psd_grade_pick(0.6, full, 7) == 4);          /* Hell Knight */
    /* 1.0 is the top *drawable* monster. */
    GR_CHECK(psd_grade_pick(1.0, freedoom, 7) == 6);      /* Cyberdemon  */
    GR_CHECK(psd_grade_pick(1.0, shareware, 7) == 5);     /* Baron       */
    /* Shareware: 5 drawable tiers, so 0.8 -> 4th of 0..4 -> Baron (5). */
    GR_CHECK(psd_grade_pick(0.8, shareware, 7) == 5);
    GR_CHECK(psd_grade_pick(0.79, shareware, 7) == 3);    /* Pinky       */
    GR_CHECK(psd_grade_pick(0.0, shareware, 7) == 0);
    /* Never an undrawable tier. */
    {
        int i;
        for (i = 0; i <= 100; i++)
        {
            int t = psd_grade_pick(i / 100.0, shareware, 7);
            GR_CHECK(t == 0 || shareware[t]);
        }
    }
    /* Tier 0 always counts as drawable. */
    GR_CHECK(psd_grade_pick(1.0, none, 7) == 0);
    GR_CHECK(psd_grade_pick(1.0, NULL, 7) == 0);
}

/* ---- response verbs through the real backend ---------------------------- */

static int gr_write_exec(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
    {
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return chmod(path, 0700) == 0;
}

static int gr_file_has(const char *path, const char *needle)
{
    char   buf[8192];
    size_t n;
    FILE  *f = fopen(path, "r");
    if (f == NULL)
    {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

static int gr_wait(const char *path, const char *needle, int ms)
{
    int waited = 0;
    while (waited <= ms)
    {
        if (gr_file_has(path, needle))
        {
            return 1;
        }
        usleep(20000);
        waited += 20;
    }
    return 0;
}

static void test_respond_verbs(void)
{
    const char *dir = PSD_E2E_SCRATCH;
    char        poll[1024], resp[1024], out[1024], body[2048];
    char        poll_cmd[1100];
    psd_proc_t  ents[8];
    int         n;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        GR_CHECK(!"cannot create PSD_E2E_SCRATCH");
        return;
    }
    snprintf(poll, sizeof(poll), "%s/grade-poll.sh", dir);
    snprintf(resp, sizeof(resp), "%s/grade-resp.sh", dir);
    snprintf(out,  sizeof(out),  "%s/grade-resp.txt", dir);
    unlink(out);

    GR_CHECK(gr_write_exec(poll,
        "#!/bin/sh\n"
        "printf 'id=4123\\tlabel=web\\tgrade=1\\n'\n"
        "printf 'id=4124\\tlabel=db\\tgrade=0.25\\n'\n"
        "printf 'id=4125\\tlabel=plain\\tweight=64\\n'\n"));
    snprintf(body, sizeof(body), "#!/bin/sh\necho \"$@\" >> '%s'\n", out);
    GR_CHECK(gr_write_exec(resp, body));
    snprintf(poll_cmd, sizeof(poll_cmd), "'%s'", poll);

    proc_script_install(poll_cmd, resp, 1000);
    n = psd_test_list_wait(ents, 8);
    GR_CHECK(n == 3);
    GR_CHECK(n == 3 && ents[0].has_grade && fabs(ents[0].grade - 1.0) < 1e-9);
    GR_CHECK(n == 3 && ents[1].has_grade && fabs(ents[1].grade - 0.25) < 1e-9);
    GR_CHECK(n == 3 && !ents[2].has_grade);
    GR_CHECK(psd_proc_kill_checked(4124, 0, "db") == PSD_ACT_DONE);
    GR_CHECK(gr_wait(out, "delete 4124 label=db", 3000));
    GR_CHECK(psd_proc_renice_checked(4123, 0, "web", 4) == PSD_ACT_DONE);
    GR_CHECK(gr_wait(out, "post 4123 amount=4 label=web", 3000));
    usleep(300000);   /* let any stray responder land before the check */
    GR_CHECK(!gr_file_has(out, "wound"));
    GR_CHECK(!gr_file_has(out, "kill"));

    proc_backend_set(NULL);
    unlink(poll);
    unlink(resp);
    unlink(out);
    (void) rmdir(dir);
}

/* ---- a backend that never writes the grade fields ------------------------ */

/* Like the native backend: fills identity and metrics only. */
static int stub_list(psd_proc_t *out, int max)
{
    if (max < 1)
    {
        return 0;
    }
    out[0].pid = 777;
    out[0].ppid = 0;
    out[0].uid = (unsigned int) getuid();
    out[0].footprint = 64;
    snprintf(out[0].name, sizeof(out[0].name), "plain");
    return 1;
}
static unsigned int stub_uid(void) { return (unsigned int) getuid(); }
static void stub_renice(int pid, int d) { (void) pid; (void) d; }
static int  stub_kill(int pid) { (void) pid; return 0; }
static int  stub_lookup(int pid, psd_proc_t *out) { (void) pid; (void) out; return 0; }
static const proc_backend_t stub_backend =
{
    stub_list, stub_uid, stub_renice, stub_kill, stub_lookup,
};

/* A graded script entity, then a backend that never sets has_grade: the
 * collect buffer must not hand the second one the first one's grade. */
static void test_collect_clears_grade(void)
{
    const char *dir = PSD_E2E_SCRATCH;
    char        poll[1024], poll_cmd[1100];
    psd_proc_t  ents[8];
    int         n;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        GR_CHECK(!"cannot create PSD_E2E_SCRATCH");
        return;
    }
    snprintf(poll, sizeof(poll), "%s/grade-carry.sh", dir);
    GR_CHECK(gr_write_exec(poll,
        "#!/bin/sh\nprintf 'id=777\\tlabel=plain\\tgrade=1\\n'\n"));
    snprintf(poll_cmd, sizeof(poll_cmd), "'%s'", poll);

    proc_script_install(poll_cmd, NULL, 1000);
    psd_select_init();
    n = psd_test_collect_wait(ents, 8);
    GR_CHECK(n == 1 && ents[0].has_grade);

    proc_backend_set(&stub_backend);
    psd_select_init();
    n = psd_test_collect_wait(ents, 8);
    GR_CHECK(n == 1 && ents[0].pid == 777);
    GR_CHECK(n == 1 && ents[0].has_grade == 0);

    proc_backend_set(NULL);
    unlink(poll);
    (void) rmdir(dir);
}

void script_grade_tests(int *checks_run, int *checks_failed)
{
    gr_run = checks_run;
    gr_failed = checks_failed;
    test_grade_parse();
    test_grade_tier();
    test_grade_pick_degrade();
    test_respond_verbs();
    test_collect_clears_grade();
}
