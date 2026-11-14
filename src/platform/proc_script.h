/*
 * psDoom -- external-script process backend.
 *
 * Implements the proc_backend interface by shelling out to user-supplied
 * commands, so psDoom can visualize and act on anything enumerable from a
 * script, not just OS processes. The wire contract (poll stdout format and
 * response argv format) is documented in docs/script-backend.md.
 *
 *     proc_backend  <-  proc_script  ->  PSDOOM_POLL_CMD  (enumerate, stdout)
 *                                    ->  PSDOOM_RESPOND_CMD (wound/kill, argv)
 */

#ifndef PSDOOM_PROC_SCRIPT_H
#define PSDOOM_PROC_SCRIPT_H

#include <stddef.h>         /* size_t */

#include "proc_backend.h"   /* psd_proc_t, proc_backend_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * If PSDOOM_POLL_CMD is set in the environment, configure the script backend
 * from PSDOOM_POLL_CMD / PSDOOM_RESPOND_CMD / PSDOOM_POLL_TIMEOUT_MS and make
 * it the active backend. Returns 1 if installed, 0 if PSDOOM_POLL_CMD is unset
 * (the native backend stays in effect).
 */
int proc_script_install_from_env(void);

/*
 * Configure and install the script backend explicitly (used by tests).
 * `respond_cmd` may be NULL (read-only); `timeout_ms` <= 0 uses the default.
 */
void proc_script_install(const char *poll_cmd, const char *respond_cmd,
                         int timeout_ms);

/*
 * Pure parser for poll output: parse the entity lines in `buf` into `out`
 * (capacity `max`), returning the count written. Engine- and OS-free (no
 * subprocess, no uid stamping) so the format can be exercised directly in
 * tests. See docs/script-backend.md for the line format.
 */
int psd_script_parse(const char *buf, psd_proc_t *out, int max);

/* ---- sort / group request (docs/script-backend.md, "Ordering") ---------- */

#define PSD_SCRIPT_ID_MAX   16   /* dimension id: [a-z][a-z0-9_]{0,15}      */
#define PSD_SCRIPT_DIMS_MAX 16   /* dimensions kept from one poll           */

typedef struct
{
    char id[PSD_SCRIPT_ID_MAX + 1];
    char label[32];
    int  numeric;
    int  sortable;
    int  groupable;
} psd_script_dim_t;

/* 1 if `id` is a well-formed dimension id (safe on a shell command line). */
int psd_script_id_ok(const char *id);

/* Pure: read the leading `#` comment block of a poll output: `# dim` lines
 * into dims (at most max; malformed ones skipped) and the `# sort id=`
 * the backend applied into applied. Returns the dimension count. */
int psd_script_parse_header(const char *buf, psd_script_dim_t *dims, int max,
                            char *applied, size_t applied_cap);

/* Pure: the poll command with the request appended: ` sort=<id>` (only a
 * well-formed id) and ` limit=<n>` (1..4096). Returns 0, and the bare base
 * command, if it would not fit in cap. */
int psd_script_poll_command(const char *base, const char *sort, int limit,
                            char *out, size_t cap);

/* 1 if the script backend is the active backend. */
int proc_script_active(void);

/* The candidate limit sent with each poll (once dimensions are declared). */
void proc_script_set_limit(int limit);

/* Sortable dimensions the last poll declared (0: no request is sent). */
int proc_script_sort_count(void);

/* Label of the sort the next poll asks for, NULL if none is known. */
const char *proc_script_sort_label(void);

/* Choose the next (dir >= 0) or previous sortable dimension. Takes effect
 * on the next poll. */
void proc_script_cycle_sort(int dir);

/* The host's effect mode, sent as `mode=` on every response: "dryrun",
 * "post-only" or "delete". Anything else sets "dryrun" and returns 0. The
 * default after install is "dryrun". */
int proc_script_set_mode(const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_SCRIPT_H */
