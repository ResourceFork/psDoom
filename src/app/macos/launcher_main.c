/*
 * psDoom.app launcher
 *
 * This compiled launcher is the bundle's CFBundleExecutable. It locates a Doom
 * IWAD and exec()s the engine binary that lives beside it in Contents/MacOS.
 *
 * NOTE: it currently launches the *stock* vendored Crispy Doom engine. The
 * psDoom process-management behaviour (process->monster, wound->renice,
 * kill->kill, labels) is not implemented yet.
 */

#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    /*
     * IWAD search path: the bundle's Resources/ dir, then the per-user support
     * dir. Only set if the caller hasn't already chosen one, so command-line use
     * and DOOMWADDIR overrides keep working.
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
     * Build the child argv: engine path + passthrough of our own args, dropping
     * the legacy -psn_ Process Serial Number argument some launch paths add.
     */
    char **child_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (child_argv == NULL)
    {
        fprintf(stderr, "psDoom: out of memory\n");
        return 1;
    }

    int n = 0;
    child_argv[n++] = engine;
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "-psn_", 5) == 0)
        {
            continue;
        }
        child_argv[n++] = argv[i];
    }
    child_argv[n] = NULL;

    execv(engine, child_argv);

    /* execv only returns on failure. */
    fprintf(stderr, "psDoom: failed to launch engine at %s: %s\n",
            engine, strerror(errno));
    free(child_argv);
    return 1;
}
