#!/bin/sh
#
# psDoom Docker responder -- act on a container.
#
#   respond-containers.sh <verb> <id> [amount=<n>] [label=<text>]
#
# `id` is the container's main PID (what the poller emitted). We map it back to
# a container, then: wound = throttle its CPU shares, kill = `docker kill`.

command -v docker >/dev/null 2>&1 || exit 0

verb=$1
id=$2
[ -n "$verb" ] && [ -n "$id" ] || exit 0

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
    wound)
        # Lower the container's CPU weight (the renice analogue).
        docker update --cpu-shares 256 "$cid" >/dev/null 2>&1
        ;;
    kill)
        docker kill "$cid" >/dev/null 2>&1
        ;;
    *)
        : ;;
esac

exit 0
