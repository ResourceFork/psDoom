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
#
# psDoom trusts a script's list and filters nothing (docs/script-backend.md,
# "Trust"), so this script leaves out, itself, what psDoom's native backend
# would: session-critical names, processes that are not fully yours (effective
# and real uid both yours: no setuid `login`/`sudo`), and its own ancestor
# chain -- this script's shell, psDoom that ran it, and psDoom's launchers.

me=$(id -u)

# This script's ancestors, from one listing: $$ and every parent above it.
chain=$(ps -axo pid=,ppid= | awk -v start="$$" '
    { parent[$1] = $2 }
    END {
        p = start
        for (n = 0; p > 1 && n < 64; n++) { printf "%s ", p; p = parent[p] }
    }')

# `ps -xo` lists your own processes (with or without a controlling terminal).
# Columns are headerless (trailing '='); comm is last so a path with spaces is
# safe to reassemble. tt is "??" for processes with no controlling terminal.
ps -xo pid=,ppid=,uid=,ruid=,rss=,pcpu=,tt=,comm= | awk -v me="$me" -v chain="$chain" -v self="$$" '
BEGIN {
    OFS = "\t"
    n = split(chain, c, " ")
    for (i = 1; i <= n; i++) skip[c[i]] = 1
}
{
    pid  = $1
    ppid = $2
    uid  = $3        # effective
    ruid = $4        # real
    rss  = $5        # KiB
    cpu  = $6        # percent
    tt   = $7

    # comm is the rest of the line; reduce to a basename for the label.
    name = $8
    for (i = 9; i <= NF; i++) name = name " " $i
    sub(/.*\//, "", name)

    if (pid + 0 <= 1) next            # 0/1 are reserved
    if (uid != me || ruid != me) next # not fully yours (setuid login, sudo)
    if (pid in skip) next             # this script, psDoom, its launchers
    if (ppid == self) next            # this script own ps and awk

    # Session-critical names (the native deny-list), the games, terminal
    # apps and the session plumbing they run, so a stray shot cannot end your
    # GUI or terminal session. Extend this list to taste.
    if (name ~ /^(loginwindow|WindowServer|Dock|Finder|SystemUIServer|launchd|kernel_task|psDoom|psdoom|crispy-doom|Terminal|iTerm2|login|sudo|su|sshd|sshd-session|ssh-agent|tmux|screen|Ghostty|ghostty|Alacritty|alacritty|kitty|WezTerm|wezterm-gui)$/)
        next
    if (name ~ /^iTermServer/) next
    if (name ~ /^-/) next             # a login shell (-zsh): its tab

    weight = rss * 1024              # KiB -> bytes (matches the size ladder)
    load   = int(cpu + 0.5)          # %CPU -> load (100 = one core)
    flags  = (tt == "??") ? 1 : 0    # no controlling tty -> background

    printf "id=%d\tparent=%d\tweight=%d\tload=%d\tflags=%d\tlabel=%s\n",
           pid, ppid, weight, load, flags, name
}'
