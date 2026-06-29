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

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_SCRIPT_H */
