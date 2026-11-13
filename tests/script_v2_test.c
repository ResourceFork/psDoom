/*
 * psDoom -- script contract v2 host tests: the `#!psdoom v2` header, the
 * `grade` field, the grade -> drawable-tier mapping (including the shareware
 * degrade), and the v2 response verbs (post/delete) against v1 (wound/kill).
 *
 * The respond checks run only this test's own recorder script under the
 * build-tree scratch directory (PSD_E2E_SCRATCH); nothing is signalled.
 * Registered from proc_select_test.c's main (script_v2_tests()).
 */

#include "proc_backend.h"
#include "proc_script.h"
#include "proc_select.h"
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

static int *v2_run;
static int *v2_failed;

#define V2_CHECK(cond)                                                       \
    do {                                                                     \
        (*v2_run)++;                                                         \
        if (!(cond)) {                                                       \
            (*v2_failed)++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* ---- header detection --------------------------------------------------- */

static void test_version_header(void)
{
    V2_CHECK(psd_script_version("") == 1);
    V2_CHECK(psd_script_version(NULL) == 1);
    V2_CHECK(psd_script_version("id=2\n") == 1);
    V2_CHECK(psd_script_version("#!psdoom v1\nid=2\n") == 1);
    V2_CHECK(psd_script_version("#!psdoom v2\nid=2\n") == 2);
    V2_CHECK(psd_script_version("#!psdoom v2") == 2);
    V2_CHECK(psd_script_version("#!psdoom v2 \t\r\nid=2\n") == 2);
    /* Only the first line declares it; later it is just a comment. */
    V2_CHECK(psd_script_version("# hi\n#!psdoom v2\n") == 1);
    V2_CHECK(psd_script_version(" #!psdoom v2\n") == 1);
    V2_CHECK(psd_script_version("#!psdoomv2\n") == 1);
    /* An unknown version is refused, not misread. */
    V2_CHECK(psd_script_version("#!psdoom v3\n") == -1);
    V2_CHECK(psd_script_version("#!psdoom v20\n") == -1);
    V2_CHECK(psd_script_version("#!psdoom v2x\n") == -1);
    V2_CHECK(psd_script_version("#!psdoom \n") == -1);
}

/* ---- grade parsing ------------------------------------------------------- */

static int parse_one(const char *buf, psd_proc_t *p, int *version)
{
    return psd_script_parse_versioned(buf, p, 1, version);
}

static void test_grade_parse(void)
{
    static const struct { const char *val; double want; } ok[] = {
        { "0", 0.0 }, { "1", 1.0 }, { "0.5", 0.5 }, { "0.25", 0.25 },
        { "1.5", 1.0 }, { "-0.2", 0.0 }, { "1e-3", 0.001 }, { "+0.75", 0.75 },
    };
    static const char *const bad[] = {
        "", "abc", "0.5x", " 0.5", "nan", "NaN", "inf", "-inf", "1e999",
    };
    psd_proc_t p;
    char       buf[128];
    int        version;
    size_t     i;

    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
    {
        snprintf(buf, sizeof(buf), "#!psdoom v2\nid=7\tgrade=%s\n", ok[i].val);
        V2_CHECK(parse_one(buf, &p, &version) == 1);
        V2_CHECK(version == 2);
        V2_CHECK(p.has_grade == 1);
        V2_CHECK(fabs(p.grade - ok[i].want) < 1e-9);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        snprintf(buf, sizeof(buf), "#!psdoom v2\nid=7\tgrade=%s\tweight=5\n",
                 bad[i]);
        V2_CHECK(parse_one(buf, &p, &version) == 1);   /* entity kept */
        V2_CHECK(p.has_grade == 0);                     /* grade ignored */
        V2_CHECK(p.footprint == 5);
    }
    /* The last valid grade wins; a later malformed one does not erase it. */
    V2_CHECK(parse_one("#!psdoom v2\nid=7\tgrade=0.2\tgrade=0.9\n",
                       &p, &version) == 1);
    V2_CHECK(fabs(p.grade - 0.9) < 1e-9);
    V2_CHECK(parse_one("#!psdoom v2\nid=7\tgrade=0.2\tgrade=junk\n",
                       &p, &version) == 1);
    V2_CHECK(p.has_grade == 1 && fabs(p.grade - 0.2) < 1e-9);

    /* v1 (header or none): grade is an unknown key, exactly as before. */
    V2_CHECK(parse_one("#!psdoom v1\nid=7\tgrade=0.9\n", &p, &version) == 1);
    V2_CHECK(version == 1 && p.has_grade == 0);
    V2_CHECK(parse_one("id=7\tgrade=0.9\n", &p, &version) == 1);
    V2_CHECK(version == 1 && p.has_grade == 0);
    V2_CHECK(psd_script_parse("#!psdoom v2\nid=7\tgrade=0.9\n", &p, 1) == 1);
    V2_CHECK(p.has_grade == 1);

    /* A refused version lists nothing. */
    V2_CHECK(parse_one("#!psdoom v3\nid=7\n", &p, &version) == 0);
    V2_CHECK(version == -1);

    /* v2 keeps every v1 field and rule. */
    V2_CHECK(parse_one("#!psdoom v2\nid=9\tparent=4\tweight=64\tload=30"
                       "\tflags=1\tlabel=x y\n", &p, &version) == 1);
    V2_CHECK(p.pid == 9 && p.ppid == 4 && p.footprint == 64);
    V2_CHECK(p.cpu_percent == 30 && p.is_daemon == 1);
    V2_CHECK(strcmp(p.name, "x y") == 0 && p.has_grade == 0);
    V2_CHECK(parse_one("#!psdoom v2\nid=1\tgrade=1\n", &p, &version) == 0);
}

/* ---- grade -> tier ------------------------------------------------------ */

static void test_grade_tier(void)
{
    /* tier = min(floor(grade * n), n - 1) */
    V2_CHECK(psd_grade_tier(0.0, 7) == 0);
    V2_CHECK(psd_grade_tier(1.0, 7) == 6);
    V2_CHECK(psd_grade_tier(0.9999, 7) == 6);
    V2_CHECK(psd_grade_tier(1.0 / 7.0 - 1e-9, 7) == 0);
    V2_CHECK(psd_grade_tier(1.0 / 7.0 + 1e-9, 7) == 1);
    V2_CHECK(psd_grade_tier(0.5, 7) == 3);
    V2_CHECK(psd_grade_tier(0.5, 2) == 1);
    V2_CHECK(psd_grade_tier(0.49, 2) == 0);
    V2_CHECK(psd_grade_tier(-1.0, 7) == 0);
    V2_CHECK(psd_grade_tier(2.0, 7) == 6);
    V2_CHECK(psd_grade_tier(NAN, 7) == 0);
    V2_CHECK(psd_grade_tier(1.0, 1) == 0);
    V2_CHECK(psd_grade_tier(1.0, 0) == 0);
}

static void test_grade_pick_degrade(void)
{
    /* Ladder: Zombieman, Shotgun Guy, Imp, Pinky, Hell Knight, Baron, Cyber. */
    static const int full[7]      = { 1, 1, 1, 1, 1, 1, 1 };
    static const int freedoom[7]  = { 1, 1, 1, 1, 0, 1, 1 }; /* no Knight   */
    static const int shareware[7] = { 1, 1, 1, 1, 0, 1, 0 }; /* + no Cyber  */
    static const int none[7]      = { 0, 0, 0, 0, 0, 0, 0 };

    V2_CHECK(psd_grade_pick(1.0, full, 7) == 6);
    V2_CHECK(psd_grade_pick(0.0, full, 7) == 0);
    V2_CHECK(psd_grade_pick(0.6, full, 7) == 4);          /* Hell Knight */
    /* 1.0 is the top *drawable* monster. */
    V2_CHECK(psd_grade_pick(1.0, freedoom, 7) == 6);      /* Cyberdemon  */
    V2_CHECK(psd_grade_pick(1.0, shareware, 7) == 5);     /* Baron       */
    /* Shareware: 5 drawable tiers, so 0.8 -> 4th of 0..4 -> Baron (5). */
    V2_CHECK(psd_grade_pick(0.8, shareware, 7) == 5);
    V2_CHECK(psd_grade_pick(0.79, shareware, 7) == 3);    /* Pinky       */
    V2_CHECK(psd_grade_pick(0.0, shareware, 7) == 0);
    /* Never an undrawable tier. */
    {
        int i;
        for (i = 0; i <= 100; i++)
        {
            int t = psd_grade_pick(i / 100.0, shareware, 7);
            V2_CHECK(t == 0 || shareware[t]);
        }
    }
    /* Tier 0 always counts as drawable. */
    V2_CHECK(psd_grade_pick(1.0, none, 7) == 0);
    V2_CHECK(psd_grade_pick(1.0, NULL, 7) == 0);
}

/* ---- v2 response verbs through the real backend -------------------------- */

static int v2_write_exec(const char *path, const char *text)
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

static int v2_file_has(const char *path, const char *needle)
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

static int v2_wait(const char *path, const char *needle, int ms)
{
    int waited = 0;
    while (waited <= ms)
    {
        if (v2_file_has(path, needle))
        {
            return 1;
        }
        usleep(20000);
        waited += 20;
    }
    return 0;
}

static void test_v2_respond_verbs(void)
{
    const char *dir = PSD_E2E_SCRATCH;
    char        poll1[1024], poll2[1024], resp[1024], out[1024], body[2048];
    char        poll_cmd[1100], resp_cmd[1100];
    psd_proc_t  ents[8];
    int         n;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        V2_CHECK(!"cannot create PSD_E2E_SCRATCH");
        return;
    }
    snprintf(poll1, sizeof(poll1), "%s/v2-poll1.sh", dir);
    snprintf(poll2, sizeof(poll2), "%s/v2-poll2.sh", dir);
    snprintf(resp,  sizeof(resp),  "%s/v2-resp.sh", dir);
    snprintf(out,   sizeof(out),   "%s/v2-resp.txt", dir);
    unlink(out);

    V2_CHECK(v2_write_exec(poll2,
        "#!/bin/sh\n"
        "printf '#!psdoom v2\\n'\n"
        "printf 'id=4123\\tlabel=web\\tgrade=1\\n'\n"
        "printf 'id=4124\\tlabel=db\\tgrade=0.25\\n'\n"));
    V2_CHECK(v2_write_exec(poll1,
        "#!/bin/sh\n"
        "printf 'id=4123\\tlabel=web\\tgrade=1\\n'\n"
        "printf 'id=4124\\tlabel=db\\n'\n"));
    snprintf(body, sizeof(body), "#!/bin/sh\necho \"$@\" >> '%s'\n", out);
    V2_CHECK(v2_write_exec(resp, body));
    snprintf(poll_cmd, sizeof(poll_cmd), "'%s'", poll2);
    snprintf(resp_cmd, sizeof(resp_cmd), "%s", resp);

    /* v2: post/delete, and the grade arrives. */
    proc_script_install(poll_cmd, resp_cmd, 1000);
    n = proc_backend()->list(ents, 8);
    V2_CHECK(n == 2);
    V2_CHECK(n == 2 && ents[0].has_grade && fabs(ents[0].grade - 1.0) < 1e-9);
    V2_CHECK(psd_proc_kill_checked(4124, 0, "db") == PSD_ACT_DONE);
    V2_CHECK(v2_wait(out, "delete 4124 label=db", 3000));
    V2_CHECK(psd_proc_renice_checked(4123, 0, "web", 4) == PSD_ACT_DONE);
    V2_CHECK(v2_wait(out, "post 4123 amount=4 label=web", 3000));

    /* Back to v1 output: wound/kill again, and grade is ignored. */
    snprintf(poll_cmd, sizeof(poll_cmd), "'%s'", poll1);
    proc_script_install(poll_cmd, resp_cmd, 1000);
    n = proc_backend()->list(ents, 8);
    V2_CHECK(n == 2 && !ents[0].has_grade);
    V2_CHECK(psd_proc_kill_checked(4124, 0, "db") == PSD_ACT_DONE);
    V2_CHECK(v2_wait(out, "kill 4124 label=db", 3000));
    V2_CHECK(psd_proc_renice_checked(4123, 0, "web", 4) == PSD_ACT_DONE);
    V2_CHECK(v2_wait(out, "wound 4123 amount=4 label=web", 3000));
    usleep(300000);   /* let any stray responder land before the counts */
    V2_CHECK(!v2_file_has(out, "post 4124"));
    V2_CHECK(!v2_file_has(out, "delete 4123"));

    proc_backend_set(NULL);
    unlink(poll1);
    unlink(poll2);
    unlink(resp);
    unlink(out);
    (void) rmdir(dir);
}

void script_v2_tests(int *checks_run, int *checks_failed)
{
    v2_run = checks_run;
    v2_failed = checks_failed;
    test_version_header();
    test_grade_parse();
    test_grade_tier();
    test_grade_pick_degrade();
    test_v2_respond_verbs();
}
