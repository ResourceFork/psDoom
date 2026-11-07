/*
 * psDoom -- process-management layer (game-side API).
 *
 * Bridges the Doom engine to OS process management: each process is represented
 * by a monster; wounding a monster renices its process, killing it signals it.
 *
 * This header is intentionally independent of the engine's headers (it only
 * forward-declares the monster type), so engine call-sites include it cheaply.
 */

#ifndef PSDOOM_H
#define PSDOOM_H

/*
 * The engine defines `typedef struct mobj_s mobj_t;`. We take `struct mobj_s *`
 * so this header needs none of the engine's headers.
 */
struct mobj_s;

#ifdef __cplusplus
extern "C" {
#endif

/* The bundled introduction advances explicitly through READY (available),
 * PENDING (the player completed New Game selection), and ACTIVE (PSDINTRO is
 * loading). Demo playback and ordinary campaign starts never activate it. */
void psdoom_set_intro_available(int available);
int psdoom_intro_available(void);
void psdoom_request_intro(void);
void psdoom_prepare_new_game(void);
void psdoom_end_intro(void);
void psdoom_restore_intro(int active);
int psdoom_intro_active(void);

/* One-time setup at startup: open the process backend, load the allow/deny
 * filter, default to the current UID's processes. */
void psdoom_init(void);

/* Periodic reconcile of the live process list with in-game monsters. Called
 * from the game tick; must be cheap and self-throttling. */
void psdoom_sync(void);

/* A process-monster took non-fatal damage -> lower its process priority
 * (renice). No-op if `target` is not a process-monster. */
void psdoom_wound(struct mobj_s *target);

/* A process-monster died -> signal its process (kill). No-op if `target` is
 * not a process-monster. */
void psdoom_kill(struct mobj_s *target);

/* Process monsters are ethereal: they are backed only by the live process
 * list. psdoom_mobj_saved is nonzero if a save writes `target` (P_ArchiveThinkers
 * and the save's mobj index enumeration, P_ThinkerToIndex / P_IndexToThinker);
 * a monster spawned on demand for a process, swarm souls included, is never
 * written. psdoom_restored_mobj clears the process identity of a mobj just
 * read from a save (P_UnArchiveThinkers), so a load restores ordinary mobjs
 * only. Neither changes the save format. psdoom_restore_intro also resets the
 * per-level process state, so the sync rebuilds process monsters from the
 * live list after a load. */
int psdoom_mobj_saved(const struct mobj_s *target);
void psdoom_restored_mobj(struct mobj_s *target);

/* After a load has restored every pointer (end of G_DoLoadGame): drop the
 * process monsters that saves from older builds still hold. */
void psdoom_loaded_game(void);

/*
 * Draw a process-monster's PID and name centered above its sprite. Called
 * from the renderer (R_DrawVisSprite) once per visible process-monster.
 *
 *   x1_fb   : vissprite left edge, view-window-relative framebuffer
 *             pixels
 *   x2_fb   : vissprite right edge, view-window-relative framebuffer
 *             pixels (used with x1_fb to find the sprite's inclusive-range
 *             midpoint; each label row is centered there using its glyph
 *             visual bounds, including patch left offsets)
 *   top_fb  : topmost rendered row of the sprite, view-window-relative
 *             framebuffer pixels (the visible sprite top)
 *   scale   : vissprite scale (larger = closer)
 *   pid     : process id to print (caller guarantees != 0)
 *   name    : process name (may be NULL)
 *   mobjflags : the sprite's mobj flags word (vis->mobjflags); used to
 *               suppress the label on corpses (MF_CORPSE) without
 *               depending on live mo->health, since the vissprite may
 *               outlive the mobj's gameplay-relevant state changes
 *
 * Coordinates are offset by the 3D view window origin, converted to 320x200
 * logical space, then translated back to the pre-V_DrawPatch coordinate space
 * internally. Distant sprites are skipped to avoid
 * clutter. The caller only invokes this for sprites that actually rendered,
 * so labels never show through walls.
 */
void psdoom_draw_label(int x1_fb, int x2_fb, int top_fb, int scale, int pid,
                       const char *name, int mobjflags);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_H */
