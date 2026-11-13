/*
 * psDoom -- end-to-end check of the plugin harness (harness/) through the
 * real script backend.
 *
 * psDoom's own proc_script backend runs the harness exactly as the setup
 * PSDOOM_POLL_CMD="ps-harness get", PSDOOM_RESPOND_CMD="ps-harness" would:
 * the poll through `sh -c` under the 1-second timeout, the responder exec'd
 * detached with `verb id [key=value...]`: grades arrive, a kill sends
 * `delete` and a wound `post`, live; then the default dry run leaves the CSV
 * alone. The
 * harness drives the CSV plugin on a CSV file this test writes, so the real
 * action path is proven on that file only. No process is ever signalled.
 *
 * All harness state lives under PSD_E2E_SCRATCH (a directory in the build
 * tree, set by CMakeLists.txt); nothing is read from the environment.
 *
 * Registered from proc_select_test.c's main (harness_e2e_tests()).
 */

#include "proc_backend.h"
#include "proc_script.h"
#include "proc_select.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PSD_PYTHON3
#error "PSD_PYTHON3 must be defined (CMakeLists.txt)"
#endif
#ifndef PSD_HARNESS_DIR
#error "PSD_HARNESS_DIR must be defined (CMakeLists.txt)"
#endif
#ifndef PSD_E2E_SCRATCH
#error "PSD_E2E_SCRATCH must be defined (CMakeLists.txt)"
#endif

static int *e2e_run;
static int *e2e_failed;

#define E2E_CHECK(cond)                                                      \
    do {                                                                     \
        (*e2e_run)++;                                                        \
        if (!(cond)) {                                                       \
            (*e2e_failed)++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static const char *const state_files[] = {
    "ids.json", "cache.json", "audit.jsonl", "rate.json", "state.lock",
    "refresh.lock", "refresh.log", "refresh.pid", "dirty", NULL
};

static void path_join(char *out, size_t cap, const char *a, const char *b)
{
    snprintf(out, cap, "%s/%s", a, b);
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
    {
        return 0;
    }
    fputs(text, f);
    return fclose(f) == 0;
}

static int read_text(const char *path, char *buf, size_t cap)
{
    FILE  *f = fopen(path, "r");
    size_t n;
    if (f == NULL)
    {
        buf[0] = '\0';
        return 0;
    }
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 1;
}

static int file_has(const char *path, const char *needle)
{
    char buf[16384];
    read_text(path, buf, sizeof(buf));
    return strstr(buf, needle) != NULL;
}

static int count_in(const char *path, const char *needle)
{
    char        buf[16384];
    const char *p;
    int         n = 0;
    read_text(path, buf, sizeof(buf));
    for (p = buf; (p = strstr(p, needle)) != NULL; p += strlen(needle))
    {
        n++;
    }
    return n;
}

/* Wait until `path` does (want=1) or does not (want=0) contain `needle`. */
static int wait_file(const char *path, const char *needle, int want, int ms)
{
    int waited = 0;
    while (waited <= ms)
    {
        if (file_has(path, needle) == want)
        {
            return 1;
        }
        usleep(20000);
        waited += 20;
    }
    return 0;
}

/* The harness detaches its refresher; wait until none holds the lock. */
static void wait_refresher(const char *state)
{
    char lock[1024];
    int  waited = 0;
    path_join(lock, sizeof(lock), state, "refresh.lock");
    while (waited < 15000)
    {
        int fd = open(lock, O_RDWR);
        if (fd < 0)
        {
            return;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            close(fd);
            return;
        }
        close(fd);
        usleep(50000);
        waited += 50;
    }
    fprintf(stderr, "harness_e2e: a refresher still holds %s\n", lock);
}

static void clear_state(const char *state)
{
    char p[1024];
    int  i;
    wait_refresher(state);
    for (i = 0; state_files[i] != NULL; i++)
    {
        path_join(p, sizeof(p), state, state_files[i]);
        (void) unlink(p);
    }
    (void) rmdir(state);
}

static int find_named(const psd_proc_t *out, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
    {
        if (strcmp(out[i].name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

/* Poll through the backend until `want` entities appear (each poll also
 * kicks the harness's background refresh). */
static int collect_until(psd_proc_t *out, int max, int want, int tries)
{
    int n = 0;
    while (tries-- > 0)
    {
        n = psd_select_collect(out, max);
        if (n == want)
        {
            break;
        }
        usleep(100000);
    }
    return n;
}

/* One ps-harness root: <root>/plugins/csv.json (the CSV plugin on this test's
 * CSV; no state_dir, so state goes to <root>/state/csv) and <root>/current
 * naming it, as `ps-harness use csv` would. */
static void write_root(const char *root, const char *csv, int live)
{
    char path[1024];
    char cfg[4096];

    (void) mkdir(root, 0700);
    path_join(path, sizeof(path), root, "plugins");
    (void) mkdir(path, 0700);
    path_join(path, sizeof(path), root, "plugins/csv.json");
    snprintf(cfg, sizeof(cfg),
             "{%s \"cache_ttl_s\": 0,\n"
             " \"plugin\": {\"name\": \"csv\", %s\n"
             "  \"command\": [\"%s\", \"%s/plugins/csv_rows.py\",\n"
             "   \"--file\", \"%s\", \"--priority-column\", \"priority\"]}}\n",
             live ? "\"dry_run\": false," : "",
             live ? "\"allow_remove\": true," : "",
             PSD_PYTHON3, PSD_HARNESS_DIR, csv);
    E2E_CHECK(write_text(path, cfg));
    path_join(path, sizeof(path), root, "current");
    E2E_CHECK(write_text(path, "csv\n"));
}

static void clear_root(const char *root)
{
    char p[1024];

    path_join(p, sizeof(p), root, "state/csv");
    clear_state(p);
    path_join(p, sizeof(p), root, "state");
    (void) rmdir(p);
    path_join(p, sizeof(p), root, "plugins/csv.json");
    (void) unlink(p);
    path_join(p, sizeof(p), root, "plugins");
    (void) rmdir(p);
    path_join(p, sizeof(p), root, "current");
    (void) unlink(p);
    (void) rmdir(root);
}

/* The human's setup: PSDOOM_POLL_CMD="ps-harness get" and
 * PSDOOM_RESPOND_CMD="ps-harness", with the launcher by its path and an
 * explicit --root (never the home directory). */
static void install(const char *root)
{
    char poll[2048];
    char respond[2048];
    snprintf(poll, sizeof(poll), "'%s/ps-harness' --root '%s' get",
             PSD_HARNESS_DIR, root);
    /* PSDOOM_RESPOND_CMD is split on whitespace, never run by a shell. */
    snprintf(respond, sizeof(respond), "%s/ps-harness --root %s",
             PSD_HARNESS_DIR, root);
    proc_script_install(poll, respond, 1000);
    psd_select_init();
}

void harness_e2e_tests(int *checks_run, int *checks_failed)
{
    const char *dir = PSD_E2E_SCRATCH;
    char        csv[1024], csvlock[1024], root[1024], root_dry[1024];
    char        audit[1024], audit_dry[1024];
    psd_proc_t  out[16];
    int         n, a, b, c, d;

    e2e_run = checks_run;
    e2e_failed = checks_failed;

    /* The responder command is split on whitespace (docs/script-backend.md). */
    /* ...and the paths are pasted into a JSON config unescaped. */
    E2E_CHECK(strpbrk(PSD_PYTHON3 PSD_HARNESS_DIR PSD_E2E_SCRATCH, " \t\"\\'") == NULL);
    E2E_CHECK(dir[0] == '/' && strlen(dir) > 1);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        E2E_CHECK(!"cannot create PSD_E2E_SCRATCH");
        return;
    }
    path_join(csv,       sizeof(csv),       dir, "list.csv");
    path_join(csvlock,   sizeof(csvlock),   dir, ".list.csv.lock");
    path_join(root,      sizeof(root),      dir, "root-live");
    path_join(root_dry,  sizeof(root_dry),  dir, "root-dry");
    path_join(audit,     sizeof(audit),     root, "state/csv/audit.jsonl");
    path_join(audit_dry, sizeof(audit_dry), root_dry, "state/csv/audit.jsonl");
    clear_root(root);
    clear_root(root_dry);

    E2E_CHECK(write_text(csv, "id,name,priority\n"
                              "r1,Alpha,5\nr2,Bravo,9\nr3,Charlie,1\n"
                              "r4,A task name well past thirty-one bytes long,5\n"));
    write_root(root, csv, 1);
    write_root(root_dry, csv, 0);

    /* ---- live plugin: real post/delete on the test CSV ---- */
    install(root);
    n = collect_until(out, 16, 4, 50);
    E2E_CHECK(n == 4);
    a = find_named(out, n, "Alpha");
    b = find_named(out, n, "Bravo");
    c = find_named(out, n, "Charlie");
    d = find_named(out, n, "A task name well past thirty-on");
    E2E_CHECK(a >= 0 && b >= 0 && c >= 0 && d >= 0);
    if (a < 0 || b < 0 || c < 0 || d < 0)
    {
        goto done;
    }
    /* Priority 9/5/1 -> grade 1, 0.5, 0 (and the weights alongside). */
    E2E_CHECK(out[b].has_grade && out[b].grade == 1.0);
    E2E_CHECK(out[a].has_grade && out[a].grade == 0.5);
    E2E_CHECK(out[c].has_grade && out[c].grade == 0.0);
    E2E_CHECK(out[b].footprint == 4294967296ULL);
    E2E_CHECK(out[a].footprint == 201326592ULL);
    E2E_CHECK(out[c].footprint == 0);

    /* kill -> `ps-harness delete <id>` -> the row is deleted from the CSV. */
    E2E_CHECK(psd_proc_kill_checked(out[b].pid, 0, "Bravo") == PSD_ACT_DONE);
    E2E_CHECK(wait_file(csv, "Bravo", 0, 5000));
    /* wound (amount=4) -> `ps-harness post <id> amount=4` -> 5 becomes 1. */
    E2E_CHECK(psd_proc_renice_checked(out[a].pid, 0, "Alpha", 4) == PSD_ACT_DONE);
    E2E_CHECK(wait_file(csv, "r1,Alpha,1", 1, 5000));
    E2E_CHECK(wait_file(audit, "\"action\": \"nudge\"", 1, 3000));
    E2E_CHECK(count_in(audit, "\"outcome\": \"ok\"") == 2);

    /* A label cut at 31 bytes: psDoom sends back exactly what the harness
     * listed, so the harness's label check lets the real kill through. */
    E2E_CHECK(strlen(out[d].name) == 31);
    E2E_CHECK(psd_proc_kill_checked(out[d].pid, 0, out[d].name) == PSD_ACT_DONE);
    E2E_CHECK(wait_file(csv, "thirty-one", 0, 5000));
    E2E_CHECK(count_in(audit, "\"outcome\": \"ok\"") == 3);
    E2E_CHECK(!file_has(audit, "label-mismatch"));
    E2E_CHECK(file_has(audit, "\"verb\": \"delete\""));
    E2E_CHECK(file_has(audit, "\"verb\": \"post\""));
    E2E_CHECK(!file_has(audit, "\"verb\": \"kill\""));
    E2E_CHECK(!file_has(audit, "\"verb\": \"wound\""));

    /* The next polls no longer list the removed row. */
    n = collect_until(out, 16, 2, 50);
    E2E_CHECK(n == 2);
    E2E_CHECK(find_named(out, n, "Bravo") < 0);

    /* ---- default config: dry run, the CSV is left alone ---- */
    install(root_dry);
    n = collect_until(out, 16, 2, 50);
    E2E_CHECK(n == 2);
    c = find_named(out, n, "Charlie");
    E2E_CHECK(c >= 0);
    if (c >= 0)
    {
        E2E_CHECK(out[c].has_grade);
        E2E_CHECK(psd_proc_kill_checked(out[c].pid, 0, "Charlie") == PSD_ACT_DONE);
        E2E_CHECK(wait_file(audit_dry, "\"outcome\": \"dry-run\"", 1, 5000));
        E2E_CHECK(file_has(audit_dry, "\"verb\": \"delete\""));
        E2E_CHECK(file_has(csv, "r3,Charlie,1"));
    }

done:
    proc_backend_set(NULL);
    clear_root(root);
    clear_root(root_dry);
    (void) unlink(csv);
    (void) unlink(csvlock);
    (void) rmdir(dir);
}
