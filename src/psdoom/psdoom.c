/*
 * psDoom -- process-management layer (the "monster creator").
 *
 * Each process the player should see is represented by a monster spawned in the
 * E1M1 "hidden courtyard" (coordinates taken from the original psDoom). Which
 * processes those are -- ownership, safety exclusions, relevance ranking and
 * truncation -- is decided entirely by the isolated proc_select layer; this
 * file just turns the curated collection it returns into monsters and keeps
 * them reconciled with the live set. Wounding a process-monster renices its
 * process; killing it sends SIGTERM.
 *
 *     proc_macos  ->  proc_select  ->  psdoom (here)
 *     (read OS)       (triage)         (spawn/reconcile/wound/kill/label)
 *
 * Engine integration points (see third_party/README.md):
 *   psdoom_init   <- D_DoomMain
 *   psdoom_sync   <- P_Ticker (each tic; self-throttled)
 *   psdoom_wound  <- P_DamageMobj (survival path)
 *   psdoom_kill   <- P_KillMobj
 *
 * Known limitation: the E1M1 courtyard only fits a few dozen monsters, so on a
 * machine with hundreds of processes most collide and are retried each sync
 * (cheap, but they don't all appear). The original shipped custom WADs with
 * large pens to solve this; that's future work.
 */

#include "psdoom.h"
#include "proc_macos.h"     /* current uid, renice, kill */
#include "proc_select.h"    /* curated process collection */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "doomtype.h"
#include "doomdef.h"
#include "d_mode.h"
#include "doomstat.h"
#include "info.h"
#include "tables.h"
#include "p_local.h"
#include "p_mobj.h"

/* For drawing process labels into the framebuffer. */
#include "hu_stuff.h"   /* hu_font[], HU_FONTSTART, HU_FONTSIZE */
#include "v_video.h"    /* V_DrawPatch                          */
#include "i_swap.h"     /* SHORT                                */
#include "i_video.h"    /* ORIGHEIGHT                           */
#include "crispy.h"     /* crispy->hires                        */
#include "r_main.h"     /* viewwindowx, viewwindowy             */

/* ------------------------------------------------------------------ config */

#define PSD_SYNC_INTERVAL 35    /* tics between syncs (~1s at 35Hz)            */
#define PSD_WOUND_NICE    4     /* priority drop per non-fatal hit             */

/* Truncation cap handed to the triage layer: at most this many (most-relevant)
 * processes are candidates for monsters each sync. Comfortably above what the
 * courtyard fits, so the relevance cut -- not this number -- is what the player
 * notices on a busy machine. Tunable. */
#define PSD_MAX_MONSTERS  64

/* Label drawing. Scale is normalized to 320-space (>> hires) before this
 * comparison, so the threshold is resolution-independent. The original psDoom
 * gated at 18000; we start more lenient so labels are easy to see, then it can
 * be tuned up to reduce clutter. */
#define PSD_LABEL_MIN_SCALE 8000
/* Highest Y (320-space) we let the top label row sit, so both rows stay above
 * the 32px status bar (200 - 32 - 16). */
#define PSD_LABEL_MAX_Y     (ORIGHEIGHT - 32 - 16)
#define PSD_LABEL_LINE_H    8   /* vertical gap between the PID and name rows  */
#define PSD_LABEL_SPACE_W   4   /* width of a space / unprintable glyph        */

/* ------------------------------------------------------------------- state */

static boolean    psd_enabled;
static int        psd_next_sync;               /* leveltime gate              */
static psd_proc_t psd_selected[PSD_MAX_MONSTERS]; /* curated set, reused each sync */

/* ------------------------------------------------------------------ helpers */

/* The original spawns process-monsters only on Doom 1 E1M1 (and Doom 2
 * MAP01). We support E1M1 for now. */
static boolean PSD_OnSpawnLevel(void)
{
    return gamestate == GS_LEVEL
        && gamemission == doom
        && gameepisode == 1
        && gamemap == 1;
}

/* True if `pid` is in this sync's curated collection. */
static boolean PSD_InSelected(int pid, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        if (psd_selected[i].pid == pid)
        {
            return true;
        }
    }
    return false;
}

/* Return the live process-monster for `pid`, or NULL. */
static mobj_t *PSD_FindMonster(int pid)
{
    thinker_t *th;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1) P_MobjThinker)
        {
            mobj_t *mo = (mobj_t *) th;
            if (mo->psd_pid == pid)
            {
                return mo;
            }
        }
    }
    return NULL;
}

/* Spawn a monster for `p` at its courtyard grid cell. Returns true if placed. */
static boolean PSD_SpawnMonster(const psd_proc_t *p)
{
    fixed_t x;
    fixed_t y;
    mobjtype_t type;
    mobj_t *mo;

    /* E1M1 "hidden courtyard" grid, from the original psDoom's add_new_process(). */
    x = (1800 + (p->pid % 16) * 40) << FRACBITS;
    y = (-3600 + (p->pid % 10) * 40) << FRACBITS;

    type = p->is_daemon ? MT_SERGEANT   /* daemons -> pink demons          */
                        : MT_SHOTGUY;   /* tty-owning procs -> shotgun guys */

    mo = P_SpawnMobj(x, y, ONFLOORZ, type);
    if (mo == NULL)
    {
        return false;
    }

    /* Process-monsters don't count toward the level kill %. */
    mo->flags &= ~MF_COUNTKILL;
    mo->psd_pid = p->pid;
    strncpy(mo->psd_name, p->name, sizeof(mo->psd_name) - 1);
    mo->psd_name[sizeof(mo->psd_name) - 1] = '\0';

    /* If it landed on top of something, drop it and retry next sync. */
    if (!P_CheckPosition(mo, mo->x, mo->y))
    {
        P_RemoveMobj(mo);
        return false;
    }

    return true;
}

/* ---------------------------------------------------------------- public API */

void psdoom_init(void)
{
    psd_select_init();
    psd_next_sync = 0;
    psd_enabled   = true;

    fprintf(stderr, "psDoom: process management active (uid %u, pid %d)\n",
            proc_macos_current_uid(), (int) getpid());
}

void psdoom_sync(void)
{
    thinker_t *th;
    thinker_t *next;
    int n;
    int i;
    int spawned = 0;
    int removed = 0;

    if (!psd_enabled || !PSD_OnSpawnLevel())
    {
        return;
    }

    /* Self-throttle: reconcile roughly once a second. */
    if (leveltime < psd_next_sync)
    {
        return;
    }
    psd_next_sync = leveltime + PSD_SYNC_INTERVAL;

    /* Ask the triage layer for the curated, ranked, truncated collection. */
    n = psd_select_collect(psd_selected, PSD_MAX_MONSTERS);

    /* 1) Retire monsters no longer in the curated set (process exited, or it
     *    dropped below the relevance cut). */
    for (th = thinkercap.next; th != &thinkercap; th = next)
    {
        next = th->next;

        if (th->function.acp1 == (actionf_p1) P_MobjThinker)
        {
            mobj_t *mo = (mobj_t *) th;
            if (mo->psd_pid != 0 && !PSD_InSelected(mo->psd_pid, n))
            {
                P_RemoveMobj(mo);
                removed++;
            }
        }
    }

    /* 2) Spawn a monster for each curated process not already represented. */
    for (i = 0; i < n; i++)
    {
        const psd_proc_t *p = &psd_selected[i];

        if (PSD_FindMonster(p->pid) == NULL && PSD_SpawnMonster(p))
        {
            spawned++;
        }
    }

    if (spawned != 0 || removed != 0)
    {
        fprintf(stderr, "psDoom: +%d / -%d process-monsters\n", spawned, removed);
    }
}

void psdoom_wound(struct mobj_s *target)
{
    mobj_t *mo = (mobj_t *) target;

    if (mo != NULL && mo->psd_pid > 1)
    {
        proc_macos_renice(mo->psd_pid, PSD_WOUND_NICE);
    }
}

void psdoom_kill(struct mobj_s *target)
{
    mobj_t *mo = (mobj_t *) target;

    if (mo != NULL && mo->psd_pid > 1)
    {
        proc_macos_kill(mo->psd_pid);
    }
}

/* --------------------------------------------------------------- label draw */

/* Draw `str` at logical (320-space) position (x, y) using the HUD font. The
 * HUD font only carries uppercase letters, digits and some punctuation, so
 * lowercase is folded to uppercase and unknown glyphs advance as a space.
 * V_DrawPatch clips safely at every screen edge, so off-screen x/y is fine. */
static void PSD_DrawHUString(int x, int y, const char *str)
{
    const char *p;
    int cx = x;

    for (p = str; *p != '\0'; p++)
    {
        int c = toupper((unsigned char) *p);

        if (c == ' ')
        {
            cx += PSD_LABEL_SPACE_W;
            continue;
        }

        c -= HU_FONTSTART;
        if (c < 0 || c >= HU_FONTSIZE || hu_font[c] == NULL)
        {
            cx += PSD_LABEL_SPACE_W;
            continue;
        }

        V_DrawPatch(cx, y, hu_font[c]);
        cx += SHORT(hu_font[c]->width);
    }
}

void psdoom_draw_label(int x1_fb, int top_fb, int scale, int pid,
                       const char *name)
{
    const int hires = crispy->hires;
    int x;
    int y;
    char buf[16];

    /* Skip distant sprites to keep the screen readable. */
    if ((scale >> hires) < PSD_LABEL_MIN_SCALE)
    {
        return;
    }

    /* The renderer's x1/top are relative to the 3D view window's top-left, in
     * framebuffer pixels (320 << hires). Add the view-window origin to get
     * absolute framebuffer coords, then fold back to the 320x200 logical space
     * V_DrawPatch expects. Including viewwindowx/y keeps labels aligned when a
     * smaller screen size / the status bar insets the 3D view. */
    x = (x1_fb + viewwindowx) >> hires;
    y = (top_fb + viewwindowy) >> hires;

    if (y < 0)
    {
        y = 0;
    }
    if (y > PSD_LABEL_MAX_Y)
    {
        y = PSD_LABEL_MAX_Y;
    }

    snprintf(buf, sizeof(buf), "%d", pid);
    PSD_DrawHUString(x, y, buf);

    if (name != NULL)
    {
        PSD_DrawHUString(x, y + PSD_LABEL_LINE_H, name);
    }
}
