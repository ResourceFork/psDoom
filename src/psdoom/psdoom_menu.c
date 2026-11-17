/*
 * psDoom -- in-game options menu (implementation).
 *
 * A self-contained Crispy-style menu page, kept entirely out of the engine's
 * m_menu.c. It reuses the engine's menu machinery (menu_t/menuitem_t via
 * m_menu.h and the standard skull cursor) but defines its own page, drawing and
 * the toggle wiring here. The settings themselves live in psdoom_options.
 */

#include "psdoom_menu.h"
#include "psdoom_options.h"
#include "psdoom_effect.h"  /* No effect / Wounds only / Wounds and kills, and help */
#include "proc_script.h"   /* "Sort by" from the script backend */

#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "m_menu.h"     /* menu_t, menuitem_t, M_SetupNextMenu, M_WriteText */
#include "v_video.h"    /* dp_translation                                  */
#include "v_trans.h"    /* crstr[], CR_*                                    */

/* Row spacing must match the engine's LINEHEIGHT so the skull cursor (drawn by
 * the engine at currentMenu->y + itemOn*LINEHEIGHT) lines up with our rows. */
#define PSD_MENU_X   48
#define PSD_MENU_Y   48
#define PSD_MENU_LH  16

/* Rows (also the menu-item indices the skull cursor lands on). */
enum
{
    psd_row_killpolicy,
    psd_row_monstercap,
    psd_row_classify,
    psd_row_showlabels,
    psd_row_showpid,
    psd_row_labelrange,
    psd_row_count
};

static const char *const psd_labelrange_names[PSD_LABELRANGE_NUM] =
{
    "near",
    "normal",
    "far",
    "all",
};

static const char *const psd_classify_names[PSD_CLASSIFY_NUM] =
{
    "memory",
    "CPU load",
};

static void M_DrawPsDoom(void);

/* The engine's Options menu; we return to it on "back". */
extern menu_t OptionsDef;
/* The engine's menu cursor row and text width (m_menu.c). */
extern short itemOn;
int M_StringWidth(const char *string);

/* One help line under the rows, centred, cut to fit the 320-wide screen. */
static void PSD_HelpLine(int y, const char *text)
{
    char buf[PSD_EFFECT_HELP_MAX + 8];
    size_t n;
    int w;

    snprintf(buf, sizeof(buf), "%s%s", crstr[CR_GRAY], text);
    n = strlen(buf);
    while ((w = M_StringWidth(buf)) > 312 && n > strlen(crstr[CR_GRAY]))
    {
        buf[--n] = '\0';
    }
    M_WriteText((320 - w) / 2, y, buf);
}

static menuitem_t PsDoomMenu[] =
{
    /* status 3 = left/right/enter cycles, no mouse-x (Crispy convention). */
    {3, "", psdoom_opt_cycle_killpolicy,  'k'},
    {3, "", psdoom_opt_adjust_monstercap, 'm'},
    {3, "", psdoom_opt_cycle_classifyby,  'c'},
    {3, "", psdoom_opt_toggle_labels,     'l'},
    {3, "", psdoom_opt_toggle_pid,        'i'},
    {3, "", psdoom_opt_cycle_labelrange,  'd'},
};

static menu_t PsDoomDef =
{
    psd_row_count,
    &OptionsDef,
    PsDoomMenu,
    M_DrawPsDoom,
    PSD_MENU_X, PSD_MENU_Y,
    0
};

/* One "Label: value" row, greyed when disabled and green when highlighted,
 * mirroring the Crispness menu's look. */
static void PSD_Row(int row, const char *label, const char *value,
                    int enabled, int highlight)
{
    char buf[80];

    snprintf(buf, sizeof(buf), "%s%s: %s%s",
             enabled ? crstr[CR_NONE] : crstr[CR_DARK], label,
             enabled ? (highlight ? crstr[CR_GREEN] : crstr[CR_DARK])
                     : crstr[CR_DARK],
             value);
    M_WriteText(PSD_MENU_X, PSD_MENU_Y + row * PSD_MENU_LH, buf);
}

static void M_DrawPsDoom(void)
{
    char title[48];

    snprintf(title, sizeof(title), "%spsDoom Options", crstr[CR_GOLD]);
    M_WriteText(PSD_MENU_X, PSD_MENU_Y - 24, title);

    /* Kill policy: colour the value by danger -- red when it really kills. */
    {
        const char *vc =
            psdoom_kill_policy == PSD_KILL_LIVE     ? crstr[CR_RED]  :
            psdoom_kill_policy == PSD_KILL_SIMULATE ? crstr[CR_GRAY] :
                                                      crstr[CR_GREEN];
        char buf[80];
        int  y = PSD_MENU_Y + psd_row_killpolicy * PSD_MENU_LH;

        /* Title and value together are wider than the screen: the value
         * goes on its own line, inside the row's 16 pixels. */
        snprintf(buf, sizeof(buf), "%s%s:", crstr[CR_NONE], PSD_EFFECT_TITLE);
        M_WriteText(PSD_MENU_X, y, buf);
        snprintf(buf, sizeof(buf), "%s%s", vc,
                 psdoom_effect_name(psdoom_kill_policy));
        M_WriteText(PSD_MENU_X + 16, y + 8, buf);
    }

    /* Max live monsters: a left/right slider clamped to 1..35. */
    {
        char numbuf[16];
        snprintf(numbuf, sizeof(numbuf), "%d", psdoom_monster_cap);
        PSD_Row(psd_row_monstercap, "Max live monsters", numbuf, 1, 1);
    }

    if (proc_script_active() && proc_script_sort_count() > 0)
    {
        const char *label = proc_script_sort_label();
        PSD_Row(psd_row_classify, "Sort by", label ? label : "default", 1, 1);
    }
    else
    {
        PSD_Row(psd_row_classify, "Monster size by",
                psd_classify_names[psdoom_classify_by], 1, 1);
    }

    PSD_Row(psd_row_showlabels, "Show process labels",
            psdoom_show_labels ? "on" : "off", 1, psdoom_show_labels);
    PSD_Row(psd_row_showpid, "Show PID in label",
            psdoom_show_pid ? "on" : "off", psdoom_show_labels, psdoom_show_pid);
    PSD_Row(psd_row_labelrange, "Label draw distance",
            psd_labelrange_names[psdoom_label_range], psdoom_show_labels, 1);

    /* On the effect row, say what a wound and a kill do, in the plugin's
     * own words where it gives them. */
    if (itemOn == psd_row_killpolicy)
    {
        char l1[PSD_EFFECT_HELP_MAX];
        char l2[PSD_EFFECT_HELP_MAX];
        int  y = PSD_MENU_Y + psd_row_count * PSD_MENU_LH + 8;

        psdoom_effect_help(psdoom_kill_policy,
                           proc_script_active() ? proc_script_effect(0) : NULL,
                           proc_script_active() ? proc_script_effect(1) : NULL,
                           l1, l2, sizeof(l1));
        PSD_HelpLine(y, l1);
        if (l2[0] != '\0')
        {
            PSD_HelpLine(y + 10, l2);
        }
    }

    dp_translation = NULL;
}

void M_PsDoom(int choice)
{
    (void) choice;
    M_SetupNextMenu(&PsDoomDef);
}
