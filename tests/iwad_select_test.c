/*
 * psDoom -- IWAD selection host tests (src/app/macos/iwad_select.c).
 *
 * Engine-free. Selection runs against a fake probe that maps paths to IWAD
 * kinds; classification runs against synthetic lump directories; the real
 * file probe reads small synthetic WADs written under $TMPDIR.
 */

#include "iwad_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int *g_run;
static int *g_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        (*g_run)++;                                                          \
        if (!(cond)) {                                                       \
            (*g_failed)++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* ---- synthetic lump directories ------------------------------------------ */

#define MAX_TEST_LUMPS 64

typedef struct
{
    unsigned char header[12];
    unsigned char dir[MAX_TEST_LUMPS * 16];
    uint32_t numlumps;
} test_wad_t;

static void wad_init(test_wad_t *w, const char *ident)
{
    memset(w, 0, sizeof(*w));
    memcpy(w->header, ident, 4);
    w->header[8] = 12; /* directory right after the header */
}

static void wad_add(test_wad_t *w, const char *name)
{
    unsigned char *entry = w->dir + (size_t) w->numlumps * 16;

    memset(entry, 0, 16);
    entry[0] = 12;
    strncpy((char *) entry + 8, name, 8);
    w->numlumps++;
    w->header[4] = (unsigned char) (w->numlumps & 0xff);
    w->header[5] = (unsigned char) ((w->numlumps >> 8) & 0xff);
}

static const char *const registered_set[] = {
    "E2M1", "E2M2", "E2M3", "E2M4", "E2M5", "E2M6", "E2M7", "E2M8", "E2M9",
    "E3M1", "E3M2", "E3M3", "E3M4", "E3M5", "E3M6", "E3M7", "E3M8", "E3M9",
    "DPHOOF", "BFGGA0", "HEADA1", "CYBRA1", "SPIDA1D1"
};

static void wad_shareware(test_wad_t *w)
{
    wad_init(w, "IWAD");
    wad_add(w, "PLAYPAL");
    wad_add(w, "E1M1");
    wad_add(w, "E1M9");
}

static void wad_registered(test_wad_t *w, int retail, const char *skip)
{
    size_t i;

    wad_shareware(w);
    for (i = 0; i < sizeof(registered_set) / sizeof(registered_set[0]); i++)
    {
        if (skip == NULL || strcmp(skip, registered_set[i]) != 0)
        {
            wad_add(w, registered_set[i]);
        }
    }
    if (retail)
    {
        wad_add(w, "E4M1");
    }
}

static psd_iwad_kind_t classify(const test_wad_t *w)
{
    return psd_iwad_classify(w->header, w->dir, w->numlumps);
}

static void test_classify(void)
{
    test_wad_t w;

    wad_shareware(&w);
    CHECK(classify(&w) == PSD_IWAD_SHAREWARE);

    wad_registered(&w, 0, NULL);
    CHECK(classify(&w) == PSD_IWAD_REGISTERED);

    wad_registered(&w, 1, NULL); /* Ultimate Doom */
    CHECK(classify(&w) == PSD_IWAD_REGISTERED);

    /* A registered-looking IWAD missing a lump Crispy's lockout requires. */
    wad_registered(&w, 0, "CYBRA1");
    CHECK(classify(&w) == PSD_IWAD_OTHER);
    wad_registered(&w, 1, "SPIDA1D1");
    CHECK(classify(&w) == PSD_IWAD_OTHER);

    /* Freedoom Phase 1: exempt from the lockout, so no id lump set needed. */
    wad_init(&w, "IWAD");
    wad_add(&w, "FREEDOOM");
    wad_add(&w, "E1M1");
    wad_add(&w, "E4M1");
    CHECK(classify(&w) == PSD_IWAD_FREEDOOM1);

    /* Freedoom Phase 2, FreeDM and Doom II are MAPxx games. */
    wad_init(&w, "IWAD");
    wad_add(&w, "FREEDOOM");
    wad_add(&w, "MAP01");
    CHECK(classify(&w) == PSD_IWAD_COMMERCIAL);
    wad_add(&w, "FREEDM");
    CHECK(classify(&w) == PSD_IWAD_COMMERCIAL);
    wad_init(&w, "IWAD");
    wad_add(&w, "MAP01");
    wad_add(&w, "MAP32");
    CHECK(classify(&w) == PSD_IWAD_COMMERCIAL);

    /* Neither E1M1 nor MAP01. */
    wad_init(&w, "IWAD");
    wad_add(&w, "PLAYPAL");
    CHECK(classify(&w) == PSD_IWAD_OTHER);

    /* Lump names match whole: E1M10 is not E1M1. */
    wad_init(&w, "IWAD");
    wad_add(&w, "E1M10");
    CHECK(classify(&w) == PSD_IWAD_OTHER);

    /* A PWAD is not an IWAD, whatever it contains. */
    wad_registered(&w, 1, NULL);
    memcpy(w.header, "PWAD", 4);
    CHECK(classify(&w) == PSD_IWAD_INVALID);
    CHECK(psd_iwad_classify(NULL, NULL, 0) == PSD_IWAD_INVALID);

    CHECK(psd_iwad_kind_arena_ok(PSD_IWAD_REGISTERED));
    CHECK(psd_iwad_kind_arena_ok(PSD_IWAD_FREEDOOM1));
    CHECK(!psd_iwad_kind_arena_ok(PSD_IWAD_SHAREWARE));
    CHECK(!psd_iwad_kind_arena_ok(PSD_IWAD_COMMERCIAL));
    CHECK(!psd_iwad_kind_arena_ok(PSD_IWAD_OTHER));
    CHECK(!psd_iwad_kind_arena_ok(PSD_IWAD_INVALID));
    CHECK(!psd_iwad_kind_arena_ok(PSD_IWAD_MISSING));
}

/* ---- selection with a fake file system ----------------------------------- */

#define RES "/App/psDoom.app/Contents/Resources"
#define SUPPORT "/Users/me/Library/Application Support/psDoom"

typedef struct
{
    const char *path;
    psd_iwad_kind_t kind;
} fake_file_t;

typedef struct
{
    const fake_file_t *files;
    int probes;
    int probed_unexpected; /* probed a name other than DOOM.WAD/doom.wad */
} fake_fs_t;

static psd_iwad_kind_t fake_probe(const char *path, void *ctx)
{
    fake_fs_t *fs = ctx;
    const char *base = strrchr(path, '/');
    const fake_file_t *f;

    fs->probes++;
    base = base ? base + 1 : path;
    if (strcmp(base, "DOOM.WAD") != 0 && strcmp(base, "doom.wad") != 0
        && strcmp(path, RES "/freedoom1.wad") != 0)
    {
        fs->probed_unexpected++;
    }

    for (f = fs->files; f->path != NULL; f++)
    {
        if (strcmp(f->path, path) == 0)
        {
            return f->kind;
        }
    }
    return PSD_IWAD_MISSING;
}

static void select_with(const fake_file_t *files, const char *explicit_iwad,
                        const char *waddir, const char *wadpath,
                        const char *home, psd_iwad_choice_t *out,
                        fake_fs_t *fs)
{
    psd_iwad_query_t q;

    memset(fs, 0, sizeof(*fs));
    fs->files = files;
    q.explicit_iwad = explicit_iwad;
    q.doomwaddir = waddir;
    q.doomwadpath = wadpath;
    q.home = home;
    q.resources_dir = RES;
    q.probe = fake_probe;
    q.probe_ctx = fs;
    psd_iwad_select(&q, out);
}

static void test_select_default_freedoom(void)
{
    static const fake_file_t files[] = {
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    select_with(files, NULL, NULL, NULL, "/Users/me", &c, &fs);
    CHECK(c.pass_iwad);
    CHECK(strcmp(c.path, RES "/freedoom1.wad") == 0);
    CHECK(c.kind == PSD_IWAD_FREEDOOM1);
    CHECK(c.arena_ok);
    CHECK(fs.probed_unexpected == 0); /* never looked for DOOM1/DOOM2.WAD */

    /* No HOME: the support directory is skipped, Freedoom still chosen. */
    select_with(files, NULL, NULL, NULL, NULL, &c, &fs);
    CHECK(c.pass_iwad && c.arena_ok);
    CHECK(strcmp(c.path, RES "/freedoom1.wad") == 0);
}

static void test_select_registered_support_dir(void)
{
    static const fake_file_t files[] = {
        { SUPPORT "/DOOM.WAD", PSD_IWAD_REGISTERED },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    select_with(files, NULL, NULL, NULL, "/Users/me", &c, &fs);
    CHECK(c.pass_iwad);
    CHECK(strcmp(c.path, SUPPORT "/DOOM.WAD") == 0);
    CHECK(c.kind == PSD_IWAD_REGISTERED);
    CHECK(c.arena_ok);
}

static void test_select_lowercase_name(void)
{
    static const fake_file_t files[] = {
        { "/wads/doom.wad", PSD_IWAD_REGISTERED },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    select_with(files, NULL, "/wads", NULL, "/Users/me", &c, &fs);
    CHECK(strcmp(c.path, "/wads/doom.wad") == 0);
    CHECK(c.arena_ok);
}

static void test_select_env_precedence(void)
{
    static const fake_file_t files[] = {
        { "/dir/DOOM.WAD", PSD_IWAD_REGISTERED },
        { "/p1/DOOM.WAD", PSD_IWAD_REGISTERED },
        { "/p2/DOOM.WAD", PSD_IWAD_REGISTERED },
        { SUPPORT "/DOOM.WAD", PSD_IWAD_REGISTERED },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    static const fake_file_t path_only[] = {
        { "/p2/DOOM.WAD", PSD_IWAD_REGISTERED },
        { SUPPORT "/DOOM.WAD", PSD_IWAD_REGISTERED },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    /* DOOMWADDIR before DOOMWADPATH before the support directory. */
    select_with(files, NULL, "/dir", "/p1:/p2", "/Users/me", &c, &fs);
    CHECK(strcmp(c.path, "/dir/DOOM.WAD") == 0);

    /* DOOMWADPATH entries in order. */
    select_with(files, NULL, NULL, "/p1:/p2", "/Users/me", &c, &fs);
    CHECK(strcmp(c.path, "/p1/DOOM.WAD") == 0);

    /* Empty entries are skipped and later entries still searched; DOOMWADPATH
     * still comes before the support directory. */
    select_with(path_only, NULL, "/empty", ":/p1::/p2:", "/Users/me", &c, &fs);
    CHECK(strcmp(c.path, "/p2/DOOM.WAD") == 0);
    CHECK(c.arena_ok);

    /* Set but empty DOOMWADDIR/DOOMWADPATH are harmless. */
    select_with(path_only, NULL, "", "", "/Users/me", &c, &fs);
    CHECK(strcmp(c.path, SUPPORT "/DOOM.WAD") == 0);
}

static void test_select_never_shareware_or_doom2(void)
{
    /* A shareware or Doom II IWAD under the registered name is passed over,
     * and the bundled Freedoom is used instead. */
    static const fake_file_t files[] = {
        { "/dir/DOOM.WAD", PSD_IWAD_SHAREWARE },
        { "/p1/DOOM.WAD", PSD_IWAD_COMMERCIAL },
        { "/p1/doom.wad", PSD_IWAD_COMMERCIAL },
        { SUPPORT "/DOOM.WAD", PSD_IWAD_OTHER },
        { SUPPORT "/doom.wad", PSD_IWAD_INVALID },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    select_with(files, NULL, "/dir", "/p1", "/Users/me", &c, &fs);
    CHECK(c.pass_iwad);
    CHECK(strcmp(c.path, RES "/freedoom1.wad") == 0);
    CHECK(c.kind == PSD_IWAD_FREEDOOM1);
    CHECK(c.arena_ok);
    CHECK(fs.probed_unexpected == 0);
}

static void test_select_bundled_missing(void)
{
    static const fake_file_t files[] = {
        { SUPPORT "/DOOM.WAD", PSD_IWAD_SHAREWARE },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    /* Still names the bundled file so the engine reports it, and never lets
     * the engine's own search fall back to a shareware IWAD. */
    select_with(files, NULL, NULL, NULL, "/Users/me", &c, &fs);
    CHECK(c.pass_iwad);
    CHECK(strcmp(c.path, RES "/freedoom1.wad") == 0);
    CHECK(c.kind == PSD_IWAD_MISSING);
    CHECK(!c.arena_ok);
}

static void test_select_explicit_iwad(void)
{
    static const fake_file_t files[] = {
        { "/w/DOOM1.WAD", PSD_IWAD_SHAREWARE },
        { "/w/DOOM.WAD", PSD_IWAD_REGISTERED },
        { "/w/DOOM2.WAD", PSD_IWAD_COMMERCIAL },
        { "/w/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { SUPPORT "/DOOM.WAD", PSD_IWAD_REGISTERED },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    /* Shareware on request: stock Crispy, no arena, nothing added. */
    select_with(files, "/w/DOOM1.WAD", NULL, NULL, "/Users/me", &c, &fs);
    CHECK(!c.pass_iwad);
    CHECK(c.kind == PSD_IWAD_SHAREWARE);
    CHECK(!c.arena_ok);
    CHECK(fs.probes == 1); /* the explicit file only: no search */

    /* Registered and Freedoom on request keep the arena. */
    select_with(files, "/w/DOOM.WAD", NULL, NULL, "/Users/me", &c, &fs);
    CHECK(!c.pass_iwad && c.arena_ok && c.kind == PSD_IWAD_REGISTERED);
    select_with(files, "/w/freedoom1.wad", NULL, NULL, "/Users/me", &c, &fs);
    CHECK(!c.pass_iwad && c.arena_ok && c.kind == PSD_IWAD_FREEDOOM1);

    /* Doom II on request: its campaign, no arena. */
    select_with(files, "/w/DOOM2.WAD", NULL, NULL, "/Users/me", &c, &fs);
    CHECK(!c.pass_iwad && !c.arena_ok && c.kind == PSD_IWAD_COMMERCIAL);

    /* An explicit name the launcher cannot find is not overridden by a
     * registered IWAD elsewhere, and gets no arena. */
    select_with(files, "DOOM1.WAD", NULL, NULL, "/Users/me", &c, &fs);
    CHECK(!c.pass_iwad && !c.arena_ok && c.kind == PSD_IWAD_MISSING);
    CHECK(!c.replace_explicit);
}

static void test_select_explicit_bare_name(void)
{
    static const fake_file_t files[] = {
        { SUPPORT "/DOOM.WAD", PSD_IWAD_REGISTERED },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { "/w/DOOM1.WAD", PSD_IWAD_SHAREWARE },
        { "/w/DOOM2.WAD", PSD_IWAD_COMMERCIAL },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    /* Found through the launcher's own search path, in either case; the
     * launcher hands the engine the resolved file. */
    select_with(files, "doom.wad", NULL, NULL, "/Users/me", &c, &fs);
    CHECK(c.replace_explicit && !c.pass_iwad);
    CHECK(strcmp(c.path, SUPPORT "/DOOM.WAD") == 0);
    CHECK(c.kind == PSD_IWAD_REGISTERED && c.arena_ok);

    select_with(files, "FREEDOOM1.WAD", NULL, NULL, "/Users/me", &c, &fs);
    CHECK(c.replace_explicit);
    CHECK(strcmp(c.path, RES "/freedoom1.wad") == 0);
    CHECK(c.kind == PSD_IWAD_FREEDOOM1 && c.arena_ok);

    /* With the caller's DOOMWADDIR/DOOMWADPATH, those are searched instead,
     * and shareware or Doom II found there still gets no arena. */
    select_with(files, "doom1.wad", "/w", NULL, "/Users/me", &c, &fs);
    CHECK(c.replace_explicit);
    CHECK(strcmp(c.path, "/w/DOOM1.WAD") == 0);
    CHECK(c.kind == PSD_IWAD_SHAREWARE && !c.arena_ok);

    select_with(files, "DOOM2.WAD", NULL, "/x:/w", "/Users/me", &c, &fs);
    CHECK(c.replace_explicit && strcmp(c.path, "/w/DOOM2.WAD") == 0);
    CHECK(c.kind == PSD_IWAD_COMMERCIAL && !c.arena_ok);

    select_with(files, "freedoom1.wad", NULL, "/x", "/Users/me", &c, &fs);
    CHECK(!c.replace_explicit && !c.arena_ok && c.kind == PSD_IWAD_MISSING);
}

static void test_select_file_valued_entries(void)
{
    static const fake_file_t files[] = {
        { "/Games/DOOM.WAD", PSD_IWAD_REGISTERED },
        { "/g/doom.wad", PSD_IWAD_REGISTERED },
        { "/s/DOOM.WAD", PSD_IWAD_SHAREWARE },
        { RES "/freedoom1.wad", PSD_IWAD_FREEDOOM1 },
        { NULL, PSD_IWAD_MISSING }
    };
    psd_iwad_choice_t c;
    fake_fs_t fs;

    /* An entry may name the DOOM.WAD file itself, as the engine allows. */
    select_with(files, NULL, "/Games/DOOM.WAD", NULL, "/Users/me", &c, &fs);
    CHECK(c.pass_iwad && strcmp(c.path, "/Games/DOOM.WAD") == 0);
    CHECK(c.arena_ok);

    select_with(files, NULL, NULL, "/s/DOOM.WAD:/g/doom.wad", "/Users/me", &c, &fs);
    CHECK(strcmp(c.path, "/g/doom.wad") == 0); /* shareware entry skipped */
    CHECK(c.kind == PSD_IWAD_REGISTERED);
}

/* ---- the real file probe -------------------------------------------------- */

static int write_file(const char *path, const void *a, size_t alen,
                      const void *b, size_t blen)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (f == NULL)
    {
        return 0;
    }
    ok = fwrite(a, 1, alen, f) == alen && (blen == 0 || fwrite(b, 1, blen, f) == blen);
    return fclose(f) == 0 && ok;
}

static void test_probe_file(void)
{
    const char *td = getenv("TMPDIR");
    char path[1024];
    test_wad_t w;

    if (td == NULL || td[0] == '\0')
    {
        td = "/tmp";
    }
    snprintf(path, sizeof(path), "%s/psd_iwad_%d.wad", td, (int) getpid());

    wad_registered(&w, 1, NULL);
    CHECK(write_file(path, w.header, 12, w.dir, (size_t) w.numlumps * 16));
    CHECK(psd_iwad_probe_file(path, NULL) == PSD_IWAD_REGISTERED);

    wad_shareware(&w);
    CHECK(write_file(path, w.header, 12, w.dir, (size_t) w.numlumps * 16));
    CHECK(psd_iwad_probe_file(path, NULL) == PSD_IWAD_SHAREWARE);

    wad_init(&w, "IWAD");
    wad_add(&w, "FREEDOOM");
    wad_add(&w, "E1M1");
    CHECK(write_file(path, w.header, 12, w.dir, (size_t) w.numlumps * 16));
    CHECK(psd_iwad_probe_file(path, NULL) == PSD_IWAD_FREEDOOM1);

    /* A directory that runs past the end of the file. */
    CHECK(write_file(path, w.header, 12, w.dir, 16));
    CHECK(psd_iwad_probe_file(path, NULL) == PSD_IWAD_INVALID);

    /* Too short for a header. */
    CHECK(write_file(path, "IWA", 3, NULL, 0));
    CHECK(psd_iwad_probe_file(path, NULL) == PSD_IWAD_INVALID);

    unlink(path);
    CHECK(psd_iwad_probe_file(path, NULL) == PSD_IWAD_MISSING);

    /* A FIFO is refused without being opened (opening it would block). */
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(psd_iwad_probe_file(path, NULL) == PSD_IWAD_MISSING);
    unlink(path);
    CHECK(psd_iwad_probe_file(td, NULL) == PSD_IWAD_MISSING); /* a directory */
    CHECK(psd_iwad_probe_file("", NULL) == PSD_IWAD_MISSING);
}

void iwad_select_tests(int *checks_run, int *checks_failed)
{
    g_run = checks_run;
    g_failed = checks_failed;

    test_classify();
    test_select_default_freedoom();
    test_select_registered_support_dir();
    test_select_lowercase_name();
    test_select_env_precedence();
    test_select_never_shareware_or_doom2();
    test_select_bundled_missing();
    test_select_explicit_iwad();
    test_select_explicit_bare_name();
    test_select_file_valued_entries();
    test_probe_file();
}
