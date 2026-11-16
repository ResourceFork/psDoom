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
#include "psd_test_wait.h"

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
    (void) psd_test_list_wait(ents, 8);
    (void) psd_test_list_wait(ents, 8);
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
    (void) psd_test_collect_wait(ents, 8);
    RQ_CHECK(rq_last_line(args, last, sizeof(last)) && strcmp(last, "[]") == 0);
    RQ_CHECK(proc_script_sort_count() == 2);
    RQ_CHECK(proc_script_sort_label() != NULL
             && strcmp(proc_script_sort_label(), "Memory") == 0);
    n = psd_test_collect_wait(ents, 8);
    RQ_CHECK(rq_last_line(args, last, sizeof(last))
             && strcmp(last, "[limit=64]") == 0);
    RQ_CHECK(n == 2 && ents[0].pid == 21 && ents[1].pid == 22);  /* no re-sort */
    RQ_CHECK(n == 2 && strcmp(ents[0].group, "Grp") == 0
             && ents[1].group[0] == '\0');
    proc_script_cycle_sort(1);                       /* Memory -> CPU */
    RQ_CHECK(strcmp(proc_script_sort_label(), "CPU") == 0);
    (void) psd_test_collect_wait(ents, 8);
    RQ_CHECK(rq_last_line(args, last, sizeof(last))
             && strcmp(last, "[sort=cpu limit=64]") == 0);
    proc_script_cycle_sort(1);                       /* CPU -> Memory (skips App) */
    RQ_CHECK(strcmp(proc_script_sort_label(), "Memory") == 0);
    proc_script_cycle_sort(-1);
    RQ_CHECK(strcmp(proc_script_sort_label(), "CPU") == 0);

    /* A protected name and psDoom's own pid, listed by the trusted backend,
     * reach the game: no host-side filter on the script path. */
    snprintf(body, sizeof(body),
             "#!/bin/sh\nprintf 'id=%d\\tlabel=self\\n'\n"
             "printf 'id=31\\tlabel=Dock\\n'\n"
             "printf 'id=32\\tlabel=loginwindow\\n'\n", (int) getpid());
    RQ_CHECK(rq_write_exec(plain, body));
    snprintf(cmd, sizeof(cmd), "'%s'", plain);
    proc_script_install(cmd, NULL, 1000);
    psd_select_init();
    n = psd_test_collect_wait(ents, 8);
    RQ_CHECK(n == 3 && ents[0].pid == (int) getpid() && ents[1].pid == 31
             && ents[2].pid == 32);

    proc_backend_set(NULL);
    unlink(plain);
    unlink(dims);
    unlink(args);
    (void) rmdir(dir);
}

/* Every response carries the host's mode; the default is dryrun, and an
 * unknown mode falls back to it. psDoom never withholds the event. */
static void test_mode_on_respond(void)
{
    const char *dir = PSD_E2E_SCRATCH;
    char        poll[1024], resp[1024], out[1024], body[2048], cmd[1100];
    char        last[512];
    psd_proc_t  ents[4];

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        RQ_CHECK(!"cannot create PSD_E2E_SCRATCH");
        return;
    }
    snprintf(poll, sizeof(poll), "%s/rq-mpoll.sh", dir);
    snprintf(resp, sizeof(resp), "%s/rq-mresp.sh", dir);
    snprintf(out,  sizeof(out),  "%s/rq-mresp.txt", dir);
    unlink(out);
    RQ_CHECK(rq_write_exec(poll,
        "#!/bin/sh\nprintf 'id=41\\tlabel=x\\n'\n"));
    snprintf(body, sizeof(body), "#!/bin/sh\necho \"$*\" >> '%s'\n", out);
    RQ_CHECK(rq_write_exec(resp, body));
    snprintf(cmd, sizeof(cmd), "'%s'", poll);
    proc_script_install(cmd, resp, 1000);
    (void) psd_test_list_wait(ents, 4);

    static const struct { const char *set; int ok; const char *want; } m[] = {
        { NULL,        0, "delete 41 label=x mode=dryrun" },
        { "post-only", 1, "delete 41 label=x mode=post-only" },
        { "delete",    1, "delete 41 label=x mode=delete" },
        { "kill-all",  0, "delete 41 label=x mode=dryrun" },
    };
    size_t i;
    for (i = 0; i < sizeof(m) / sizeof(m[0]); i++)
    {
        int waited = 0;
        if (m[i].set != NULL)
        {
            RQ_CHECK(proc_script_set_mode(m[i].set) == m[i].ok);
        }
        RQ_CHECK(psd_proc_kill_checked(41, 0, "x") == PSD_ACT_DONE);
        while (waited < 3000 && (!rq_last_line(out, last, sizeof(last))
                                 || strcmp(last, m[i].want) != 0))
        {
            usleep(20000);
            waited += 20;
        }
        RQ_CHECK(strcmp(last, m[i].want) == 0);
    }
    RQ_CHECK(psd_proc_renice_checked(41, 0, "x", 4) == PSD_ACT_DONE);
    {
        int waited = 0;
        while (waited < 3000 && (!rq_last_line(out, last, sizeof(last))
                                 || strncmp(last, "post", 4) != 0))
        {
            usleep(20000);
            waited += 20;
        }
        RQ_CHECK(strcmp(last, "post 41 amount=4 label=x mode=dryrun") == 0);
    }

    proc_backend_set(NULL);
    unlink(poll);
    unlink(resp);
    unlink(out);
    (void) rmdir(dir);
}

/* `# effect` header lines: what a wound and a kill do, in the plugin's own
 * plain words. */
static void test_parse_effects(void)
{
    char post[PSD_SCRIPT_EFFECT_MAX];
    char del[PSD_SCRIPT_EFFECT_MAX];

    psd_script_parse_effects(
        "# dim id=relevance kind=numeric dir=desc roles=sort unit=score "
        "label=Relevance\n"
        "# effect verb=post text=lowers its priority\n"
        "# effect verb=delete text=quits the process\r\n"
        "# effect verb=other text=ignored\n"
        "id=5\tlabel=x\n"
        "# effect verb=post text=after the header: ignored\n",
        post, del, sizeof(post));
    RQ_CHECK(strcmp(post, "lowers its priority") == 0);
    RQ_CHECK(strcmp(del, "quits the process") == 0);

    /* None given; a control byte is drawn as a space; too long is cut. */
    psd_script_parse_effects("id=5\tlabel=x\n", post, del, sizeof(post));
    RQ_CHECK(post[0] == '\0' && del[0] == '\0');
    psd_script_parse_effects("# effect verb=post text=a\tb\x7f" "c\n",
                             post, del, sizeof(post));
    RQ_CHECK(strcmp(post, "a b c") == 0);
    psd_script_parse_effects(
        "# effect verb=delete text=0123456789012345678901234567890123456789XYZ\n",
        post, del, sizeof(post));
    RQ_CHECK(strlen(del) == PSD_SCRIPT_EFFECT_MAX - 1);
    RQ_CHECK(del[PSD_SCRIPT_EFFECT_MAX - 2] == '9');

    /* Before any poll the backend has said nothing. */
    proc_script_install("true", "true", 1000);
    RQ_CHECK(proc_script_effect(0) == NULL);
    RQ_CHECK(proc_script_effect(1) == NULL);
    proc_backend_set(NULL);
}

void script_request_tests(int *checks_run, int *checks_failed)
{
    rq_run = checks_run;
    rq_failed = checks_failed;
    test_id_ok();
    test_header();
    test_poll_command();
    test_backend_request_roundtrip();
    test_mode_on_respond();
    test_parse_effects();
}
