/*
 * psDoom -- the poll's `limit` is the player's live-monster cap.
 *
 * The game asks the curating backend for exactly as many items as it shows
 * monsters, so the plugin's top N in its sort order are the monsters. A
 * monster whose process is no longer in that list teleports out (the
 * lifecycle tests), so lowering the cap prunes the lowest-ranked with no
 * new code and no event to the responder. This proves the cap reaches the
 * poll: after the config is read, and on the poll after each menu change.
 *
 * Only this test's own script under PSD_E2E_SCRATCH runs; nothing is
 * signalled. Registered from proc_select_test.c's main.
 */

#include "proc_backend.h"
#include "proc_script.h"
#include "proc_select.h"
#include "psdoom_options.h"
#include "psd_test_wait.h"

#include "m_argv.h"   /* boolean, M_ParmExists */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PSD_E2E_SCRATCH
#error "PSD_E2E_SCRATCH must be defined (CMakeLists.txt)"
#endif

/* psdoom_options_parse_args reads the command line; this binary has none
 * of the game's flags. */
boolean M_ParmExists(const char *check)
{
    (void) check;
    return false;
}

static int *pl_run;
static int *pl_failed;

#define PL_CHECK(cond)                                                       \
    do {                                                                     \
        (*pl_run)++;                                                         \
        if (!(cond)) {                                                       \
            (*pl_failed)++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static int pl_last_line(const char *path, char *out, size_t cap)
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

/* Poll once and return the words the poll was given, as "[...]". */
static void pl_poll(const char *args, char *last, size_t cap)
{
    psd_proc_t ents[8];

    (void) psd_test_collect_wait(ents, 8);
    (void) pl_last_line(args, last, cap);
}

static void test_poll_limit_is_the_monster_cap(void)
{
    const char *dir = PSD_E2E_SCRATCH;
    char        script[1024], args[1024], body[2048], cmd[1100];
    char        last[512];
    FILE       *f;
    int         i;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        PL_CHECK(!"cannot create PSD_E2E_SCRATCH");
        return;
    }
    snprintf(script, sizeof(script), "%s/pl-dims.sh", dir);
    snprintf(args,   sizeof(args),   "%s/pl-args.txt", dir);
    unlink(args);
    snprintf(body, sizeof(body),
             "#!/bin/sh\necho \"[$*]\" >> '%s'\n"
             "printf '# dim id=mem kind=numeric dir=desc roles=sort unit=b label=Memory\\n'\n"
             "printf 'id=21\\tlabel=a\\n'\n", args);
    f = fopen(script, "w");
    PL_CHECK(f != NULL);
    if (f == NULL)
    {
        return;
    }
    fputs(body, f);
    fclose(f);
    PL_CHECK(chmod(script, 0700) == 0);

    /* As psdoom_init: install the backend, then read the options (the
     * config already set the cap). */
    snprintf(cmd, sizeof(cmd), "'%s'", script);
    proc_script_install(cmd, NULL, 1000);
    psd_select_init();
    psdoom_monster_cap = 10;
    psdoom_options_parse_args();

    pl_poll(args, last, sizeof(last));   /* dimensions not known yet */
    PL_CHECK(strcmp(last, "[]") == 0);
    pl_poll(args, last, sizeof(last));
    PL_CHECK(strcmp(last, "[limit=10]") == 0);

    /* Lowering the cap in the menu reaches the next poll. */
    for (i = 0; i < 5; i++)
    {
        psdoom_opt_adjust_monstercap(0);
    }
    PL_CHECK(psdoom_monster_cap == 5);
    pl_poll(args, last, sizeof(last));
    PL_CHECK(strcmp(last, "[limit=5]") == 0);

    /* Down to the slider's floor, 1, which holds; a single monster is
     * asked for as limit=1. */
    for (i = 0; i < 10; i++)
    {
        psdoom_opt_adjust_monstercap(0);
    }
    PL_CHECK(PSD_MONSTER_CAP_MIN == 1);
    PL_CHECK(psdoom_monster_cap == PSD_MONSTER_CAP_MIN);
    pl_poll(args, last, sizeof(last));
    PL_CHECK(strcmp(last, "[limit=1]") == 0);

    /* So does raising it. */
    for (i = 0; i < 7; i++)
    {
        psdoom_opt_adjust_monstercap(1);
    }
    pl_poll(args, last, sizeof(last));
    PL_CHECK(strcmp(last, "[limit=8]") == 0);

    /* A config value out of range is clamped before it is sent: above 35
     * to 35, and 0 or a negative to 1. */
    psdoom_monster_cap = 99;
    psdoom_options_parse_args();
    pl_poll(args, last, sizeof(last));
    PL_CHECK(strcmp(last, "[limit=35]") == 0);
    psdoom_monster_cap = 0;
    psdoom_options_parse_args();
    pl_poll(args, last, sizeof(last));
    PL_CHECK(strcmp(last, "[limit=1]") == 0);
    psdoom_monster_cap = -3;
    psdoom_options_parse_args();
    pl_poll(args, last, sizeof(last));
    PL_CHECK(strcmp(last, "[limit=1]") == 0);

    psdoom_monster_cap = 10;   /* the shipped default, for later tests */
    proc_backend_set(NULL);
    unlink(script);
    unlink(args);
    (void) rmdir(dir);
}

void poll_limit_tests(int *checks_run, int *checks_failed)
{
    pl_run = checks_run;
    pl_failed = checks_failed;
    test_poll_limit_is_the_monster_cap();
}
