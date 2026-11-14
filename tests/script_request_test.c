/*
 * psDoom -- script contract host tests for the host's sort request: the
 * `# dim` / `# sort` header, the request words appended to the poll command,
 * the "Sort by" cycle, `group=`, and the script backend keeping its
 * plugin's order through triage (the safety filters still apply).
 *
 * Only this test's own scripts under PSD_E2E_SCRATCH run; nothing is
 * signalled. Registered from proc_select_test.c's main.
 */

#include "proc_backend.h"
#include "proc_script.h"
#include "proc_select.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PSD_E2E_SCRATCH
#error "PSD_E2E_SCRATCH must be defined (CMakeLists.txt)"
#endif

static int *rq_run;
static int *rq_failed;

#define RQ_CHECK(cond)                                                       \
    do {                                                                     \
        (*rq_run)++;                                                         \
        if (!(cond)) {                                                       \
            (*rq_failed)++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static void test_id_ok(void)
{
    RQ_CHECK(psd_script_id_ok("mem"));
    RQ_CHECK(psd_script_id_ok("team_name2"));
    RQ_CHECK(psd_script_id_ok("abcdefghijklmnop"));       /* 16 */
    RQ_CHECK(!psd_script_id_ok("abcdefghijklmnopq"));     /* 17 */
    RQ_CHECK(!psd_script_id_ok(""));
    RQ_CHECK(!psd_script_id_ok(NULL));
    RQ_CHECK(!psd_script_id_ok("Mem"));
    RQ_CHECK(!psd_script_id_ok("2x"));
    RQ_CHECK(!psd_script_id_ok("a b"));
    RQ_CHECK(!psd_script_id_ok("a;rm"));
    RQ_CHECK(!psd_script_id_ok("$(x)"));
}

static void test_header(void)
{
    psd_script_dim_t d[4];
    char             applied[17];
    int              n;
    const char      *buf =
        "# dim id=relevance kind=numeric dir=desc roles=sort unit=score label=Relevance\n"
        "# dim id=app kind=category dir=desc roles=group unit= label=App Name\n"
        "# dim id=Bad kind=numeric roles=sort label=x\n"
        "# dim id=cpu kind=numeric dir=desc roles=sort unit=percent label=CPU\r\n"
        "# sort id=cpu dir=desc\n"
        "# summon: ignored sort=x\n"
        "id=5\tlabel=a\n"
        "# dim id=late kind=numeric roles=sort label=Late\n";

    n = psd_script_parse_header(buf, d, 4, applied, sizeof(applied));
    RQ_CHECK(n == 3);                               /* Bad and late skipped */
    RQ_CHECK(strcmp(d[0].id, "relevance") == 0 && d[0].sortable);
    RQ_CHECK(strcmp(d[0].label, "Relevance") == 0);
    RQ_CHECK(strcmp(d[1].id, "app") == 0 && !d[1].sortable && d[1].groupable);
    RQ_CHECK(strcmp(d[1].label, "App Name") == 0);  /* label keeps spaces */
    RQ_CHECK(strcmp(d[2].id, "cpu") == 0 && d[2].sortable);
    RQ_CHECK(strcmp(d[2].label, "CPU") == 0);       /* CR dropped */
    RQ_CHECK(strcmp(applied, "cpu") == 0);
    RQ_CHECK(psd_script_parse_header("id=2\n", d, 4, applied,
                                     sizeof(applied)) == 0);
    RQ_CHECK(applied[0] == '\0');
    /* a category can never be a sort, whatever its roles say */
    RQ_CHECK(psd_script_parse_header(
        "# dim id=team kind=category roles=sort label=T\n", d, 4, applied,
        sizeof(applied)) == 1 && !d[0].sortable);
}

static void test_poll_command(void)
{
    char out[64];

    RQ_CHECK(psd_script_poll_command("summon get", "cpu", 64, out,
                                     sizeof(out)));
    RQ_CHECK(strcmp(out, "summon get sort=cpu limit=64") == 0);
    RQ_CHECK(psd_script_poll_command("summon get", NULL, 64, out,
                                     sizeof(out)));
    RQ_CHECK(strcmp(out, "summon get limit=64") == 0);
    RQ_CHECK(psd_script_poll_command("summon get", "$(rm -rf ~)", 0, out,
                                     sizeof(out)));
    RQ_CHECK(strcmp(out, "summon get") == 0);       /* bad id: not sent */
    RQ_CHECK(psd_script_poll_command("summon get", "cpu", 99999, out,
                                     sizeof(out)));
    RQ_CHECK(strcmp(out, "summon get sort=cpu") == 0);
    RQ_CHECK(!psd_script_poll_command("summon get with a very long command "
                                      "line", "cpu", 64, out, 48));
    RQ_CHECK(strcmp(out, "summon get with a very long command line") == 0);
}

static psd_proc_t rq_mk(int pid, const char *name, unsigned long long mem,
                        int daemon)
{
    psd_proc_t p;
    memset(&p, 0, sizeof(p));
    p.pid = pid;
    p.ppid = 1;
    p.uid = 501;
    p.is_daemon = daemon;
    p.footprint = mem;
    snprintf(p.name, sizeof(p.name), "%s", name);
    return p;
}

static void test_triage_ordered(void)
{
    /* Small first, big last: the ranked triage puts the big one first, the
     * ordered one keeps the input order; both drop the protected and the
     * foreign-uid entries and self. */
    psd_proc_t raw[5];
    psd_proc_t out[8];
    int        n;

    raw[0] = rq_mk(10, "small", 1, 1);
    raw[1] = rq_mk(11, "Dock", 1ULL << 32, 0);     /* protected */
    raw[2] = rq_mk(12, "mid", 1ULL << 28, 1);
    raw[3] = rq_mk(13, "big", 1ULL << 32, 0);
    raw[4] = rq_mk(99, "self", 1ULL << 32, 0);
    n = psd_select_triage(raw, 5, 501, 99, 0, out, 8);
    RQ_CHECK(n == 3 && out[0].pid == 13);
    n = psd_select_triage_ordered(raw, 5, 501, 99, 0, out, 8);
    RQ_CHECK(n == 3);
    RQ_CHECK(n == 3 && out[0].pid == 10 && out[1].pid == 12
             && out[2].pid == 13);
    n = psd_select_triage_ordered(raw, 5, 501, 99, 0, out, 2);
    RQ_CHECK(n == 2 && out[1].pid == 12);           /* the cut keeps the head */
}

static int rq_write_exec(const char *path, const char *text)
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

static int rq_last_line(const char *path, char *out, size_t cap)
{
    char  line[512];
    FILE *f = fopen(path, "r");
    int   got = 0;

    out[0] = '\0';
    if (f == NULL)
    {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        line[strcspn(line, "\n")] = '\0';
        snprintf(out, cap, "%s", line);
        got = 1;
    }
    fclose(f);
    return got;
}

static void test_backend_request_roundtrip(void)
{
    const char *dir = PSD_E2E_SCRATCH;
    char        plain[1024], dims[1024], args[1024], body[2048], cmd[1100];
    char        last[512];
    psd_proc_t  ents[8];
    int         n;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        RQ_CHECK(!"cannot create PSD_E2E_SCRATCH");
        return;
    }
    snprintf(plain, sizeof(plain), "%s/rq-plain.sh", dir);
    snprintf(dims,  sizeof(dims),  "%s/rq-dims.sh", dir);
    snprintf(args,  sizeof(args),  "%s/rq-args.txt", dir);
    unlink(args);

    /* A plain script never gets request words. */
    snprintf(body, sizeof(body),
             "#!/bin/sh\necho \"[$*]\" >> '%s'\nprintf 'id=7\\tlabel=p\\n'\n",
             args);
    RQ_CHECK(rq_write_exec(plain, body));
    snprintf(cmd, sizeof(cmd), "'%s'", plain);
    proc_script_install(cmd, NULL, 1000);
    proc_script_set_limit(64);
    (void) proc_backend()->list(ents, 8);
    (void) proc_backend()->list(ents, 8);
    RQ_CHECK(rq_last_line(args, last, sizeof(last)) && strcmp(last, "[]") == 0);
    RQ_CHECK(proc_script_sort_count() == 0);
    RQ_CHECK(proc_script_sort_label() == NULL);
    RQ_CHECK(proc_backend()->preserves_order == 1);

    /* A dimension-declaring backend: the second poll carries limit, and
     * after the player cycles, sort too. Entities keep the poll's order
     * through psd_select_collect, small first. */
    snprintf(body, sizeof(body),
             "#!/bin/sh\necho \"[$*]\" >> '%s'\n"
             "printf '# dim id=mem kind=numeric dir=desc roles=sort unit=b label=Memory\\n'\n"
             "printf '# dim id=cpu kind=numeric dir=desc roles=sort unit=p label=CPU\\n'\n"
             "printf '# dim id=app kind=category roles=group label=App\\n'\n"
             "printf '# sort id=mem dir=desc\\n'\n"
             "printf 'id=21\\tweight=1\\tgroup=Grp\\tlabel=small\\n'\n"
             "printf 'id=22\\tweight=4294967296\\tlabel=big\\n'\n", args);
    RQ_CHECK(rq_write_exec(dims, body));
    snprintf(cmd, sizeof(cmd), "'%s'", dims);
    proc_script_install(cmd, NULL, 1000);
    proc_script_set_limit(64);
    psd_select_init();
    (void) psd_select_collect(ents, 8);
    RQ_CHECK(rq_last_line(args, last, sizeof(last)) && strcmp(last, "[]") == 0);
    RQ_CHECK(proc_script_sort_count() == 2);
    RQ_CHECK(proc_script_sort_label() != NULL
             && strcmp(proc_script_sort_label(), "Memory") == 0);
    n = psd_select_collect(ents, 8);
    RQ_CHECK(rq_last_line(args, last, sizeof(last))
             && strcmp(last, "[limit=64]") == 0);
    RQ_CHECK(n == 2 && ents[0].pid == 21 && ents[1].pid == 22);  /* no re-sort */
    RQ_CHECK(n == 2 && strcmp(ents[0].group, "Grp") == 0
             && ents[1].group[0] == '\0');
    proc_script_cycle_sort(1);                       /* Memory -> CPU */
    RQ_CHECK(strcmp(proc_script_sort_label(), "CPU") == 0);
    (void) psd_select_collect(ents, 8);
    RQ_CHECK(rq_last_line(args, last, sizeof(last))
             && strcmp(last, "[sort=cpu limit=64]") == 0);
    proc_script_cycle_sort(1);                       /* CPU -> Memory (skips App) */
    RQ_CHECK(strcmp(proc_script_sort_label(), "Memory") == 0);
    proc_script_cycle_sort(-1);
    RQ_CHECK(strcmp(proc_script_sort_label(), "CPU") == 0);

    proc_backend_set(NULL);
    unlink(plain);
    unlink(dims);
    unlink(args);
    (void) rmdir(dir);
}

void script_request_tests(int *checks_run, int *checks_failed)
{
    rq_run = checks_run;
    rq_failed = checks_failed;
    test_id_ok();
    test_header();
    test_poll_command();
    test_triage_ordered();
    test_backend_request_roundtrip();
}
