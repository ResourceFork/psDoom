/*
 * psDoom -- user-tunable options (implementation). See psdoom_options.h.
 */

#include "psdoom_options.h"

#include "m_argv.h"   /* M_ParmExists */

/* ------------------------------------------------------------------ state */

int psdoom_kill_policy = PSD_KILL_RENICE;   /* SAFE default: renice, never kill
                                             * (opt into real kills via the menu
                                             * or the -psdoom-live flag)       */
int psdoom_all_users   = 0;                 /* default: our own processes     */
int psdoom_show_labels = 1;                 /* default: labels on             */
int psdoom_label_range = PSD_LABELS_NORMAL;

/* ------------------------------------------------------------------ startup */

void psdoom_options_parse_args(void)
{
    if (M_ParmExists("-psdoom-safe"))
    {
        psdoom_kill_policy = PSD_KILL_RENICE;    /* wound only, never kill     */
    }
    if (M_ParmExists("-psdoom-simulate"))
    {
        psdoom_kill_policy = PSD_KILL_SIMULATE;  /* no real effect at all      */
    }
    if (M_ParmExists("-psdoom-live"))
    {
        psdoom_kill_policy = PSD_KILL_LIVE;      /* opt in: really kill        */
    }
    if (M_ParmExists("-psdoom-allusers"))
    {
        psdoom_all_users = 1;
    }
    if (M_ParmExists("-psdoom-nolabels"))
    {
        psdoom_show_labels = 0;
    }
}

/* ------------------------------------------------------------------ queries */

int psdoom_should_kill(void)
{
    return psdoom_kill_policy == PSD_KILL_LIVE;
}

int psdoom_should_renice(void)
{
    return psdoom_kill_policy == PSD_KILL_LIVE
        || psdoom_kill_policy == PSD_KILL_RENICE;
}

int psdoom_labels_enabled(void)
{
    return psdoom_show_labels != 0;
}

int psdoom_all_users_enabled(void)
{
    return psdoom_all_users != 0;
}

int psdoom_label_min_scale(void)
{
    switch (psdoom_label_range)
    {
        case PSD_LABELS_NEAR:   return 16000;
        case PSD_LABELS_FAR:    return 3000;
        case PSD_LABELS_ALL:    return 0;
        case PSD_LABELS_NORMAL:
        default:                return 8000;
    }
}

/* ------------------------------------------------------------------ mutators */

static void psd_cycle(int *value, int choice, int count)
{
    if (choice == 1)
    {
        *value += 1;
    }
    else
    {
        *value += count - 1;
    }
    *value %= count;
}

void psdoom_opt_cycle_killpolicy(int choice)
{
    psd_cycle(&psdoom_kill_policy, choice, PSD_KILL_NUM);
}

void psdoom_opt_toggle_allusers(int choice)
{
    (void) choice;
    psdoom_all_users = !psdoom_all_users;
}

void psdoom_opt_toggle_labels(int choice)
{
    (void) choice;
    psdoom_show_labels = !psdoom_show_labels;
}

void psdoom_opt_cycle_labelrange(int choice)
{
    psd_cycle(&psdoom_label_range, choice, PSD_LABELRANGE_NUM);
}
