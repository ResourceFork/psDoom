#!/bin/sh
#
# psDoom reference responder -- act on a process.
#
# Reproduces psDoom's native response as a script: wounding renices the
# process (lower priority), killing sends SIGTERM. psDoom sends `post` for a
# wound and `delete` for a kill. Invoked by psDoom as:
#
#   respond-processes.sh <verb> <id> [amount=<n>] [label=<text>]
#
#   verb  : post | delete   (unknown verbs are a no-op -- forward compatibility)
#   id    : the pid from the poller
#   amount: nice step for `post` (psDoom sends 4; absent for `delete`)
#   label : the process name, when known (used only for logging here)
#   mode  : dryrun | post-only | delete -- the game's effect setting; this
#           script does nothing in dryrun and never deletes in post-only
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

# The host's effect mode (docs/script-backend.md): dryrun = act on nothing,
# post-only = never delete, delete = both. Missing or unknown = dryrun.
mode=dryrun
for kv in "$@"; do
    case $kv in
        mode=post-only) mode=post-only ;;
        mode=delete)    mode=delete ;;
        mode=*)         mode=dryrun ;;
    esac
done
case "$mode:$verb" in
    dryrun:*|post-only:delete) exit 0 ;;
esac

case "$verb" in
    post)
        # Lower the process's scheduling priority. (BSD `renice` sets an
        # absolute niceness; psDoom's native backend adds cumulatively. Close
        # enough for a wound -- bump `amount` for a harder hit.)
        renice "$amount" -p "$id" >/dev/null 2>&1
        ;;
    delete)
        kill "$id" >/dev/null 2>&1      # SIGTERM, like the native backend
        ;;
    *)
        : ;;                            # unknown verb: do nothing, succeed
esac

exit 0
