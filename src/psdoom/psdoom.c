/*
 * psDoom -- process-management layer (stub implementation).
 *
 * No-ops for now: this increment wires the engine call-sites and the build
 * without changing gameplay. The real process enumeration / renice / kill lands
 * in a later increment, backed by src/platform/macos/proc_macos.*.
 */

#include "psdoom.h"

void psdoom_init(void)
{
    /* TODO: open the macOS process backend; load the allow/deny filter;
     * default to the current UID's processes. */
}

void psdoom_sync(void)
{
    /* TODO: enumerate processes; spawn monsters for new ones; retire monsters
     * whose processes are gone. Self-throttle to ~1 Hz. */
}

void psdoom_wound(struct mobj_s *target)
{
    (void)target;
    /* TODO: if target is a process-monster, setpriority(PRIO_PROCESS, pid, +n). */
}

void psdoom_kill(struct mobj_s *target)
{
    (void)target;
    /* TODO: if target is a process-monster, kill(pid, SIGTERM) -> SIGKILL. */
}
