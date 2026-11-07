/*
 * psDoom IWAD selection (engine-free).
 *
 * The launcher decides which IWAD the engine runs and whether the bundled
 * PSDINTRO arena may be loaded with it. The policy, in order:
 *
 *   1. An explicit -iwad from the caller is never replaced by another IWAD
 *      (a bare name is resolved the way the engine would find it). The arena
 *      is loaded only when that file is a registered/Ultimate Doom or
 *      Freedoom Phase 1 IWAD; anything else (shareware, Doom II, unknown)
 *      runs as stock Crispy Doom with no arena.
 *   2. Otherwise a registered or Ultimate Doom DOOM.WAD found in DOOMWADDIR,
 *      then each DOOMWADPATH entry, then ~/Library/Application Support/psDoom
 *      wins. An entry may name the DOOM.WAD file itself.
 *   3. Otherwise the bundled Resources/freedoom1.wad is used.
 *
 * A shareware DOOM1.WAD, and a Doom II-format IWAD, are never selected
 * automatically.
 */

#ifndef PSD_IWAD_SELECT_H
#define PSD_IWAD_SELECT_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    PSD_IWAD_MISSING = 0, /* cannot be opened or read                       */
    PSD_IWAD_INVALID,     /* not an IWAD (a PWAD, truncated, or not a WAD)  */
    PSD_IWAD_OTHER,       /* an IWAD psDoom does not run its arena on       */
    PSD_IWAD_SHAREWARE,   /* Doom shareware (Episode 1 only)                */
    PSD_IWAD_REGISTERED,  /* registered or Ultimate Doom                    */
    PSD_IWAD_FREEDOOM1,   /* Freedoom: Phase 1                              */
    PSD_IWAD_COMMERCIAL   /* Doom II-format (MAPxx): Doom II, Final Doom,
                             Freedoom: Phase 2, FreeDM                      */
} psd_iwad_kind_t;

/* Classify one file. ctx is passed through from the query. */
typedef psd_iwad_kind_t (*psd_iwad_probe_fn)(const char *path, void *ctx);

typedef struct
{
    const char *explicit_iwad; /* value after the caller's -iwad, or NULL   */
    const char *doomwaddir;    /* $DOOMWADDIR, or NULL                       */
    const char *doomwadpath;   /* $DOOMWADPATH (':'-separated), or NULL      */
    const char *home;          /* $HOME, or NULL                             */
    const char *resources_dir; /* the bundle's Contents/Resources            */
    psd_iwad_probe_fn probe;
    void *probe_ctx;
} psd_iwad_query_t;

typedef struct
{
    /* When pass_iwad is set the launcher adds "-iwad path". It is clear when
     * the caller gave -iwad. When replace_explicit is set, the caller's bare
     * -iwad name was found and path is that file: the launcher passes path
     * in its place so the engine opens exactly the file classified here. */
    int pass_iwad;
    int replace_explicit;
    char path[PATH_MAX];
    psd_iwad_kind_t kind; /* classification of the IWAD the engine will run */
    int arena_ok;         /* the bundled arena may be loaded with it        */
} psd_iwad_choice_t;

/* 1 for the kinds the arena is validated against and Crispy's -file lockout
 * accepts: registered/Ultimate Doom and Freedoom Phase 1. */
int psd_iwad_kind_arena_ok(psd_iwad_kind_t kind);

/* Apply the policy above. Always fills *out. When nothing usable is found it
 * still names the bundled freedoom1.wad so the engine reports it missing,
 * rather than letting the engine's own search pick a shareware IWAD. */
void psd_iwad_select(const psd_iwad_query_t *query, psd_iwad_choice_t *out);

/* Classify a WAD from its 12-byte header and its lump directory (numlumps
 * 16-byte entries). Pure; mirrors Crispy's identification and its -file
 * lockout checks. */
psd_iwad_kind_t psd_iwad_classify(const unsigned char header[12],
                                  const unsigned char *directory,
                                  uint32_t numlumps);

/* The real probe: reads the header and directory of path and classifies it.
 * ctx is unused. */
psd_iwad_kind_t psd_iwad_probe_file(const char *path, void *ctx);

#endif
