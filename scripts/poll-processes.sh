#!/bin/sh
#
# psDoom reference poller -- your processes as monsters.
#
# Reproduces psDoom's native enumeration as a script: emit one entity per
# process you own, sized by resident memory (weight) and CPU load (load), so
# the existing classifier turns memory/CPU hogs into bigger, meaner monsters.
# See docs/script-backend.md for the line format.
#
# Wire it up:
#   export PSDOOM_POLL_CMD="$PWD/scripts/poll-processes.sh"
#   export PSDOOM_RESPOND_CMD="$PWD/scripts/respond-processes.sh"
#
# Output: TAB-separated key=value lines on stdout, one per process:
#   id=<pid>  parent=<ppid>  weight=<rss-bytes>  load=<cpu%>  flags=<bit0=no-tty>  label=<name>

# `ps -xo` lists your own processes (with or without a controlling terminal).
# Columns are headerless (trailing '='); comm is last so a path with spaces is
# safe to reassemble. tt is "??" for processes with no controlling terminal.
ps -xo pid=,ppid=,rss=,pcpu=,tt=,comm= | awk '
BEGIN { OFS = "\t" }
{
    pid  = $1
    ppid = $2
    rss  = $3        # KiB
    cpu  = $4        # percent
    tt   = $5

    # comm is the rest of the line; reduce to a basename for the label.
    name = $6
    for (i = 7; i <= NF; i++) name = name " " $i
    sub(/.*\//, "", name)

    if (pid + 0 <= 1) next   # 0/1 are reserved

    # Mirror the native session-critical deny-list so a stray shot through this
    # script cannot signal something that would end your GUI session. In script
    # mode YOU own safety -- extend this list to taste.
    if (name ~ /^(loginwindow|WindowServer|Dock|Finder|SystemUIServer|launchd|kernel_task)$/)
        next

    weight = rss * 1024              # KiB -> bytes (matches the size ladder)
    load   = int(cpu + 0.5)          # %CPU -> load (100 = one core)
    flags  = (tt == "??") ? 1 : 0    # no controlling tty -> background

    printf "id=%d\tparent=%d\tweight=%d\tload=%d\tflags=%d\tlabel=%s\n",
           pid, ppid, weight, load, flags, name
}'
