/*
 * psDoom -- the effect setting's words: the names the menu shows and its help
 * line. Pure; no process is touched. Registered from window_state_test.c's
 * main (psDoom only: psQuake has its own menu). The `# effect` header lines
 * the plugin's words arrive in are tested in script_request_test.c.
 */

#include "psdoom_effect.h"
#include "psdoom_options.h"

#include <stdio.h>
#include <string.h>

extern int checks_run;
extern int checks_failed;

#define EF_CHECK(cond)                                                       \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static void test_names(void)
{
    EF_CHECK(strcmp(psdoom_effect_name(PSD_KILL_SIMULATE), "Off") == 0);
    EF_CHECK(strcmp(psdoom_effect_name(PSD_KILL_RENICE), "Wounds only") == 0);
    EF_CHECK(strcmp(psdoom_effect_name(PSD_KILL_LIVE), "Wounds and kills")
             == 0);
    EF_CHECK(strcmp(psdoom_effect_name(-1), "Off") == 0);
    EF_CHECK(strcmp(psdoom_effect_name(PSD_KILL_NUM), "Off") == 0);
}

static void test_help(void)
{
    char a[PSD_EFFECT_HELP_MAX];
    char b[PSD_EFFECT_HELP_MAX];

    psdoom_effect_help(PSD_KILL_SIMULATE, "lowers its priority",
                       "quits the process", a, b, sizeof(a));
    EF_CHECK(strcmp(a, "Wounds and kills do nothing") == 0);
    EF_CHECK(b[0] == '\0');

    psdoom_effect_help(PSD_KILL_RENICE, "lowers its priority",
                       "quits the process", a, b, sizeof(a));
    EF_CHECK(strcmp(a, "Wound: lowers its priority") == 0);
    EF_CHECK(strcmp(b, "Kill: nothing") == 0);

    psdoom_effect_help(PSD_KILL_LIVE, "lowers its priority",
                       "quits the process", a, b, sizeof(a));
    EF_CHECK(strcmp(a, "Wound: lowers its priority") == 0);
    EF_CHECK(strcmp(b, "Kill: quits the process") == 0);

    /* A plugin that gives no words still gets a plain line. */
    psdoom_effect_help(PSD_KILL_LIVE, NULL, "", a, b, sizeof(a));
    EF_CHECK(strcmp(a, "Wound: acts on it") == 0);
    EF_CHECK(strcmp(b, "Kill: acts on it") == 0);
}

void test_effect_setting_words(void)
{
    test_names();
    test_help();
}
