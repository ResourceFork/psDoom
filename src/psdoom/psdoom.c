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
#include "psdoom_options.h" /* user-tunable settings (menu / CLI / config) */

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
#include "r_state.h"    /* sprites[], numsprites (WAD sprite check) */

/* ------------------------------------------------------------------ config */

#define PSD_SYNC_INTERVAL 35    /* tics between syncs (~1s at 35Hz)            */
#define PSD_WOUND_NICE    4     /* priority drop per non-fatal hit             */

/* Candidate pool: the triage layer returns at most this many (most-relevant)
 * processes each sync. The live-monster cap below picks from these, so keep it
 * comfortably larger than the cap. */
#define PSD_CANDIDATE_CAP 64

/* The live-monster cap is now a user setting (psdoom_monster_cap, the menu
 * slider). The candidate pool stays comfortably larger than its max. */

/* Cap on remembered killed-this-level processes (see PSD_MarkKilled). */
#define PSD_MAX_KILLED    1024

/* Label layout. The minimum sprite scale to draw a label at now comes from the
 * "Label draw distance" option (psdoom_label_min_scale()); scale is normalized
 * to 320-space (>> hires) before the comparison so it is resolution-independent. */
/* Highest Y (320-space) we let the top label row sit, so both rows stay above
 * the 32px status bar (200 - 32 - 16). */
#define PSD_LABEL_MAX_Y     (ORIGHEIGHT - 32 - 16)
#define PSD_LABEL_LINE_H    8   /* vertical gap between the PID and name rows  */
#define PSD_LABEL_SPACE_W   4   /* width of a space / unprintable glyph        */

/* ------------------------------------------------------------------- state */

static boolean    psd_enabled;
static int        psd_next_sync;               /* leveltime gate              */
static psd_proc_t psd_selected[PSD_CANDIDATE_CAP]; /* curated set, reused each sync */

/* Processes the player has killed on the current level. Remembered (by pid +
 * name, so a reused pid isn't wrongly suppressed) so psdoom_sync doesn't
 * respawn them -- essential when the kill policy doesn't actually terminate the
 * process, and the masking death-corpse may be retired. Cleared on level restart. */
typedef struct
{
    int  pid;
    char name[16];
} psd_killed_t;

static psd_killed_t psd_killed[PSD_MAX_KILLED];
static int          psd_killed_count;
static int          psd_last_leveltime;   /* detect a level restart */

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

/* True if (pid, name) was killed this level. The name is compared with the same
 * truncation used when storing it, so it matches the mobj's psd_name. */
static boolean PSD_IsKilled(int pid, const char *name)
{
    int i;

    for (i = 0; i < psd_killed_count; i++)
    {
        if (psd_killed[i].pid == pid
            && strncmp(psd_killed[i].name, name,
                       sizeof(psd_killed[i].name) - 1) == 0)
        {
            return true;
        }
    }
    return false;
}

/* Remember that the monster for (pid, name) was killed, so psdoom_sync won't
 * respawn it this level. */
static void PSD_MarkKilled(int pid, const char *name)
{
    if (pid <= 1 || psd_killed_count >= PSD_MAX_KILLED || PSD_IsKilled(pid, name))
    {
        return;
    }

    psd_killed[psd_killed_count].pid = pid;
    strncpy(psd_killed[psd_killed_count].name, name,
            sizeof(psd_killed[psd_killed_count].name) - 1);
    psd_killed[psd_killed_count].name[sizeof(psd_killed[psd_killed_count].name) - 1] = '\0';
    psd_killed_count++;
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

/* Number of live (still-alive) process-monsters in the world. Corpses don't
 * count -- they're non-solid and don't block the player, so only living
 * monsters press against the live-monster cap. */
static int PSD_LiveMonsterCount(void)
{
    thinker_t *th;
    int count = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1) P_MobjThinker)
        {
            mobj_t *mo = (mobj_t *) th;
            if (mo->psd_pid != 0 && mo->health > 0)
            {
                count++;
            }
        }
    }
    return count;
}

/*
 * Map a process to a monster by its memory footprint: heavier process -> tougher
 * monster. The ladder is grounded (no floating types) so the courtyard grid can
 * place them, and ordered by Doom HP: Zombieman(20) -> Shotgun Guy(30) ->
 * Imp(60) -> Pinky(150) -> Hell Knight(500) -> Baron(1000) -> Cyberdemon(4000).
 * A process keeps getting wounded (reniced) until it dies, so a heavier process
 * is literally harder to kill -- exactly the intent. Thresholds are tunable.
 */
static const struct
{
    unsigned long long max_bytes;   /* upper bound (exclusive) for this tier */
    mobjtype_t         type;
} psd_monster_tiers[] =
{
    {   16ULL << 20, MT_POSSESSED },  /* <  16 MB : Zombieman    */
    {   64ULL << 20, MT_SHOTGUY  },   /* <  64 MB : Shotgun Guy  */
    {  192ULL << 20, MT_TROOP    },   /* < 192 MB : Imp          */
    {  512ULL << 20, MT_SERGEANT },   /* < 512 MB : Pinky Demon  */
    { 1536ULL << 20, MT_KNIGHT   },   /* < 1.5 GB : Hell Knight  */
    { 4096ULL << 20, MT_BRUISER  },   /* < 4   GB : Baron of Hell*/
    { ~0ULL,         MT_CYBORG   },   /* >= 4  GB : Cyberdemon   */
};

/* True if monster `type`'s sprite is present in the loaded WAD. Spawning a
 * monster the renderer can't draw is fatal (R_ProjectSprite I_Error), and the
 * shareware IWAD is missing several monster sprites, so classification checks
 * this and degrades to a drawable type. */
static boolean PSD_MonsterDrawable(mobjtype_t type)
{
    statenum_t  st  = mobjinfo[type].spawnstate;
    spritenum_t spr = states[st].sprite;

    return spr >= 0 && spr < numsprites && sprites[spr].numframes > 0;
}

static mobjtype_t PSD_ClassifyMonster(const psd_proc_t *p)
{
    const int ntiers = (int) (sizeof(psd_monster_tiers) / sizeof(psd_monster_tiers[0]));
    int ideal = ntiers - 1;
    int i;

    /* Toughest tier this footprint earns. */
    for (i = 0; i < ntiers; i++)
    {
        if (p->footprint < psd_monster_tiers[i].max_bytes)
        {
            ideal = i;
            break;
        }
    }

    /* Degrade to the first lighter tier whose sprite the loaded WAD actually
     * has. The shareware IWAD lacks Hell Knight / Cyberdemon sprites, and
     * spawning an undrawable monster is fatal (R_ProjectSprite I_Error), so a
     * heavy process caps at Baron there and uses the full ladder on a complete
     * WAD. Zombieman (the last fallback) exists in every Doom IWAD. */
    for (i = ideal; i > 0; i--)
    {
        if (PSD_MonsterDrawable(psd_monster_tiers[i].type))
        {
            return psd_monster_tiers[i].type;
        }
    }
    return psd_monster_tiers[0].type;
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

    type = PSD_ClassifyMonster(p);  /* memory footprint -> monster toughness */

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
    psdoom_options_parse_args();   /* let -psdoom-* flags override the config */
    psd_next_sync      = 0;
    psd_killed_count   = 0;
    psd_last_leveltime = 0;
    psd_enabled        = true;

    fprintf(stderr, "psDoom: process management active (uid %u, pid %d)\n",
            proc_macos_current_uid(), (int) getpid());
}

void psdoom_sync(void)
{
    thinker_t *th;
    thinker_t *next;
    int n;
    int i;
    int live;
    int spawned = 0;
    int removed = 0;

    if (!psd_enabled || !PSD_OnSpawnLevel())
    {
        return;
    }

    /* A fresh level restarts leveltime; forget this level's kills and resync
     * immediately (this also clears a stale throttle when re-entering E1M1). */
    if (leveltime < psd_last_leveltime)
    {
        psd_killed_count = 0;
        psd_next_sync    = 0;
    }
    psd_last_leveltime = leveltime;

    /* Self-throttle: reconcile roughly once a second. */
    if (leveltime < psd_next_sync)
    {
        return;
    }
    psd_next_sync = leveltime + PSD_SYNC_INTERVAL;

    /* Ask the triage layer for the curated, ranked, truncated collection. */
    n = psd_select_collect(psd_selected, PSD_CANDIDATE_CAP);

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

    /* 2) Spawn a monster for each curated process not already represented,
     *    unless the player already killed it this level (don't respawn kills),
     *    and only up to the live-monster cap so the level stays playable.
     *    psd_selected is relevance-ordered, so the cap keeps the most relevant. */
    live = PSD_LiveMonsterCount();
    for (i = 0; i < n && live < psdoom_monster_cap; i++)
    {
        const psd_proc_t *p = &psd_selected[i];

        if (PSD_IsKilled(p->pid, p->name))
        {
            continue;
        }
        if (PSD_FindMonster(p->pid) == NULL && PSD_SpawnMonster(p))
        {
            spawned++;
            live++;
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

    if (mo != NULL && mo->psd_pid > 1 && psdoom_should_renice())
    {
        proc_macos_renice(mo->psd_pid, PSD_WOUND_NICE);
    }
}

void psdoom_kill(struct mobj_s *target)
{
    mobj_t *mo = (mobj_t *) target;

    if (mo == NULL || mo->psd_pid <= 1)
    {
        return;
    }

    if (psdoom_should_kill())
    {
        proc_macos_kill(mo->psd_pid);
    }

    /* Remember it so psdoom_sync won't respawn it -- crucial when the kill
     * policy leaves the real process running. */
    PSD_MarkKilled(mo->psd_pid, mo->psd_name);
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

    if (!psdoom_labels_enabled())
    {
        return;
    }

    /* Skip distant sprites to keep the screen readable (gate from the
     * "Label draw distance" option). */
    if ((scale >> hires) < psdoom_label_min_scale())
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
