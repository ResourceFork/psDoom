#!/bin/sh
#
# psDoom reference responder -- act on a process.
#
# Reproduces psDoom's native response as a script: wounding renices the
# process (lower priority), killing sends SIGTERM. Invoked by psDoom as:
#
#   respond-processes.sh <verb> <id> [amount=<n>] [label=<text>]
#
#   verb  : wound | kill   (unknown verbs are a no-op -- forward compatibility)
#   id    : the pid from the poller
#   amount: nice step for `wound` (psDoom sends 4; absent for `kill`)
#   label : the process name, when known (used only for logging here)
#
# psDoom execs this directly (no shell), so $id/$label are inert arguments.

verb=$1
id=$2
[ -n "$verb" ] && [ -n "$id" ] || exit 0
shift 2

amount=4
for kv in "$@"; do
    case $kv in
        amount=*) amount=${kv#amount=} ;;
    esac
done

case "$verb" in
    wound)
        # Lower the process's scheduling priority. (BSD `renice` sets an
        # absolute niceness; psDoom's native backend adds cumulatively. Close
        # enough for a wound -- bump `amount` for a harder hit.)
        renice "$amount" -p "$id" >/dev/null 2>&1
        ;;
    kill)
        kill "$id" >/dev/null 2>&1      # SIGTERM, like the native backend
        ;;
    *)
        : ;;                            # unknown verb: do nothing, succeed
esac

exit 0
