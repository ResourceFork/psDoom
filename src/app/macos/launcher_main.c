/*
 * psDoom.app launcher
 *
 * This compiled launcher is the bundle's CFBundleExecutable. It selects a Doom
 * IWAD (iwad_select.h: registered Doom, else the bundled Freedoom, never
 * shareware), loads the bundled introduction resource when that IWAD supports
 * it, and exec()s the psDoom engine binary that lives beside it in
 * Contents/MacOS. The engine presents stock DEMO1 first and activates the
 * intro only through New Game.
 */

#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "iwad_select.h"

#define PSDOOM_BUNDLED_ARENA_ARG "-psdoom-bundled-arena"

static int is_map_override(const char *argument)
{
    static const char *const overrides[] = {
        "-file", "-merge", "-nwtmerge", "-af", "-as", "-aa",
        "-warp", "-episode", "-loadgame", "-playdemo", "-timedemo",
        "-record", "-testcontrols", "-response"
    };
    size_t i;

    if (argument[0] == '@')
    {
        return 1;
    }

    for (i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++)
    {
        if (strcasecmp(argument, overrides[i]) == 0)
        {
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    char exe_path[PATH_MAX];
    uint32_t size = sizeof(exe_path);

    if (_NSGetExecutablePath(exe_path, &size) != 0)
    {
        fprintf(stderr, "psDoom: cannot resolve own executable path\n");
        return 1;
    }

    char resolved[PATH_MAX];
    if (realpath(exe_path, resolved) == NULL)
    {
        strncpy(resolved, exe_path, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    /* Contents/MacOS (directory holding this launcher and the engine binary) */
    char tmp[PATH_MAX];
    strncpy(tmp, resolved, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char macos_dir[PATH_MAX];
    strncpy(macos_dir, dirname(tmp), sizeof(macos_dir) - 1);
    macos_dir[sizeof(macos_dir) - 1] = '\0';

    /* Contents (parent of MacOS) */
    strncpy(tmp, macos_dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char contents_dir[PATH_MAX];
    strncpy(contents_dir, dirname(tmp), sizeof(contents_dir) - 1);
    contents_dir[sizeof(contents_dir) - 1] = '\0';

    char engine[PATH_MAX];
    snprintf(engine, sizeof(engine), "%s/crispy-doom", macos_dir);

    char arena[PATH_MAX];
    snprintf(arena, sizeof(arena), "%s/Resources/psdoom-arena.wad",
             contents_dir);

    char resources_dir[PATH_MAX];
    snprintf(resources_dir, sizeof(resources_dir), "%s/Resources",
             contents_dir);

    /*
     * Choose the IWAD from the caller's own environment, before this launcher
     * adds a DOOMWADPATH of its own. The engine's -iwad check takes the first
     * "-iwad <file>" pair, so the launcher looks at the same one.
     */
    const char *explicit_iwad = NULL;
    int explicit_index = 0; /* argv index of the caller's -iwad value */
    for (int i = 1; i + 1 < argc; i++)
    {
        if (strcasecmp(argv[i], "-iwad") == 0)
        {
            explicit_iwad = argv[i + 1];
            explicit_index = i + 1;
            break;
        }
    }

    /* The engine expands a response file (@file, -response file) in place,
     * so an -iwad inside one is invisible here. */
    int has_response_file = 0;
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '@' || strcasecmp(argv[i], "-response") == 0)
        {
            has_response_file = 1;
            break;
        }
    }

    psd_iwad_query_t query = {
        .explicit_iwad = explicit_iwad,
        .doomwaddir = getenv("DOOMWADDIR"),
        .doomwadpath = getenv("DOOMWADPATH"),
        .home = getenv("HOME"),
        .resources_dir = resources_dir,
        .probe = psd_iwad_probe_file,
        .probe_ctx = NULL,
    };
    psd_iwad_choice_t iwad;
    psd_iwad_select(&query, &iwad);

    /*
     * Search path for anything else the engine looks up by name (-file, a bare
     * -iwad name): the bundle's Resources/ dir, then the per-user support dir.
     * Only set if the caller hasn't already chosen one, so command-line use and
     * DOOMWADDIR overrides keep working.
     */
    if (getenv("DOOMWADDIR") == NULL && getenv("DOOMWADPATH") == NULL)
    {
        const char *home = getenv("HOME");
        char wadpath[2 * PATH_MAX];

        if (home != NULL && home[0] != '\0')
        {
            snprintf(wadpath, sizeof(wadpath),
                     "%s/Resources:%s/Library/Application Support/psDoom",
                     contents_dir, home);
        }
        else
        {
            snprintf(wadpath, sizeof(wadpath), "%s/Resources", contents_dir);
        }

        setenv("DOOMWADPATH", wadpath, 1);
    }

    /*
     * Explicit map, demo, save, response, or PWAD controls suppress the intro,
     * and so does an IWAD the arena is not for (shareware, Doom II, unknown):
     * those run as stock Crispy Doom.
     */
    int has_map_override = 0;
    for (int i = 1; i < argc; i++)
    {
        if (is_map_override(argv[i]))
        {
            has_map_override = 1;
            break;
        }
    }
    int inject_arena = iwad.arena_ok && !has_map_override
                       && access(arena, R_OK) == 0;

    /*
     * Build the child argv: engine path, the selected IWAD unless the caller
     * named one, optional trusted arena, then pass through caller arguments
     * (a bare -iwad name replaced by the file it resolved to).
     * Caller-provided copies of the launcher-only trust marker are always
     * discarded.
     */
    char **child_argv = calloc((size_t)argc + 6, sizeof(char *));
    if (child_argv == NULL)
    {
        fprintf(stderr, "psDoom: out of memory\n");
        return 1;
    }

    /* The engine uses the first -iwad pair. With a response file the
     * launcher's choice goes last, so an -iwad the file holds still wins. */
    int iwad_first = iwad.pass_iwad && !has_response_file;
    int iwad_last = iwad.pass_iwad && has_response_file;

    int n = 0;
    child_argv[n++] = engine;
    if (iwad_first)
    {
        child_argv[n++] = "-iwad";
        child_argv[n++] = iwad.path;
    }
    if (inject_arena)
    {
        child_argv[n++] = "-file";
        child_argv[n++] = arena;
        child_argv[n++] = PSDOOM_BUNDLED_ARENA_ARG;
    }
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "-psn_", 5) == 0
            || strcasecmp(argv[i], PSDOOM_BUNDLED_ARENA_ARG) == 0)
        {
            continue;
        }
        if (i == explicit_index && iwad.replace_explicit)
        {
            child_argv[n++] = iwad.path;
            continue;
        }
        child_argv[n++] = argv[i];
    }
    if (iwad_last)
    {
        child_argv[n++] = "-iwad";
        child_argv[n++] = iwad.path;
    }
    child_argv[n] = NULL;

    execv(engine, child_argv);

    /* execv only returns on failure. */
    fprintf(stderr, "psDoom: failed to launch engine at %s: %s\n",
            engine, strerror(errno));
    free(child_argv);
    return 1;
}
