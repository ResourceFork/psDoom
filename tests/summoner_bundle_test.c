/*
 * psDoom -- Summoner as the only path to real processes.
 *
 * - The default backend is inert: it lists nothing and acts on nothing.
 * - The bundled `summon` is found relative to the engine executable inside
 *   the app, including through a path with spaces, and is run as a fixed
 *   argv [python3, summon, verb, id, ...].
 * - A missing or broken Summoner yields no monsters and no action, and a
 *   reason psDoom can show.
 *
 * Every script here is this test's own stub under PSD_E2E_SCRATCH; nothing
 * is signalled. Registered from proc_select_test.c's main.
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

static int *bt_run;
static int *bt_failed;

#define BT_CHECK(cond)                                                       \
    do {                                                                     \
        (*bt_run)++;                                                         \
        if (!(cond)) {                                                       \
            (*bt_failed)++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static void test_default_backend_is_inert(void)
{
    psd_proc_t ents[4];

    proc_backend_set(NULL);
    BT_CHECK(proc_backend() == &proc_none_backend);
    BT_CHECK(proc_backend()->list(ents, 4) == 0);
    BT_CHECK(proc_backend()->lookup((int) getpid(), &ents[0]) == 0);
    BT_CHECK(proc_backend()->kill((int) getpid()) == 0);   /* a no-op */
    proc_backend()->renice((int) getpid(), 4);             /* a no-op */
    psd_select_init();
    BT_CHECK(psd_select_collect(ents, 4) == 0);
    /* The checked actions find nothing to act on. */
    BT_CHECK(psd_proc_kill_checked((int) getpid(), 0, "x") == PSD_ACT_GONE);
    BT_CHECK(psd_proc_renice_checked((int) getpid(), 0, "x", 4)
             == PSD_ACT_GONE);
    BT_CHECK(!proc_script_active());
}

static void test_bundle_path(void)
{
    char out[512];

    BT_CHECK(psd_summon_bundle_path("/A/psDoom.app/Contents/MacOS/crispy-doom",
                                    out, sizeof(out)));
    BT_CHECK(strcmp(out, "/A/psDoom.app/Contents/Resources/summoner/summon") == 0);
    BT_CHECK(psd_summon_bundle_path("/A/psDoom copy.app/Contents/MacOS/psDoom",
                                    out, sizeof(out)));
    BT_CHECK(strcmp(out,
                    "/A/psDoom copy.app/Contents/Resources/summoner/summon") == 0);
    BT_CHECK(!psd_summon_bundle_path("/usr/local/bin/crispy-doom", out,
                                     sizeof(out)));
    BT_CHECK(!psd_summon_bundle_path("crispy-doom", out, sizeof(out)));
    BT_CHECK(!psd_summon_bundle_path(NULL, out, sizeof(out)));
    BT_CHECK(!psd_summon_bundle_path("/A/psDoom.app/Contents/MacOS/x", out, 10));
}

static const char *bt_allowed;
static int bt_usable(const char *path)
{
    return bt_allowed != NULL && strstr(bt_allowed, path) != NULL;
}

static void test_python_choice(void)
{
    char out[256];

    bt_allowed = "/usr/bin/python3";            /* only the install shim */
    BT_CHECK(!psd_python_choose(bt_usable, out, sizeof(out)));
    bt_allowed = "/usr/bin/python3 /Library/Developer/CommandLineTools/usr/bin/python3";
    BT_CHECK(psd_python_choose(bt_usable, out, sizeof(out)));
    BT_CHECK(strcmp(out, "/Library/Developer/CommandLineTools/usr/bin/python3") == 0);
    bt_allowed = "/Library/Developer/CommandLineTools/usr/bin/python3 /opt/homebrew/bin/python3";
    BT_CHECK(psd_python_choose(bt_usable, out, sizeof(out)));
    BT_CHECK(strcmp(out, "/opt/homebrew/bin/python3") == 0);   /* first */
    bt_allowed = NULL;
    BT_CHECK(!psd_python_choose(bt_usable, out, sizeof(out)));
}

static void test_sh_quote(void)
{
    char out[64];

    BT_CHECK(psd_sh_quote("/a b/c", out, sizeof(out)));
    BT_CHECK(strcmp(out, "'/a b/c'") == 0);
    BT_CHECK(psd_sh_quote("it's", out, sizeof(out)));
    BT_CHECK(strcmp(out, "'it'\\''s'") == 0);
    BT_CHECK(psd_sh_quote("$(rm -rf ~)", out, sizeof(out)));
    BT_CHECK(strcmp(out, "'$(rm -rf ~)'") == 0);
    BT_CHECK(!psd_sh_quote("abcdef", out, 6));
}

static int bt_mkdirs(const char *path)
{
    char tmp[1024];
    size_t i;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i] != '\0'; i++)
    {
        if (tmp[i] == '/')
        {
            tmp[i] = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            {
                return 0;
            }
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static int bt_write(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
    {
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return 1;
}

static int bt_wait_for(const char *path, const char *needle, int ms)
{
    char   buf[2048];
    int    waited = 0;
    while (waited <= ms)
    {
        FILE *f = fopen(path, "r");
        if (f != NULL)
        {
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            if (strstr(buf, needle) != NULL)
            {
                return 1;
            }
        }
        usleep(20000);
        waited += 20;
    }
    return 0;
}

/* Does this host have a python3 the bundled path would use? */
static int bt_have_python(void)
{
    static const char *const c[] = {
        "/opt/homebrew/bin/python3", "/usr/local/bin/python3",
        "/Library/Developer/CommandLineTools/usr/bin/python3",
        "/Applications/Xcode.app/Contents/Developer/usr/bin/python3", NULL };
    int i;
    for (i = 0; c[i] != NULL; i++)
    {
        if (access(c[i], X_OK) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void test_bundled_summoner(void)
{
    const char *dir = PSD_E2E_SCRATCH;
    char app[1024], macos[1024], res[1024], exe[1024], summon[1100];
    char log[1024], body[2048];
    psd_proc_t ents[4];
    int n;

    snprintf(app, sizeof(app), "%s/bundle test/psDoom copy.app", dir);
    snprintf(macos, sizeof(macos), "%s/Contents/MacOS", app);
    snprintf(res, sizeof(res), "%s/Contents/Resources/summoner", app);
    snprintf(exe, sizeof(exe), "%s/crispy-doom", macos);
    snprintf(summon, sizeof(summon), "%s/summon", res);
    snprintf(log, sizeof(log), "%s/bundle-args.txt", dir);
    BT_CHECK(bt_mkdirs(macos) && bt_mkdirs(res));
    unlink(log);

    /* Missing: no summon in the bundle -> inert, with a reason. */
    unlink(summon);
    BT_CHECK(proc_script_install_bundled(exe) == 0);
    BT_CHECK(proc_backend() == &proc_none_backend);
    BT_CHECK(proc_script_status() != NULL
             && strstr(proc_script_status(), "missing") != NULL);
    BT_CHECK(psd_select_collect(ents, 4) == 0);
    BT_CHECK(proc_script_install_bundled(NULL) == 0);       /* no exe path */

    if (!bt_have_python())
    {
        /* This host has no usable python3: the bundled path must say so. */
        BT_CHECK(bt_write(summon, "#!/bin/sh\n"));
        BT_CHECK(proc_script_install_bundled(exe) == 0);
        BT_CHECK(proc_script_status() != NULL
                 && strstr(proc_script_status(), "python3") != NULL);
        unlink(summon);
        return;
    }

    /* Present: a stub summon (run by python3) lists one entity and records
     * every respond argv. */
    snprintf(body, sizeof(body),
             "import sys\n"
             "if sys.argv[1:2] == ['get']:\n"
             "    print('id=77\\tlabel=stub')\n"
             "else:\n"
             "    open(%s, 'a').write(' '.join(sys.argv[1:]) + '\\n')\n",
             "'''" "%s" "'''");
    {
        char real[2400];
        snprintf(real, sizeof(real), body, log);
        BT_CHECK(bt_write(summon, real));
    }
    BT_CHECK(proc_script_install_bundled(exe) == 1);
    BT_CHECK(proc_script_active());
    BT_CHECK(proc_script_status() == NULL);
    psd_select_init();
    n = psd_select_collect(ents, 4);
    BT_CHECK(n == 1 && ents[0].pid == 77);
    proc_script_set_mode("dryrun");
    BT_CHECK(psd_proc_kill_checked(77, 0, "stub") == PSD_ACT_DONE);
    BT_CHECK(bt_wait_for(log, "delete 77 label=stub mode=dryrun", 5000));

    /* Broken: the bundled summon is not valid python -> no entities, and
     * the game can only send events (none to send: nothing is listed). */
    BT_CHECK(bt_write(summon, "this is not python (\n"));
    n = psd_select_collect(ents, 4);
    BT_CHECK(n == 0);
    BT_CHECK(proc_script_status() != NULL
             && strstr(proc_script_status(), "failed") != NULL);
    /* Summoner reporting an error (exit 0, "# summon error" only). */
    BT_CHECK(bt_write(summon, "print('# summon error: root is writable')\n"));
    BT_CHECK(proc_script_install_bundled(exe) == 1);
    n = psd_select_collect(ents, 4);
    BT_CHECK(n == 0);
    BT_CHECK(proc_script_status() != NULL
             && strstr(proc_script_status(), "failed") != NULL);
    BT_CHECK(psd_proc_kill_checked(77, 0, "stub") == PSD_ACT_GONE);

    /* Hung: a summon that never answers -> the poll times out, no
     * entities, and the reason is recorded once. */
    BT_CHECK(bt_write(summon, "import time\ntime.sleep(30)\n"));
    n = psd_select_collect(ents, 4);
    BT_CHECK(n == 0);
    BT_CHECK(proc_script_status() != NULL
             && strstr(proc_script_status(), "did not answer") != NULL);

    proc_backend_set(NULL);
    unlink(summon);
    unlink(log);
    rmdir(res);
    {
        char p[1100];
        snprintf(p, sizeof(p), "%s/Contents/Resources", app);
        rmdir(p);
        rmdir(macos);
        snprintf(p, sizeof(p), "%s/Contents", app);
        rmdir(p);
        rmdir(app);
        snprintf(p, sizeof(p), "%s/bundle test", dir);
        rmdir(p);
    }
    (void) rmdir(dir);
}

void summoner_bundle_tests(int *checks_run, int *checks_failed)
{
    bt_run = checks_run;
    bt_failed = checks_failed;
    test_default_backend_is_inert();
    test_bundle_path();
    test_python_choice();
    test_sh_quote();
    test_bundled_summoner();
}
