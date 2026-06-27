/*
 * psDoom -- user-tunable options.
 *
 * Holds the small set of runtime settings the player can change (in the
 * in-game psDoom menu, on the command line, or via the saved config file) and
 * the policy queries the game logic asks of them. Deliberately engine-free
 * (no Doom headers, getters return int) so it can be shared by the pure
 * proc_select layer as well as the game and menu code.
 *
 * Storage is plain int globals so they can be bound to the config file with
 * M_BindIntVariable (see d_main.c) and persisted across runs.
 */

#ifndef PSDOOM_OPTIONS_H
#define PSDOOM_OPTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* What happens to the real process behind a monster. Ordered by increasing
 * real-world impact. The default is RENICE: safe (no process is ever killed)
 * but still functional. Real killing is opt-in (psDoom menu or -psdoom-live). */
enum
{
    PSD_KILL_SIMULATE = 0, /* no real process effect at all (pure game)       */
    PSD_KILL_RENICE,       /* wound -> renice; monster death -> nothing (safe)*/
    PSD_KILL_LIVE,         /* wound -> renice; monster death -> SIGTERM        */
    PSD_KILL_NUM
};

/* Label draw distance -> minimum (320-space) vissprite scale to label. */
enum
{
    PSD_LABELS_NEAR = 0,  /* only very close monsters                         */
    PSD_LABELS_NORMAL,
    PSD_LABELS_FAR,
    PSD_LABELS_ALL,       /* label everything, however distant                */
    PSD_LABELRANGE_NUM
};

/* Live-monster cap range (the psDoom menu slider). */
#define PSD_MONSTER_CAP_MIN 5
#define PSD_MONSTER_CAP_MAX 35

/* Which process metric drives monster toughness. */
enum
{
    PSD_CLASSIFY_MEM = 0,  /* memory footprint (default)                      */
    PSD_CLASSIFY_CPU,      /* recent CPU load                                 */
    PSD_CLASSIFY_NUM
};

/* Persisted settings (bound to the config file in d_main.c; changed in the
 * psDoom menu). Plain ints for M_BindIntVariable. */
extern int psdoom_kill_policy;    /* PSD_KILL_*               */
extern int psdoom_monster_cap;    /* live-monster cap (5..35) */
extern int psdoom_classify_by;    /* PSD_CLASSIFY_*           */
extern int psdoom_all_users;      /* 0 = our uid only         */
extern int psdoom_show_labels;    /* 0 / 1                    */
extern int psdoom_label_range;    /* PSD_LABELS_*             */

/* Apply -psdoom-* command-line overrides. Call once at startup, after the
 * config file has been loaded (so the flags win). */
void psdoom_options_parse_args(void);

/* Policy queries used by the game logic (1 = yes). */
int psdoom_should_kill(void);       /* send SIGTERM when a monster dies?      */
int psdoom_should_renice(void);     /* renice on a non-fatal hit?             */
int psdoom_labels_enabled(void);
int psdoom_all_users_enabled(void);
int psdoom_label_min_scale(void);   /* 320-space scale gate for labels        */

/* Menu mutators (match the menu routine signature void(int choice)). For the
 * cycle handlers, choice 1 advances, anything else goes back. */
void psdoom_opt_cycle_killpolicy(int choice);
void psdoom_opt_adjust_monstercap(int choice);   /* slider: clamp 5..35 */
void psdoom_opt_cycle_classifyby(int choice);
void psdoom_opt_toggle_allusers(int choice);
void psdoom_opt_toggle_labels(int choice);
void psdoom_opt_cycle_labelrange(int choice);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_OPTIONS_H */
