/*
 * psDoom -- grade -> monster tier (script contract v2).
 *
 * A v2 poll line may carry `grade=<float 0..1>`: where the entity sits on its
 * source's own full range, with no knowledge of the game. The game maps that
 * range onto however many monster tiers it can actually draw, so 1.0 is
 * always the top drawable monster and 0.0 the bottom one, whatever the WAD.
 *
 * Engine-free and OS-free so the mapping is host-testable; psdoom.c supplies
 * which ladder tiers are drawable. See docs/script-backend.md (v2).
 */

#ifndef PSDOOM_GRADE_H
#define PSDOOM_GRADE_H

#ifdef __cplusplus
extern "C" {
#endif

/* tier = min(floor(grade * n), n - 1), with grade clamped to [0, 1]. A NaN
 * grade counts as 0. Returns 0 when n <= 1. */
int psd_grade_tier(double grade, int n);

/*
 * Pick a ladder index for `grade` among the drawable tiers. `drawable[i]` is
 * nonzero if ladder tier i (0 = weakest) can be drawn; tier 0 is treated as
 * drawable whatever it says, since the bottom of the ladder exists in every
 * Doom IWAD. Returns the ladder index (0..ntiers-1) of the chosen tier.
 */
int psd_grade_pick(double grade, const int *drawable, int ntiers);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_GRADE_H */
