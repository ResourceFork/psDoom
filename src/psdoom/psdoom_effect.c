/*
 * psDoom -- the words the player sees for the effect setting. See
 * psdoom_effect.h.
 */

#include "psdoom_effect.h"
#include "psdoom_options.h"

#include <stdio.h>

static const char *const psd_effect_names[PSD_KILL_NUM] =
{
    "Off",
    "Wounds only",
    "Wounds and kills",
};

const char *psdoom_effect_name(int policy)
{
    if (policy < 0 || policy >= PSD_KILL_NUM)
    {
        return psd_effect_names[PSD_KILL_SIMULATE];
    }
    return psd_effect_names[policy];
}

int psdoom_effect_at_launch(int saved)
{
    if (saved == PSD_KILL_SIMULATE || saved == PSD_KILL_RENICE)
    {
        return saved;
    }
    return PSD_KILL_RENICE;
}

/* The plugin's words, or a plain fallback when it gave none. */
static const char *psd_effect_or(const char *words)
{
    return (words != NULL && words[0] != '\0') ? words : "acts on it";
}

void psdoom_effect_help(int policy, const char *wound, const char *kill,
                        char *line1, char *line2, size_t cap)
{
    if (line1 == NULL || line2 == NULL || cap == 0)
    {
        return;
    }
    line2[0] = '\0';
    if (policy == PSD_KILL_RENICE)
    {
        snprintf(line1, cap, "Wound: %s", psd_effect_or(wound));
        snprintf(line2, cap, "Kill: nothing");
    }
    else if (policy == PSD_KILL_LIVE)
    {
        snprintf(line1, cap, "Wound: %s", psd_effect_or(wound));
        snprintf(line2, cap, "Kill: %s", psd_effect_or(kill));
    }
    else
    {
        snprintf(line1, cap, "Wounds and kills do nothing");
    }
}
