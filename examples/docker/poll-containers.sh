#!/bin/sh
#
# psDoom Docker poller -- running containers as monsters.
#
# Demonstrates wiring psDoom to something that isn't an OS process. Each
# running container becomes an entity keyed by its main PID (a stable, unique
# integer while the container runs), labelled with its name and sized by its
# configured memory limit.
#
#   export PSDOOM_POLL_CMD="$PWD/examples/docker/poll-containers.sh"
#   export PSDOOM_RESPOND_CMD="$PWD/examples/docker/respond-containers.sh"
#
# Note: this keeps to one `docker inspect` per container for clarity. To size
# by *live* usage instead of the configured limit, merge in
# `docker stats --no-stream` (CPU% -> load, MemUsage -> weight).

command -v docker >/dev/null 2>&1 || exit 0   # no docker: enumerate nothing

docker ps -q 2>/dev/null | while read -r cid; do
    docker inspect \
        -f '{{.State.Pid}}	{{.Name}}	{{.HostConfig.Memory}}' \
        "$cid" 2>/dev/null
done | awk -F'\t' '
BEGIN { OFS = "\t" }
{
    pid    = $1
    name   = $2; sub(/^\//, "", name)   # docker names come back as "/name"
    weight = $3                          # configured memory limit (0 = unlimited)

    if (pid + 0 <= 1) next
    printf "id=%d\tlabel=%s\tweight=%d\tload=0\n", pid, name, weight
}'

exit 0
