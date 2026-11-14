#!/bin/sh
#
# psDoom Docker responder -- act on a container.
#
#   respond-containers.sh <verb> <id> [amount=<n>] [label=<text>]
#
# `id` is the container's main PID (what the poller emitted). We map it back to
# a container, then: post (a wound) = throttle its CPU shares, delete (a
# kill) = `docker kill`.

command -v docker >/dev/null 2>&1 || exit 0

verb=$1
id=$2
[ -n "$verb" ] && [ -n "$id" ] || exit 0
shift 2

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

# Find the container whose main PID matches `id`.
cid=""
for c in $(docker ps -q 2>/dev/null); do
    p=$(docker inspect -f '{{.State.Pid}}' "$c" 2>/dev/null)
    if [ "$p" = "$id" ]; then
        cid=$c
        break
    fi
done
[ -n "$cid" ] || exit 0

case "$verb" in
    post)
        # Lower the container's CPU weight (the renice analogue).
        docker update --cpu-shares 256 "$cid" >/dev/null 2>&1
        ;;
    delete)
        docker kill "$cid" >/dev/null 2>&1
        ;;
    *)
        : ;;
esac

exit 0
