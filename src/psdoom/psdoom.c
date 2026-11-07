/*
 * psDoom -- process-management layer (the "monster creator").
 *
 * Each selected process is represented by a monster spawned on the custom
 * intro arena's clear 11-by-7 grid. Which processes those are -- ownership,
 * safety exclusions, relevance ranking and truncation -- is decided entirely
 * by the isolated proc_select layer; this file turns the curated collection it
 * returns into monsters and keeps them reconciled with the live set. Wounding
 * a process-monster renices its process; killing it sends SIGTERM.
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
 * The custom intro arena provides 77 deterministic spawn cells; the live
 * monster cap keeps the process visualization playable on busy systems.
 */

#include "psdoom.h"
#include "proc_backend.h"    /* current uid, renice, kill (via vtable) */
#include "proc_script.h"     /* optional external-script backend (env-driven) */
#include "proc_select.h"    /* curated process collection */
#include "psdoom_lifecycle.h" /* psd_pid tags; save and retire rules */
#include "psdoom_options.h" /* user-tunable settings (menu / CLI / config) */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "doomtype.h"
#include "doomdef.h"
#include "d_main.h"
#include "d_mode.h"
#include "doomstat.h"
#include "g_game.h"
#include "m_argv.h"
#include "m_misc.h"
#include "info.h"
#include "tables.h"
#include "p_extsaveg.h"
#include "p_local.h"
#include "p_mobj.h"
#include "p_saveg.h"
#include "sounds.h"     /* sfx_telept (morph fog cue)            */
#include "s_sound.h"    /* S_StartSound                          */

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

/* Live re-classification (hysteresis). Each sync a process-monster re-grades to
 * the tier its process currently earns -- but only once the new tier has "won"
 * a number of consecutive syncs. Promote quickly (a spike should show), demote
 * slowly (a brief dip shouldn't shrink a monster); this is what defeats the
 * frame-to-frame flapping that classifying-once-at-spawn was avoiding. */
#define PSD_REGRADE_UP_HOLD   2 /* syncs a tougher tier must persist to promote */
#define PSD_REGRADE_DOWN_HOLD 5 /* syncs a lighter tier must persist to demote  */

/* Fork-bomb swarms. A process whose live child count jumps by PSD_FORK_BURST in
 * one sync is forking hard; we spawn a capped swarm of inert Lost Souls around
 * its monster. Souls are tagged with PSD_SWARM_PID so every process code path
 * ignores them (they never signal/renice anything) -- they are a cosmetic
 * threat the player clears by shooting. A per-parent cooldown, a per-burst cap
 * and a global live ceiling together bound how many can pile up. */
/* PSD_SWARM_PID and PSD_GONE_PID (psd_pid tags) live in psdoom_lifecycle.h. */
#define PSD_FORK_BURST        5   /* child-count jump (per sync) that triggers */
#define PSD_SWARM_PER_BURST    6   /* max souls spawned per detected burst      */
#define PSD_MAX_SWARM_LIVE    24   /* global ceiling on live swarm souls        */
#define PSD_SWARM_COOLDOWN  (PSD_SYNC_INTERVAL * 4) /* tics before a parent re-swarms */

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

#ifndef PSD_BUILD_ID
#define PSD_BUILD_ID "0.1.1-label-centering-v2"
#endif

/* ------------------------------------------------------------------- state */

typedef enum
{
    PSD_INTRO_UNAVAILABLE,
    PSD_INTRO_READY,
    PSD_INTRO_PENDING,
    PSD_INTRO_ACTIVE,
} psd_intro_state_t;

static boolean           psd_enabled;
static psd_intro_state_t psd_intro_state;
static boolean           psd_first_active_collection;
static int        psd_intro_smoke_stage;
static int        psd_intro_smoke_campaign_collections;
static int        psd_collect_count;
static int        psd_backend_action_count;
static int        psd_next_sync;               /* leveltime gate              */
static psd_proc_t psd_selected[PSD_CANDIDATE_CAP]; /* curated set, reused each sync */

/* Processes the player has killed on the current level. Remembered (by pid +
 * start token + name, so a reused pid isn't wrongly suppressed) so psdoom_sync
 * doesn't respawn them -- essential when the kill policy doesn't actually
 * terminate the process, and the masking death-corpse may be retired. Cleared
 * on level restart. */
typedef struct
{
    int                pid;
    unsigned long long start;
    char               name[16];
} psd_killed_t;

/* Identity names are compared over PSD_PROC_IDENT_NAME_LEN bytes; the monster
 * and the killed list must be able to hold that many plus the terminator. */
typedef char psd_check_ident_len_mobj[
    (sizeof(((mobj_t *) 0)->psd_name) > PSD_PROC_IDENT_NAME_LEN) ? 1 : -1];
typedef char psd_check_ident_len_killed[
    (sizeof(((psd_killed_t *) 0)->name) > PSD_PROC_IDENT_NAME_LEN) ? 1 : -1];

static psd_killed_t psd_killed[PSD_MAX_KILLED];
static int          psd_killed_count;
static int          psd_last_leveltime;   /* detect a level restart */

/* Per-monster re-classification hysteresis, keyed by pid (so no engine field is
 * needed). `pending` is the tier currently accumulating votes to change to;
 * `streak` counts the consecutive syncs that have voted for it. The table is
 * compacted against the live curated set each sync, so it stays bounded by the
 * candidate pool. */
typedef struct
{
    int        pid;
    mobjtype_t pending;
    int        streak;
} psd_grade_t;

static psd_grade_t psd_grade[PSD_CANDIDATE_CAP];
static int         psd_grade_count;

/* Fork-bomb tracking, keyed by pid. `children` is the parent's child count at
 * the previous sync (-1 = first sight, establish a baseline without firing);
 * `cooldown_until` is the earliest leveltime it may spawn another swarm. */
typedef struct
{
    int pid;
    int children;
    int cooldown_until;
} psd_fork_t;

static psd_fork_t psd_fork[PSD_CANDIDATE_CAP];
static int        psd_fork_count;
static int        psd_swarm_live;   /* live inert souls (death-decremented) */

/* ------------------------------------------------------------------ helpers */

/* Process synchronization belongs only to the one-time custom intro. Stock
 * campaign maps, including IWAD E1M1, must remain ordinary Doom levels. */
static boolean PSD_OnSpawnLevel(void)
{
    return gamestate == GS_LEVEL && psd_intro_state == PSD_INTRO_ACTIVE;
}

/* True if `pid` is in this sync's curated collection. Used only for the
 * pid-keyed bookkeeping tables; monsters use PSD_MonsterSelected. */
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

/* True if the process `mo` was spawned for -- the same pid, start token and
 * name, not merely the same pid -- is in this sync's curated collection. A pid
 * the OS has reused for another process does not count. */
static boolean PSD_MonsterSelected(const mobj_t *mo, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        if (psd_proc_same(&psd_selected[i], mo->psd_pid, mo->psd_start,
                          mo->psd_name))
        {
            return true;
        }
    }
    return false;
}

/* Log a checked action that made no backend call. Process names and script
 * labels are untrusted, so only printable ASCII reaches the terminal. */
static void PSD_LogSkipped(const mobj_t *mo, psd_act_result_t r,
                           const char *action)
{
    char safe[sizeof(mo->psd_name)];
    int  i;

    for (i = 0; i < (int) sizeof(safe) - 1 && mo->psd_name[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char) mo->psd_name[i];
        safe[i] = (c >= 0x20 && c < 0x7f) ? (char) c : '?';
    }
    safe[i] = '\0';

    fprintf(stderr, "psDoom: %s skipped: pid %d (%s) %s\n",
            action, mo->psd_pid, safe,
            r == PSD_ACT_REUSED ? "now belongs to another process; retiring its monster"
          : r == PSD_ACT_GONE   ? "has exited; retiring its monster"
                                : "could not be verified");
}

/* A checked renice/kill found that `mo`'s process has exited or its pid now
 * belongs to another process. At once the monster stops acting on processes
 * and loses its label (it can still fight); the next sync removes it. It is
 * not removed here because the engine is still using it inside
 * P_DamageMobj / P_KillMobj. */
static void PSD_MarkGone(mobj_t *mo, psd_act_result_t r, const char *action)
{
    PSD_LogSkipped(mo, r, action);
    mo->psd_pid = PSD_GONE_PID;
}

/* True if (pid, start, name) was killed this level. The name is compared with
 * the same truncation used when storing it, so it matches the mobj's psd_name.
 * Script entities have start 0, so for them this stays (id, label). */
static boolean PSD_IsKilled(int pid, unsigned long long start, const char *name)
{
    int i;

    for (i = 0; i < psd_killed_count; i++)
    {
        if (psd_killed[i].pid == pid && psd_killed[i].start == start
            && strncmp(psd_killed[i].name, name,
                       sizeof(psd_killed[i].name) - 1) == 0)
        {
            return true;
        }
    }
    return false;
}

/* Remember that the monster for (pid, start, name) was killed, so psdoom_sync
 * won't respawn it this level. */
static void PSD_MarkKilled(int pid, unsigned long long start, const char *name)
{
    if (pid <= 1 || psd_killed_count >= PSD_MAX_KILLED
        || PSD_IsKilled(pid, start, name))
    {
        return;
    }

    psd_killed[psd_killed_count].pid = pid;
    psd_killed[psd_killed_count].start = start;
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
            if (mo->psd_pid > 0 && mo->health > 0)
            {
                count++;
            }
        }
    }
    return count;
}

/*
 * Monster toughness ladder. A heavier / busier process -> a tougher monster.
 * The ladder is grounded (no floating types) so the courtyard grid can place
 * them, ordered by Doom HP: Zombieman(20) -> Shotgun Guy(30) -> Imp(60) ->
 * Pinky(150) -> Hell Knight(500) -> Baron(1000) -> Cyberdemon(4000). The
 * classifier uses the memory ladder or the CPU ladder per the "Classify by"
 * option; a process keeps getting wounded (reniced) until it dies, so a tougher
 * monster is literally harder to kill. Thresholds are tunable.
 */
typedef struct
{
    unsigned long long max;   /* upper bound (exclusive) for this tier */
    mobjtype_t         type;
} psd_tier_t;

/* By memory footprint (bytes). */
static const psd_tier_t psd_mem_tiers[] =
{
    {   16ULL << 20, MT_POSSESSED },  /* <  16 MB : Zombieman     */
    {   64ULL << 20, MT_SHOTGUY  },   /* <  64 MB : Shotgun Guy   */
    {  192ULL << 20, MT_TROOP    },   /* < 192 MB : Imp           */
    {  512ULL << 20, MT_SERGEANT },   /* < 512 MB : Pinky Demon   */
    { 1536ULL << 20, MT_KNIGHT   },   /* < 1.5 GB : Hell Knight   */
    { 4096ULL << 20, MT_BRUISER  },   /* < 4   GB : Baron of Hell */
    { ~0ULL,         MT_CYBORG   },   /* >= 4  GB : Cyberdemon    */
};

/* By CPU load (percent of one core; 100 = one full core, >100 multi-core). */
static const psd_tier_t psd_cpu_tiers[] =
{
    {     5, MT_POSSESSED },  /* <   5% : Zombieman (idle)     */
    {    25, MT_SHOTGUY  },   /* <  25% : Shotgun Guy          */
    {    60, MT_TROOP    },   /* <  60% : Imp                  */
    {   120, MT_SERGEANT },   /* < 120% : Pinky Demon (~1 core)*/
    {   250, MT_KNIGHT   },   /* < 250% : Hell Knight          */
    {   500, MT_BRUISER  },   /* < 500% : Baron of Hell        */
    { ~0ULL, MT_CYBORG   },   /* >=500% : Cyberdemon           */
};

/* True if monster `type`'s sprite is present in the loaded WAD. Spawning a
 * monster the renderer can't draw is fatal (R_ProjectSprite I_Error), and no
 * Doom 1-format IWAD has every ladder sprite (none has Hell Knight), so
 * classification checks this and degrades to a drawable type. */
static boolean PSD_MonsterDrawable(mobjtype_t type)
{
    statenum_t  st  = mobjinfo[type].spawnstate;
    spritenum_t spr = states[st].sprite;

    return spr >= 0 && spr < numsprites && sprites[spr].numframes > 0;
}

/* Set once a sync has played its teleport sound; cleared as each sync starts. */
static boolean psd_fog_sound_played;

/* Teleport fog and sound at (x, y, z), as the engine's teleporters do. The
 * sound plays from the fog, so it outlives a monster removed right after, and
 * only once per sync, so a burst of spawns or exits doesn't take every sound
 * channel from the player's own sounds. The fog is skipped if the WAD lacks
 * its sprite. */
static void PSD_TeleportFog(fixed_t x, fixed_t y, fixed_t z)
{
    if (PSD_MonsterDrawable(MT_TFOG))
    {
        mobj_t *fog = P_SpawnMobj(x, y, z, MT_TFOG);

        if (!psd_fog_sound_played)
        {
            S_StartSound(fog, sfx_telept);
            psd_fog_sound_played = true;
        }
    }
}

/* Drop every reference the game holds to `mo` before it is removed, so no
 * monster, missile, player or sector keeps a pointer to freed memory. The
 * engine already copes with these being NULL: loading a vanilla save leaves
 * them NULL. */
static void PSD_ForgetMobj(const mobj_t *mo)
{
    thinker_t *th;
    int        i;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 == (actionf_p1) P_MobjThinker)
        {
            mobj_t *other = (mobj_t *) th;

            if (other->target == mo)
            {
                other->target = NULL;
            }
            if (other->tracer == mo)
            {
                other->tracer = NULL;
            }
        }
    }
    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (players[i].attacker == mo)
        {
            players[i].attacker = NULL;
        }
    }
    for (i = 0; i < numsectors; i++)
    {
        if (sectors[i].soundtarget == mo)
        {
            sectors[i].soundtarget = NULL;
        }
    }
}

/* End a process monster's life for `reason` (psd_end_action decides how). A
 * live monster whose process left the list teleports out; it never just
 * vanishes. A corpse died normally and stays, as an ordinary corpse that acts
 * on nothing and is not saved. Only the sync calls this, between thinker
 * runs, so `mo` is not in use. Returns true if `mo` was removed. */
static boolean PSD_RetireMonster(mobj_t *mo, psd_end_reason_t reason)
{
    if (psd_end_action(reason) != PSD_END_TELEPORT_OUT)
    {
        mo->psd_pid = PSD_CORPSE_PID;
        return false;
    }
    PSD_TeleportFog(mo->x, mo->y, mo->z);
    PSD_ForgetMobj(mo);
    P_RemoveMobj(mo);
    return true;
}

/* Pick the toughest tier `metric` earns, then degrade to the first lighter
 * tier whose sprite the loaded WAD actually has. Freedoom Phase 1 and
 * registered Doom lack the Hell Knight sprite, and spawning an undrawable
 * monster is fatal (R_ProjectSprite I_Error), so that tier falls back to the
 * next lighter one; a complete WAD uses the full ladder. Zombieman (the last fallback) exists
 * in every Doom IWAD. */
static mobjtype_t PSD_ClassifyByTiers(unsigned long long metric,
                                      const psd_tier_t *tiers, int ntiers)
{
    int ideal = ntiers - 1;
    int i;

    for (i = 0; i < ntiers; i++)
    {
        if (metric < tiers[i].max)
        {
            ideal = i;
            break;
        }
    }
    for (i = ideal; i > 0; i--)
    {
        if (PSD_MonsterDrawable(tiers[i].type))
        {
            return tiers[i].type;
        }
    }
    return tiers[0].type;
}

static mobjtype_t PSD_ClassifyMonster(const psd_proc_t *p)
{
    if (psdoom_classify_by == PSD_CLASSIFY_CPU)
    {
        return PSD_ClassifyByTiers((unsigned long long) p->cpu_percent,
            psd_cpu_tiers, (int) (sizeof(psd_cpu_tiers) / sizeof(psd_cpu_tiers[0])));
    }
    return PSD_ClassifyByTiers(p->footprint,
        psd_mem_tiers, (int) (sizeof(psd_mem_tiers) / sizeof(psd_mem_tiers[0])));
}

/* Custom-arena placement grid. Cell spacing is wide enough to fit a Baron
 * (radius 24); the generator and validator reserve every cell from static map
 * things and blocking geometry. Monsters wander once active, so this only sets
 * the initial spawn position. */
#define PSD_COURT_X   1800
#define PSD_COURT_Y  (-3600)
#define PSD_CELL        56
#define PSD_COLS        11
#define PSD_ROWS         7

/* Spawn `p`'s monster at the first free courtyard cell, scanning row-major.
 * The caller iterates the curated list in relevance order, so the most relevant
 * processes claim cells first and reliably appear -- no more losing monsters to
 * pid-hash collisions, and big monsters get room to spawn. Returns true if
 * placed. */
static boolean PSD_SpawnMonster(const psd_proc_t *p)
{
    mobjtype_t type = PSD_ClassifyMonster(p);  /* footprint -> toughness */
    mobj_t *mo;
    int col;
    int row;

    mo = P_SpawnMobj(PSD_COURT_X * FRACUNIT, PSD_COURT_Y * FRACUNIT,
                     ONFLOORZ, type);
    if (mo == NULL)
    {
        return false;
    }

    for (row = 0; row < PSD_ROWS; row++)
    {
        for (col = 0; col < PSD_COLS; col++)
        {
            fixed_t x = (PSD_COURT_X + col * PSD_CELL) * FRACUNIT;
            fixed_t y = (PSD_COURT_Y + row * PSD_CELL) * FRACUNIT;

            /* P_CheckPosition is clear only when the cell has no wall and no
             * other thing -- i.e. the cell is free. */
            if (P_CheckPosition(mo, x, y))
            {
                P_UnsetThingPosition(mo);
                mo->x = x;
                mo->y = y;
                mo->floorz = mo->z = tmfloorz;
                mo->ceilingz = tmceilingz;
                P_SetThingPosition(mo);

                mo->flags &= ~MF_COUNTKILL;   /* not part of the level kill % */
                mo->psd_pid = p->pid;
                mo->psd_start = p->start_token;
                strncpy(mo->psd_name, p->name, sizeof(mo->psd_name) - 1);
                mo->psd_name[sizeof(mo->psd_name) - 1] = '\0';

                /* It teleports in, as it will teleport out. */
                PSD_TeleportFog(mo->x, mo->y, mo->z);
                return true;
            }
        }
    }

    /* Courtyard full this sync; drop it and retry next time. */
    P_RemoveMobj(mo);
    return false;
}

/* ------------------------------------------------------- re-classification */

/* Keep only grade entries whose pid is still in this sync's curated set, so the
 * table doesn't accumulate stale pids as processes come and go. */
static void PSD_GradeCompact(int n)
{
    int w = 0;
    int i;

    for (i = 0; i < psd_grade_count; i++)
    {
        if (PSD_InSelected(psd_grade[i].pid, n))
        {
            psd_grade[w++] = psd_grade[i];
        }
    }
    psd_grade_count = w;
}

/* The hysteresis record for `pid`, creating it if absent. NULL only if the
 * (candidate-pool-sized) table is somehow full. */
static psd_grade_t *PSD_GradeFor(int pid)
{
    int i;

    for (i = 0; i < psd_grade_count; i++)
    {
        if (psd_grade[i].pid == pid)
        {
            return &psd_grade[i];
        }
    }
    if (psd_grade_count >= (int) (sizeof(psd_grade) / sizeof(psd_grade[0])))
    {
        return NULL;
    }
    psd_grade[psd_grade_count].pid     = pid;
    psd_grade[psd_grade_count].pending = MT_PLAYER;  /* placeholder, streak 0 */
    psd_grade[psd_grade_count].streak  = 0;
    return &psd_grade[psd_grade_count++];
}

/*
 * Re-grade a live process-monster to `newtype` (already WAD-drawable, via
 * PSD_ClassifyByTiers). Swaps the type and its physical attributes, scales
 * health so the damage taken so far is preserved as a fraction (a half-dead Imp
 * doesn't become a full-HP Baron), re-enters an active state, and pops a
 * teleport fog + cue so the change reads as deliberate. A size *increase* is
 * gated by P_CheckPosition at the current spot -- if it would clip a wall or
 * another thing the morph is deferred (returns false) and retried next sync.
 */
static boolean PSD_MorphMonster(mobj_t *mo, mobjtype_t newtype)
{
    mobjinfo_t *ni       = &mobjinfo[newtype];
    int         oldspawn = mo->info->spawnhealth;
    fixed_t     fx       = mo->x;
    fixed_t     fy       = mo->y;
    fixed_t     fz       = mo->z;
    int         newhealth;
    statenum_t  newstate;

    /* Defer a size increase that wouldn't fit where the monster stands. */
    if (ni->radius > mo->radius)
    {
        fixed_t saved_r = mo->radius;
        fixed_t saved_h = mo->height;
        boolean fits;

        mo->radius = ni->radius;
        mo->height = ni->height;
        fits = P_CheckPosition(mo, mo->x, mo->y);
        mo->radius = saved_r;
        mo->height = saved_h;
        if (!fits)
        {
            return false;
        }
    }

    newhealth = (oldspawn > 0)
        ? (int) (((long long) mo->health * ni->spawnhealth) / oldspawn)
        : ni->spawnhealth;
    if (newhealth < 1)
    {
        newhealth = 1;
    }

    mo->type   = newtype;
    mo->info   = ni;
    mo->radius = ni->radius;
    mo->height = ni->height;
    mo->health = newhealth;
    mo->flags  = ni->flags & ~MF_COUNTKILL;   /* never part of the kill % */

    newstate = (ni->seestate != S_NULL) ? ni->seestate : ni->spawnstate;

    /* Morph flash + cue. Done while `mo` is still valid -- before
     * P_SetMobjState, which could in principle retire it. */
    PSD_TeleportFog(fx, fy, fz);

    P_SetMobjState(mo, newstate);
    return true;
}

/*
 * Re-evaluate every live process-monster against its process's current metric
 * and morph it once the hysteresis hold is met. `n` is the size of the curated
 * set in psd_selected (relevance-ordered). Called once per sync.
 */
static void PSD_ReclassifyMonsters(int n)
{
    int i;

    PSD_GradeCompact(n);

    for (i = 0; i < n; i++)
    {
        const psd_proc_t *p = &psd_selected[i];
        mobj_t           *mo = PSD_FindMonster(p->pid);
        mobjtype_t        desired;
        psd_grade_t      *g;
        int               hold;

        if (mo == NULL || mo->health <= 0)   /* not spawned yet, or dying */
        {
            continue;
        }

        desired = PSD_ClassifyMonster(p);
        g       = PSD_GradeFor(p->pid);
        if (g == NULL)
        {
            continue;
        }

        /* Already the right tier: reset the vote. */
        if (desired == mo->type)
        {
            g->streak  = 0;
            g->pending = mo->type;
            continue;
        }

        /* Count consecutive syncs voting for the same target tier. */
        if (g->streak == 0 || g->pending != desired)
        {
            g->pending = desired;
            g->streak  = 1;
        }
        else
        {
            g->streak++;
        }

        /* Promote fast, demote slow (by spawn-health = toughness). */
        hold = (mobjinfo[desired].spawnhealth > mobjinfo[mo->type].spawnhealth)
             ? PSD_REGRADE_UP_HOLD
             : PSD_REGRADE_DOWN_HOLD;

        if (g->streak >= hold && PSD_MorphMonster(mo, desired))
        {
            g->streak  = 0;
            g->pending = desired;
        }
    }
}

/* ------------------------------------------------------------ fork swarms */

/* The monster type used for swarm souls. Lost Soul is the thematic choice, but
 * a WAD without its sprite (shareware, Episode 1 only) degrades to a
 * drawable grunt -- spawning an undrawable type is fatal. */
static mobjtype_t PSD_SwarmType(void)
{
    if (PSD_MonsterDrawable(MT_SKULL))    return MT_SKULL;
    if (PSD_MonsterDrawable(MT_TROOP))    return MT_TROOP;
    return MT_POSSESSED;
}

/* Spawn up to `count` inert souls around `parent`, honoring the global live
 * ceiling. Each soul is tagged PSD_SWARM_PID so no process path ever acts on
 * it. Returns the number spawned. */
static int PSD_SpawnSwarm(mobj_t *parent, int count)
{
    static const struct { int dx; int dy; } ring[] =
    {
        {  48,   0 }, { -48,   0 }, {   0,  48 }, {   0, -48 },
        {  40,  40 }, { -40,  40 }, {  40, -40 }, { -40, -40 },
    };
    const int  nring = (int) (sizeof(ring) / sizeof(ring[0]));
    mobjtype_t type  = PSD_SwarmType();
    int spawned = 0;
    int i;

    for (i = 0; i < nring && spawned < count
                && psd_swarm_live < PSD_MAX_SWARM_LIVE; i++)
    {
        fixed_t x = parent->x + ring[i].dx * FRACUNIT;
        fixed_t y = parent->y + ring[i].dy * FRACUNIT;
        mobj_t *soul = P_SpawnMobj(parent->x, parent->y, parent->z, type);

        if (soul == NULL)
        {
            continue;
        }

        /* Nudge to a free ring cell if one is clear; otherwise leave it at the
         * parent's spot (it will wander off on its own). */
        if (P_CheckPosition(soul, x, y))
        {
            P_UnsetThingPosition(soul);
            soul->x = x;
            soul->y = y;
            soul->floorz = soul->z = tmfloorz;
            soul->ceilingz = tmceilingz;
            P_SetThingPosition(soul);
        }

        soul->flags &= ~MF_COUNTKILL;     /* not part of the level kill %     */
        soul->psd_pid = PSD_SWARM_PID;    /* inert: never signals anything    */
        soul->psd_name[0] = '\0';
        spawned++;
        psd_swarm_live++;
    }
    return spawned;
}

/* Keep only fork entries whose pid is still in this sync's curated set. */
static void PSD_ForkCompact(int n)
{
    int w = 0;
    int i;

    for (i = 0; i < psd_fork_count; i++)
    {
        if (PSD_InSelected(psd_fork[i].pid, n))
        {
            psd_fork[w++] = psd_fork[i];
        }
    }
    psd_fork_count = w;
}

/* The fork record for `pid`, creating it (with a -1 baseline) if absent. */
static psd_fork_t *PSD_ForkFor(int pid)
{
    int i;

    for (i = 0; i < psd_fork_count; i++)
    {
        if (psd_fork[i].pid == pid)
        {
            return &psd_fork[i];
        }
    }
    if (psd_fork_count >= (int) (sizeof(psd_fork) / sizeof(psd_fork[0])))
    {
        return NULL;
    }
    psd_fork[psd_fork_count].pid            = pid;
    psd_fork[psd_fork_count].children       = -1;   /* baseline next sync */
    psd_fork[psd_fork_count].cooldown_until = 0;
    return &psd_fork[psd_fork_count++];
}

/*
 * Spot processes that are forking hard and spawn a swarm around their monster.
 * `n` is the size of the curated set in psd_selected. Called once per sync,
 * after the live monsters are reconciled (so a forking parent has a monster).
 */
static void PSD_DetectForkBombs(int n)
{
    int i;

    PSD_ForkCompact(n);

    for (i = 0; i < n && psd_swarm_live < PSD_MAX_SWARM_LIVE; i++)
    {
        const psd_proc_t *p = &psd_selected[i];
        int          children = psd_select_child_count(p->pid);
        psd_fork_t  *f = PSD_ForkFor(p->pid);
        int          burst;
        mobj_t      *parent;

        if (f == NULL)
        {
            continue;
        }

        /* First sight: just record the baseline, don't treat it as a burst. */
        if (f->children < 0)
        {
            f->children = children;
            continue;
        }

        burst       = children - f->children;
        f->children = children;

        if (burst < PSD_FORK_BURST || leveltime < f->cooldown_until)
        {
            continue;
        }

        parent = PSD_FindMonster(p->pid);
        if (parent != NULL && parent->health > 0)
        {
            int want = (burst < PSD_SWARM_PER_BURST) ? burst : PSD_SWARM_PER_BURST;

            if (PSD_SpawnSwarm(parent, want) > 0)
            {
                f->cooldown_until = leveltime + PSD_SWARM_COOLDOWN;
                fprintf(stderr, "psDoom: fork burst pid %d (+%d children) -> swarm\n",
                        p->pid, burst);
            }
        }
    }
}

/* ---------------------------------------------------------------- public API */

void psdoom_set_intro_available(int available)
{
    psd_intro_state = available ? PSD_INTRO_READY : PSD_INTRO_UNAVAILABLE;
    psd_first_active_collection = false;
}

int psdoom_intro_available(void)
{
    return psd_intro_state != PSD_INTRO_UNAVAILABLE;
}

void psdoom_request_intro(void)
{
    if (psd_intro_state == PSD_INTRO_READY)
    {
        psd_intro_state = PSD_INTRO_PENDING;
    }
}

void psdoom_prepare_new_game(void)
{
    if (psd_intro_state == PSD_INTRO_PENDING)
    {
        psd_intro_state = PSD_INTRO_ACTIVE;
    }
    else if (psd_intro_state != PSD_INTRO_UNAVAILABLE)
    {
        psd_intro_state = PSD_INTRO_READY;
    }
    psd_first_active_collection = psd_intro_state == PSD_INTRO_ACTIVE;
}

void psdoom_end_intro(void)
{
    if (psd_intro_state != PSD_INTRO_UNAVAILABLE)
    {
        psd_intro_state = PSD_INTRO_READY;
    }
    psd_first_active_collection = false;
}

/* Forget everything psDoom tracks about the current level's monsters. */
static void PSD_ResetLevelState(void)
{
    psd_killed_count = 0;
    psd_grade_count  = 0;
    psd_fork_count   = 0;
    psd_swarm_live   = 0;
    psd_next_sync    = 0;
}

void psdoom_restore_intro(int active)
{
    /* A save keeps no process monster or swarm soul (they are ethereal), so
     * the loaded level starts with none: drop the kills, grades, fork
     * baselines and the live-soul tally of the level being replaced. The sync
     * rebuilds process monsters from the live list. */
    PSD_ResetLevelState();
    psd_last_leveltime = 0;

    if (active && psd_intro_state != PSD_INTRO_UNAVAILABLE)
    {
        psd_intro_state = PSD_INTRO_ACTIVE;
    }
    else if (psd_intro_state != PSD_INTRO_UNAVAILABLE)
    {
        psd_intro_state = PSD_INTRO_READY;
    }
    psd_first_active_collection = psd_intro_state == PSD_INTRO_ACTIVE;
}

int psdoom_intro_active(void)
{
    return psd_intro_state == PSD_INTRO_ACTIVE;
}

void psdoom_init(void)
{
    /* If PSDOOM_POLL_CMD is set, swap the native OS backend for the external
     * script backend before anything reads the process list or the uid. */
    proc_script_install_from_env();

    psd_select_init();
    psdoom_options_parse_args();   /* let -psdoom-* flags override the config */
    PSD_ResetLevelState();
    psd_last_leveltime = 0;
    psd_enabled        = true;
    psd_first_active_collection = psdoom_intro_active();
    psd_collect_count = 0;
    psd_backend_action_count = 0;
    psd_intro_smoke_stage = M_ParmExists("-psdoom-intro-smoke") ? 1 : 0;

    fprintf(stderr,
            "psDoom: process management active (build %s; uid %u, pid %d)\n",
            PSD_BUILD_ID, proc_backend()->current_uid(), (int) getpid());
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

    if (psd_intro_smoke_stage == 1 && gamestate == GS_LEVEL
        && demoplayback && !psdoom_intro_active() && leveltime >= 1)
    {
        if (psd_collect_count != 0 || psd_backend_action_count != 0)
        {
            fprintf(stderr,
                    "psDoom intro smoke: DEMO1 performed forbidden process work (collections=%d actions=%d)\n",
                    psd_collect_count, psd_backend_action_count);
            psd_intro_smoke_stage = 99;
            return;
        }
        fprintf(stderr,
                "psDoom intro smoke: stock DEMO1 E%dM%d active; intro inactive; process collections=0 backend actions=0\n",
                gameepisode, gamemap);
        psd_intro_smoke_stage = 2;
        G_DeferedPsDoomNewGame(sk_medium, 1, 1);
        return;
    }

    if (psd_intro_smoke_stage == 2 && PSD_OnSpawnLevel()
        && leveltime >= PSD_SYNC_INTERVAL + 1)
    {
        if (psd_collect_count == 0)
        {
            fprintf(stderr,
                    "psDoom intro smoke: PSDINTRO did not synchronize processes\n");
            psd_intro_smoke_stage = 99;
            return;
        }
        fprintf(stderr,
                "psDoom intro smoke: custom arena active; process synchronization=on; collections=%d\n",
                psd_collect_count);
        psd_intro_smoke_stage = 3;
        G_SaveGame(6, "PSDOOM INTRO SMOKE");
        return;
    }

    if (psd_intro_smoke_stage == 3 && !sendsave
        && gameaction == ga_nothing && PSD_OnSpawnLevel() && leveltime >= 1)
    {
        fprintf(stderr,
                "psDoom intro smoke: intro-owned save restored on PSDINTRO; process synchronization=on\n");
        savewadfilename = M_StringDuplicate("psdoom-smoke-missing.wad");
        fprintf(stderr,
                "psDoom intro smoke: attempting incompatible-owner save load while PSDINTRO is active\n");
        psd_intro_smoke_stage = 4;
        G_LoadGame(P_SaveGameFile(6));
        return;
    }

    if (psd_intro_smoke_stage == 4 && gameaction == ga_nothing
        && PSD_OnSpawnLevel() && leveltime >= 1)
    {
        psd_intro_smoke_campaign_collections = psd_collect_count;
        fprintf(stderr,
                "psDoom intro smoke: incompatible-owner save rejected and canceled; ownership=PSDINTRO; process synchronization=on; exit routing=IWAD E1M1\n");
        psd_intro_smoke_stage = 5;
        G_ExitLevel();
        return;
    }

    if (psd_intro_smoke_stage == 5 && gamestate == GS_LEVEL
        && !psdoom_intro_active() && gameepisode == 1 && gamemap == 1
        && leveltime >= 1)
    {
        if (psd_collect_count != psd_intro_smoke_campaign_collections)
        {
            fprintf(stderr,
                    "psDoom intro smoke: stock E1M1 performed forbidden process synchronization\n");
            psd_intro_smoke_stage = 99;
            return;
        }
        fprintf(stderr,
                "psDoom intro smoke: stock E1M1 active; process synchronization=off\n");
        psd_intro_smoke_stage = 6;
        G_SaveGame(7, "PSDOOM INTRO SMOKE");
        return;
    }

    if (psd_intro_smoke_stage == 6 && !sendsave
        && gameaction == ga_nothing && gamestate == GS_LEVEL
        && !psdoom_intro_active() && gameepisode == 1 && gamemap == 1
        && leveltime >= 1)
    {
        if (psd_collect_count != psd_intro_smoke_campaign_collections)
        {
            fprintf(stderr,
                    "psDoom intro smoke: campaign save restore enabled process synchronization\n");
            psd_intro_smoke_stage = 99;
            return;
        }
        fprintf(stderr,
                "psDoom intro smoke: menu-path campaign save restored on IWAD E1M1; process synchronization=off\n");
        psd_intro_smoke_stage = 7;
        G_ExitLevel();
        return;
    }

    if (psd_intro_smoke_stage == 7 && gamestate == GS_LEVEL
        && !psdoom_intro_active() && gameepisode == 1 && gamemap == 2
        && leveltime >= 1)
    {
        if (psd_collect_count != psd_intro_smoke_campaign_collections)
        {
            fprintf(stderr,
                    "psDoom intro smoke: stock E1M2 performed forbidden process synchronization\n");
            psd_intro_smoke_stage = 99;
            return;
        }
        fprintf(stderr,
                "psDoom intro smoke: stock E1M2 active; process synchronization=off\n");
        psd_intro_smoke_stage = 8;
        return;
    }

    if (!psd_enabled || !PSD_OnSpawnLevel())
    {
        return;
    }

    /* A fresh level restarts leveltime; forget this level's kills and resync
     * immediately (this also clears a stale throttle when re-entering E1M1). */
    if (leveltime < psd_last_leveltime)
    {
        PSD_ResetLevelState();
        psd_first_active_collection = true;
    }
    psd_last_leveltime = leveltime;

    /* Self-throttle: reconcile roughly once a second. */
    if (leveltime < psd_next_sync)
    {
        return;
    }
    psd_next_sync = leveltime + PSD_SYNC_INTERVAL;

    /* The first active snapshot is a CPU-rate baseline. Do not enumerate
     * processes during the title demo; begin only after PSDINTRO is active. */
    if (psd_first_active_collection)
    {
        (void) psd_select_collect(psd_selected, PSD_CANDIDATE_CAP);
        psd_collect_count++;
        psd_first_active_collection = false;
        psd_next_sync = leveltime + PSD_SYNC_INTERVAL;
        return;
    }

    /* Ask the triage layer for the curated, ranked, truncated collection. */
    n = psd_select_collect(psd_selected, PSD_CANDIDATE_CAP);
    psd_collect_count++;

    psd_fog_sound_played = false;

    /* 1) Retire monsters no longer in the curated set (process exited, its pid
     *    was reused, or it dropped below the relevance cut), and monsters a
     *    checked action already found gone. Live ones teleport out; corpses
     *    stay (psd_end_reason). A fog spawned here joins the end of the list
     *    with psd_pid 0, so the walk skips it. */
    for (th = thinkercap.next; th != &thinkercap; th = next)
    {
        next = th->next;

        if (th->function.acp1 == (actionf_p1) P_MobjThinker)
        {
            mobj_t          *mo = (mobj_t *) th;
            psd_end_reason_t why = psd_end_reason(mo->psd_pid, mo->health <= 0,
                mo->psd_pid > 0 && PSD_MonsterSelected(mo, n));

            if (why != PSD_END_STAYS && PSD_RetireMonster(mo, why))
            {
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

        if (PSD_IsKilled(p->pid, p->start_token, p->name))
        {
            continue;
        }
        if (PSD_FindMonster(p->pid) == NULL && PSD_SpawnMonster(p))
        {
            spawned++;
            live++;
        }
    }

    /* 3) Re-grade existing monsters to their process's current metric (with
     *    hysteresis), morphing the ones whose load/footprint moved tiers. */
    PSD_ReclassifyMonsters(n);

    /* 4) Spot fork bombs (a parent's child count jumping) and spawn a capped
     *    swarm of inert Lost Souls around the offending monster. */
    PSD_DetectForkBombs(n);

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
        psd_act_result_t r;

        psd_backend_action_count++;
        r = psd_proc_renice_checked(mo->psd_pid, mo->psd_start, mo->psd_name,
                                    PSD_WOUND_NICE);
        if (r == PSD_ACT_GONE || r == PSD_ACT_REUSED)
        {
            PSD_MarkGone(mo, r, "renice");
        }
        else if (r == PSD_ACT_REFUSED)
        {
            PSD_LogSkipped(mo, r, "renice");
        }
    }
}

void psdoom_kill(struct mobj_s *target)
{
    mobj_t *mo = (mobj_t *) target;

    if (mo == NULL)
    {
        return;
    }

    /* A fork-swarm soul died: just keep the live-soul tally accurate so the
     * global ceiling self-corrects. It represents no process -- nothing to
     * signal or remember. */
    if (mo->psd_pid == PSD_SWARM_PID)
    {
        if (psd_swarm_live > 0)
        {
            psd_swarm_live--;
        }
        return;
    }

    if (mo->psd_pid <= 1)
    {
        return;
    }

    if (psdoom_should_kill())
    {
        psd_act_result_t r;

        psd_backend_action_count++;
        r = psd_proc_kill_checked(mo->psd_pid, mo->psd_start, mo->psd_name);
        if (r == PSD_ACT_GONE || r == PSD_ACT_REUSED)
        {
            /* Nothing was signalled, and the kill was not of the process now
             * holding the pid, so don't remember it as killed. */
            PSD_MarkGone(mo, r, "kill");
            return;
        }
        if (r == PSD_ACT_REFUSED)
        {
            PSD_LogSkipped(mo, r, "kill");
        }
    }

    /* Remember it so psdoom_sync won't respawn it -- crucial when the kill
     * policy leaves the real process running. */
    PSD_MarkKilled(mo->psd_pid, mo->psd_start, mo->psd_name);
}

/* ---------------------------------------------------------------- save/load */

int psdoom_mobj_saved(const struct mobj_s *target)
{
    const mobj_t *mo = (const mobj_t *) target;

    /* P_SpawnMapThing records the map thing; an on-demand P_SpawnMobj leaves
     * the spawn point zeroed, and no map thing has type 0. */
    return psd_save_disposition(mo->psd_pid, mo->spawnpoint.type != 0)
        != PSD_SAVE_SKIP;
}

void psdoom_restored_mobj(struct mobj_s *target)
{
    mobj_t *mo = (mobj_t *) target;

    /* The identity is not in the save and the restored block is not zeroed:
     * every restored mobj is an ordinary one. */
    mo->psd_pid   = 0;
    mo->psd_start = 0;
    memset(mo->psd_name, 0, sizeof(mo->psd_name));
}

void psdoom_loaded_game(void)
{
    thinker_t *th;
    thinker_t *next;
    int        dropped = 0;

    /* Saves made before process monsters became ethereal still hold them, as
     * plain mobjs. Only psDoom spawns a monster on demand (no spawn point)
     * and clears the MF_COUNTKILL its type carries, so that signature finds
     * them; left in place they would sit beside the monsters the sync
     * rebuilds. This runs after every pointer in the save is restored, so
     * dropping them does not shift any index. */
    for (th = thinkercap.next; th != &thinkercap; th = next)
    {
        next = th->next;

        if (th->function.acp1 == (actionf_p1) P_MobjThinker)
        {
            mobj_t *mo = (mobj_t *) th;

            if (psd_legacy_process_monster(mo->player != NULL,
                                           mo->spawnpoint.type != 0,
                                           (mo->info->flags & MF_COUNTKILL) != 0,
                                           (mo->flags & MF_COUNTKILL) != 0))
            {
                PSD_ForgetMobj(mo);
                P_RemoveMobj(mo);
                dropped++;
            }
        }
    }
    if (dropped != 0)
    {
        fprintf(stderr, "psDoom: dropped %d process monsters from an older save\n",
                dropped);
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

/* Find the painted horizontal bounds of `str` relative to the starting pen.
 * The pen follows PSD_DrawHUString exactly, but each glyph's visual left edge
 * is shifted by its patch origin. Spaces and missing glyphs advance the pen
 * without contributing painted pixels. */
static void PSD_HUStringBounds(const char *str, int *left, int *right)
{
    const char *p;
    int advance = 0;
    boolean have_glyph = false;

    for (p = str; *p != '\0'; p++)
    {
        int c = toupper((unsigned char) *p);
        patch_t *glyph;
        int glyph_left;
        int glyph_right;

        if (c == ' ')
        {
            advance += PSD_LABEL_SPACE_W;
            continue;
        }

        c -= HU_FONTSTART;
        if (c < 0 || c >= HU_FONTSIZE || hu_font[c] == NULL)
        {
            advance += PSD_LABEL_SPACE_W;
            continue;
        }

        glyph = hu_font[c];
        glyph_left = advance - SHORT(glyph->leftoffset);
        glyph_right = glyph_left + SHORT(glyph->width);
        if (!have_glyph || glyph_left < *left)
        {
            *left = glyph_left;
        }
        if (!have_glyph || glyph_right > *right)
        {
            *right = glyph_right;
        }
        have_glyph = true;
        advance += SHORT(glyph->width);
    }

    /* A row with no painted glyph has no visual midpoint; retaining its pen
     * extent keeps this helper well-defined without changing draw behavior. */
    if (!have_glyph)
    {
        *left = 0;
        *right = advance;
    }
}

/* Center `str`'s painted bounds on the sprite's doubled edge midpoint. The
 * input to V_DrawPatch is one WIDESCREENDELTA left of absolute logical screen
 * coordinates because V_DrawPatch adds that offset before drawing. */
static void PSD_DrawCenteredHUString(int sprite_center_twice, int y,
                                     const char *str)
{
    int left;
    int right;
    int x;

    PSD_HUStringBounds(str, &left, &right);
    x = (sprite_center_twice - left - right) / 2 - WIDESCREENDELTA;
    PSD_DrawHUString(x, y, str);
}

void psdoom_draw_label(int x1_fb, int x2_fb, int top_fb, int scale, int pid,
                       const char *name, int mobjflags)
{
    const int hires = crispy->hires;
    int x1;
    int x2;
    int sprite_center_twice;
    int y;
    char buf[16];

    if (!psdoom_labels_enabled())
    {
        return;
    }

    /* Inert swarm souls (psd_pid < 0) and ordinary monsters (0) carry no
     * process identity to show. */
    if (pid <= 0)
    {
        return;
    }

    /* Don't label corpses: the process is already gone (psdoom_kill ran
     * when it died) and the count-down sprite looks better without a
     * lingering PID overhead it. Gate on the vissprite's captured flags,
     * not mo->health, since flags are stable at draw time. */
    if (mobjflags & MF_CORPSE)
    {
        return;
    }

    /* Skip distant sprites to keep the screen readable (gate from the
     * "Label draw distance" option). */
    if ((scale >> hires) < psdoom_label_min_scale())
    {
        return;
    }

    /* The renderer's x1/x2/top are relative to the 3D view window's top-left,
     * in framebuffer pixels (320 << hires). Add the view-window origin to get
     * absolute framebuffer coordinates, then fold back to 320x200 logical
     * space. x1/x2 describe inclusive sprite pixels, so their doubled edge
     * midpoint is x1 + x2 + 1. PSD_DrawCenteredHUString converts that absolute
     * center back to V_DrawPatch's pre-widescreen coordinate space. */
    x1 = (x1_fb + viewwindowx) >> hires;
    x2 = (x2_fb + viewwindowx) >> hires;
    sprite_center_twice = x1 + x2 + 1;
    y = (top_fb + viewwindowy) >> hires;

    /* Move the top-of-visible-sprite y up one HUD label line before clamping
     * so labels remain above sprites. */
    y -= PSD_LABEL_LINE_H;

    if (y < 0)
    {
        y = 0;
    }
    if (y > PSD_LABEL_MAX_Y)
    {
        y = PSD_LABEL_MAX_Y;
    }

    /* PID is opt-in (off by default to keep the courtyard readable). When it's
     * shown it takes the top row and the name drops below; otherwise the name
     * sits on the top row by itself. */
    if (psdoom_pid_enabled())
    {
        snprintf(buf, sizeof(buf), "%d", pid);
        PSD_DrawCenteredHUString(sprite_center_twice, y, buf);
        y += PSD_LABEL_LINE_H;
    }

    if (name != NULL)
    {
        PSD_DrawCenteredHUString(sprite_center_twice, y, name);
    }
}
