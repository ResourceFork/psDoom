/*
 * psDoom -- process-monster lifecycle rules (engine-free). See the header.
 */

#include "psdoom_lifecycle.h"

psd_save_t psd_save_disposition(int psd_pid, int map_placed)
{
    if (psd_pid == 0)
    {
        return PSD_SAVE_KEEP;
    }
    /* Any process tag -- a live process, a swarm soul, a gone monster -- marks
     * a mobj backed only by the process list. A map-placed one still belongs
     * to the map, so it stays, as a plain monster. */
    return map_placed ? PSD_SAVE_STRIP : PSD_SAVE_SKIP;
}

psd_end_action_t psd_end_action(psd_end_reason_t reason)
{
    switch (reason)
    {
    case PSD_END_LEFT_LIST:
    case PSD_END_IDENTITY_LOST:
        return PSD_END_TELEPORT_OUT;
    case PSD_END_KILLED:
    default:
        return PSD_END_DIE;
    }
}
