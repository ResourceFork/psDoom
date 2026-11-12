#!/usr/bin/env python3
"""psDoom harness plugin: your own processes (the native behaviour).

    processes.py describe
    processes.py list                  one JSON object per process you own
    processes.py nudge <key> amount=N  raise its nice value by N (cumulative)
    processes.py remove <key>          SIGTERM

The key is "<pid>@<start time>", so an action re-reads the process's start
time first and refuses if the pid now names a different process. Neither
action is undoable: an unprivileged user cannot lower a nice value back, and a
terminated process cannot be restarted. Standard library only.
"""

import json
import os
import signal
import subprocess
import sys

# Session-critical names, mirroring scripts/poll-processes.sh, plus the game.
DENY = {"loginwindow", "WindowServer", "Dock", "Finder", "SystemUIServer",
        "launchd", "kernel_task", "psDoom", "psdoom", "crispy-doom"}

PS_FIELDS = "pid=,ppid=,rss=,pcpu=,tt=,lstart=,comm="


def describe():
    return {
        "protocol": 1,
        "name": "processes",
        "verbs": ["list", "nudge", "remove"],
        "size": {"meaning": "resident memory", "unit": "bytes",
                 "scale": "native"},
        "load": {"meaning": "CPU use, 100 = one core", "unit": "percent",
                 "scale": "native"},
        "nudge": {"meaning": "raise the nice value by amount"},
        "remove": {"meaning": "send SIGTERM", "undoable": False},
    }


def parse_ps(text, self_pid):
    """Parse `ps -xo PS_FIELDS` output. lstart is five words
    ("Thu Nov 12 09:01:02 2026"); comm is the rest of the line."""
    rows = []
    for line in text.splitlines():
        f = line.split()
        if len(f) < 11:
            continue
        try:
            pid, ppid, rss = int(f[0]), int(f[1]), int(f[2])
            cpu = float(f[3])
        except ValueError:
            continue
        tt = f[4]
        start = "-".join(f[5:10])
        name = os.path.basename(" ".join(f[10:]))
        if pid <= 1 or pid == self_pid or name in DENY:
            continue
        rows.append({"pid": pid, "ppid": ppid, "rss": rss, "cpu": cpu,
                     "tty": tt, "start": start, "name": name})
    return rows


def items(rows):
    keys = {r["pid"]: "%d@%s" % (r["pid"], r["start"]) for r in rows}
    out = []
    for r in rows:
        it = {"key": keys[r["pid"]], "name": r["name"],
              "size": r["rss"] * 1024, "load": int(r["cpu"] + 0.5),
              "background": r["tty"] == "??"}
        if r["ppid"] in keys:
            it["parent"] = keys[r["ppid"]]
        out.append(it)
    return out


def ps(args):
    return subprocess.run(["ps"] + args, stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=10).stdout


def resolve(key):
    """key -> pid if that pid still has the recorded start time, else None."""
    pid_s, sep, start = key.partition("@")
    if not sep or not pid_s.isdigit() or int(pid_s) <= 1:
        return None
    pid = int(pid_s)
    now = "-".join(ps(["-o", "lstart=", "-p", str(pid)]).split())
    return pid if now and now == start else None


def result(ok, message="", undo=None):
    print(json.dumps({"ok": ok, "message": message, "undo": undo}))
    return 0 if ok else 1


def main(argv):
    if not argv:
        return 2
    verb = argv[0]
    if verb == "describe":
        print(json.dumps(describe()))
        return 0
    if verb == "list":
        for it in items(parse_ps(ps(["-xo", PS_FIELDS]), os.getpid())):
            print(json.dumps(it))
        return 0
    if verb in ("nudge", "remove") and len(argv) >= 2:
        pid = resolve(argv[1])
        if pid is None:
            return result(False, "process is gone or its pid was reused")
        if verb == "nudge":
            amount = 4
            for kv in argv[2:]:
                if kv.startswith("amount=") and kv[7:].isdigit():
                    amount = int(kv[7:])
            cur = os.getpriority(os.PRIO_PROCESS, pid)
            os.setpriority(os.PRIO_PROCESS, pid, min(20, cur + amount))
            return result(True, "nice %d -> %d" % (cur, min(20, cur + amount)))
        os.kill(pid, signal.SIGTERM)
        return result(True, "SIGTERM sent")
    return result(False, "unsupported verb")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
