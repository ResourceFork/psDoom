/*
 * psDoom -- the words the player sees for the effect setting (pure).
 *
 * The setting is "Off", "Wounds only" or "Wounds and kills". The protocol's
 * words (dryrun, post-only, delete) never reach the screen: the help line says
 * what a wound and a kill do in the plugin's own plain words, when it gives
 * them (the `# effect` header lines; see docs/script-backend.md).
 */
#ifndef PSDOOM_EFFECT_H
#define PSDOOM_EFFECT_H

#include <stddef.h>

/* A help line's room, terminator included. */
#define PSD_EFFECT_HELP_MAX 64

/* The setting's name for policy (PSD_KILL_*); "Off" for anything else. */
const char *psdoom_effect_name(int policy);

/* The setting a launch starts at, given the saved one: "Wounds and kills"
 * lasts only until quit (as in psQuake), so a saved one starts at Wounds
 * only; Off and Wounds only carry over; anything unknown starts at Wounds
 * only. The -psdoom-live flag still opts in for one run. */
int psdoom_effect_at_launch(int saved);

/* The setting's help: what a wound (line1) and a kill (line2) do under
 * policy, from the plugin's words (NULL or "" when it gave none). line2 is ""
 * when one line says it all. */
void psdoom_effect_help(int policy, const char *wound, const char *kill,
                        char *line1, char *line2, size_t cap);

#endif
