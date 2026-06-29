/*
 * psDoom -- fake process backend (for tests).
 *
 * A deterministic, in-memory backend: tests queue process snapshots (one served
 * per list() call, the last one repeating once the queue drains) and inspect a
 * log of the renice/kill calls the game made. No OS calls, so triage and policy
 * can be exercised off-machine and reproducibly.
 */

#ifndef PSDOOM_PROC_FAKE_H
#define PSDOOM_PROC_FAKE_H

#include "proc_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Make the fake the active backend and set the uid current_uid() reports.
 * Clears any queued snapshots and the action log. */
void proc_fake_install(unsigned int uid);

/* Queue one snapshot (a deep copy of `procs[0..n)`) to be returned by a future
 * list() call, in FIFO order. */
void proc_fake_push_snapshot(const psd_proc_t *procs, int n);

/* Forget queued snapshots and the action log (keeps the uid). */
void proc_fake_reset(void);

/* Action log inspectors. */
int proc_fake_renice_count(void);
int proc_fake_kill_count(void);
int proc_fake_last_renice_pid(void);
int proc_fake_last_renice_delta(void);
int proc_fake_last_kill_pid(void);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_PROC_FAKE_H */
