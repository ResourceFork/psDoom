/*
 * psDoom IWAD selection (engine-free). See iwad_select.h for the policy.
 */

#include "iwad_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PSD_LUMP_ENTRY_SIZE 16
#define PSD_MAX_LUMPS       65536u

/* Registered DOOM.WAD file names probed in each user directory. Probing both
 * spellings keeps case-sensitive volumes working; on the default
 * case-insensitive volume both name the same file. */
static const char *const registered_names[] = { "DOOM.WAD", "doom.wad" };

static const char bundled_name[] = "freedoom1.wad";

/* Crispy's check for a "fake IWAD with right name, but w/o all the lumps of
 * the registered version" (d_main.c), copied as is, including its duplicated
 * e3m3. A registered IWAD missing any of these aborts with -file. */
static const char *const registered_lumps[] = {
    "e2m1", "e2m2", "e2m3", "e2m4", "e2m5", "e2m6", "e2m7", "e2m8", "e2m9",
    "e3m1", "e3m3", "e3m3", "e3m4", "e3m5", "e3m6", "e3m7", "e3m8", "e3m9",
    "dphoof", "bfgga0", "heada1", "cybra1", "spida1d1"
};

int psd_iwad_kind_arena_ok(psd_iwad_kind_t kind)
{
    return kind == PSD_IWAD_REGISTERED || kind == PSD_IWAD_FREEDOOM1;
}

static int has_lump(const unsigned char *directory, uint32_t numlumps,
                    const char *name)
{
    uint32_t i;

    for (i = 0; i < numlumps; i++)
    {
        const char *entry = (const char *) directory
                            + (size_t) i * PSD_LUMP_ENTRY_SIZE + 8;

        if (strncasecmp(entry, name, 8) == 0
            && (strlen(name) >= 8 || entry[strlen(name)] == '\0'))
        {
            return 1;
        }
    }
    return 0;
}

psd_iwad_kind_t psd_iwad_classify(const unsigned char header[12],
                                  const unsigned char *directory,
                                  uint32_t numlumps)
{
    size_t i;

    if (header == NULL || memcmp(header, "IWAD", 4) != 0
        || (directory == NULL && numlumps > 0))
    {
        return PSD_IWAD_INVALID;
    }

    if (has_lump(directory, numlumps, "FREEDOOM")
        && !has_lump(directory, numlumps, "FREEDM")
        && has_lump(directory, numlumps, "E1M1"))
    {
        return PSD_IWAD_FREEDOOM1;
    }

    if (has_lump(directory, numlumps, "MAP01"))
    {
        return PSD_IWAD_COMMERCIAL;
    }

    if (!has_lump(directory, numlumps, "E1M1"))
    {
        return PSD_IWAD_OTHER;
    }

    /* Crispy: E4M1 -> retail, else E3M1 -> registered, else shareware. */
    if (!has_lump(directory, numlumps, "E4M1")
        && !has_lump(directory, numlumps, "E3M1"))
    {
        return PSD_IWAD_SHAREWARE;
    }

    /* Required of retail too (Ultimate Doom has them all): it keeps the
     * arena off Doom-format IWADs that are not id's game data. */
    for (i = 0; i < sizeof(registered_lumps) / sizeof(registered_lumps[0]); i++)
    {
        if (!has_lump(directory, numlumps, registered_lumps[i]))
        {
            return PSD_IWAD_OTHER;
        }
    }

    return PSD_IWAD_REGISTERED;
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
           | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

psd_iwad_kind_t psd_iwad_probe_file(const char *path, void *ctx)
{
    unsigned char header[12];
    unsigned char *directory;
    struct stat st;
    uint32_t numlumps;
    uint32_t offset;
    size_t dir_size;
    psd_iwad_kind_t kind;
    FILE *f;

    (void) ctx;

    if (path == NULL || path[0] == '\0')
    {
        return PSD_IWAD_MISSING;
    }

    f = fopen(path, "rb");
    if (f == NULL)
    {
        return PSD_IWAD_MISSING;
    }

    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode))
    {
        fclose(f);
        return PSD_IWAD_MISSING;
    }

    if (fread(header, 1, sizeof(header), f) != sizeof(header))
    {
        fclose(f);
        return PSD_IWAD_INVALID;
    }

    numlumps = read_le32(header + 4);
    offset = read_le32(header + 8);
    if (memcmp(header, "IWAD", 4) != 0 || numlumps > PSD_MAX_LUMPS)
    {
        fclose(f);
        return PSD_IWAD_INVALID;
    }

    dir_size = (size_t) numlumps * PSD_LUMP_ENTRY_SIZE;
    if ((uint64_t) offset + dir_size > (uint64_t) st.st_size)
    {
        fclose(f);
        return PSD_IWAD_INVALID;
    }

    directory = malloc(dir_size > 0 ? dir_size : 1);
    if (directory == NULL)
    {
        fclose(f);
        return PSD_IWAD_MISSING;
    }

    if (fseek(f, (long) offset, SEEK_SET) != 0
        || fread(directory, 1, dir_size, f) != dir_size)
    {
        free(directory);
        fclose(f);
        return PSD_IWAD_INVALID;
    }
    fclose(f);

    kind = psd_iwad_classify(header, directory, numlumps);
    free(directory);
    return kind;
}

/* Probe dir/name; on a registered hit copy the path into out. */
static int try_registered(const psd_iwad_query_t *query, const char *dir,
                          size_t dir_len, psd_iwad_choice_t *out)
{
    char candidate[PATH_MAX];
    size_t i;

    if (dir == NULL || dir_len == 0)
    {
        return 0;
    }

    for (i = 0; i < sizeof(registered_names) / sizeof(registered_names[0]); i++)
    {
        int n = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                         (int) dir_len, dir, registered_names[i]);

        if (n < 0 || (size_t) n >= sizeof(candidate))
        {
            continue;
        }
        if (query->probe(candidate, query->probe_ctx) == PSD_IWAD_REGISTERED)
        {
            memcpy(out->path, candidate, (size_t) n + 1);
            out->pass_iwad = 1;
            out->kind = PSD_IWAD_REGISTERED;
            out->arena_ok = 1;
            return 1;
        }
    }
    return 0;
}

void psd_iwad_select(const psd_iwad_query_t *query, psd_iwad_choice_t *out)
{
    char support[PATH_MAX];
    int n;

    memset(out, 0, sizeof(*out));
    out->kind = PSD_IWAD_MISSING;

    if (query == NULL || query->probe == NULL)
    {
        return;
    }

    /* 1. The caller's -iwad is theirs: pass it through, and allow the arena
     * only when the very file the engine opens first is arena-capable. The
     * engine checks the argument as given before searching, so probing the
     * same string matches what it loads; a bare name that is not in the
     * working directory is not resolved here and gets no arena. */
    if (query->explicit_iwad != NULL)
    {
        out->kind = query->probe(query->explicit_iwad, query->probe_ctx);
        out->arena_ok = psd_iwad_kind_arena_ok(out->kind);
        return;
    }

    /* 2. A registered or Ultimate DOOM.WAD in the user's directories. */
    if (try_registered(query, query->doomwaddir,
                       query->doomwaddir ? strlen(query->doomwaddir) : 0, out))
    {
        return;
    }

    if (query->doomwadpath != NULL)
    {
        const char *entry = query->doomwadpath;

        for (;;)
        {
            const char *sep = strchr(entry, ':');
            size_t len = sep ? (size_t) (sep - entry) : strlen(entry);

            if (try_registered(query, entry, len, out))
            {
                return;
            }
            if (sep == NULL)
            {
                break;
            }
            entry = sep + 1;
        }
    }

    if (query->home != NULL && query->home[0] != '\0')
    {
        n = snprintf(support, sizeof(support),
                     "%s/Library/Application Support/psDoom", query->home);
        if (n > 0 && (size_t) n < sizeof(support)
            && try_registered(query, support, (size_t) n, out))
        {
            return;
        }
    }

    /* 3. The bundled Freedoom. Named even when it is missing or damaged, so
     * the engine fails loudly instead of searching for another IWAD. */
    out->pass_iwad = 1;
    n = snprintf(out->path, sizeof(out->path), "%s/%s",
                 query->resources_dir ? query->resources_dir : ".",
                 bundled_name);
    if (n < 0 || (size_t) n >= sizeof(out->path))
    {
        out->path[0] = '\0';
        out->pass_iwad = 0;
        return;
    }
    out->kind = query->probe(out->path, query->probe_ctx);
    out->arena_ok = out->kind == PSD_IWAD_FREEDOOM1;
}
