/*
 * psDoom -- process-monster lifecycle rules (engine-free).
 *
 * Process monsters are ethereal: they are backed only by the live process
 * list, so a save never keeps one, and a monster whose process leaves the list
 * teleports out instead of vanishing. These are the pure decisions behind
 * that; psdoom.c applies them to the engine's mobjs, and the host tests cover
 * them without the engine.
 */

#ifndef PSDOOM_LIFECYCLE_H
#define PSDOOM_LIFECYCLE_H

/* psd_pid tags. 0 is an ordinary monster; > 1 is a process monster. */
#define PSD_SWARM_PID (-1)  /* an inert fork-swarm soul                       */
#define PSD_GONE_PID  (-2)  /* its process is gone or its pid was reused; it
                             * acts on nothing and shows no label until the
                             * next sync retires it                           */

/* What a save does with one mobj. */
typedef enum
{
    PSD_SAVE_KEEP,   /* an ordinary mobj: written as is                       */
    PSD_SAVE_STRIP,  /* a map-placed monster carrying a process identity:
                      * written as a plain map monster; the identity is not
                      * part of the save, and a load never restores one       */
    PSD_SAVE_SKIP,   /* spawned on demand for the process list: never written */
} psd_save_t;

/* `psd_pid` is the mobj's tag; `map_placed` is nonzero if the map spawned it
 * (it has a spawn point), zero if it was spawned on demand. */
psd_save_t psd_save_disposition(int psd_pid, int map_placed);

/* Why a process monster's life ends. */
typedef enum
{
    PSD_END_LEFT_LIST,     /* its process left the curated list (exited, its
                            * pid was reused, or it fell below the cut)       */
    PSD_END_IDENTITY_LOST, /* a checked renice or kill found its pid gone or
                            * reused (tagged PSD_GONE_PID)                    */
    PSD_END_KILLED,        /* it was killed in the game                       */
} psd_end_reason_t;

typedef enum
{
    PSD_END_DIE,          /* the engine's normal death; psDoom does nothing   */
    PSD_END_TELEPORT_OUT, /* teleport fog and sound where it stood, then it
                           * is removed                                       */
} psd_end_action_t;

psd_end_action_t psd_end_action(psd_end_reason_t reason);

#endif /* PSDOOM_LIFECYCLE_H */
