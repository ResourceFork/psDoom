#!/usr/bin/env python3
"""Summoner plugin: your own processes (the native behaviour).

    processes.py describe
    processes.py list [sort=ID[:asc|desc],..] [group=ID] [limit=N]
                                       one JSON object per process you own,
                                       curated: sorted, de-duplicated, graded
    processes.py nudge <key> amount=N  raise its nice value by N (cumulative)
    processes.py remove <key>          SIGTERM

Dimensions: relevance (psDoom's own native ranking score, the default),
mem (resident bytes), cpu (percent of a core), age (seconds since start) to
sort by; app (process name) and parent (parent's name) to group by.

relevance reproduces psDoom's native ranking (src/psdoom/proc_select.c,
select_score/select_cmp): base 100, +50 for a process with a controlling
terminal, -80 for a background-noise name (Apple helpers, XPC services,
agents and daemons: psDoom's list), and +1 per 32 MiB of memory up to +120;
a newer (higher) pid wins a tie. So `summon use ps` with its default sort
picks the same kind of top processes as psDoom's native backend.

Curation, after sorting: the first process of each name keeps its place;
the 2nd and 3rd of the same name move below every first-of-its-name; any
further ones are dropped (one browser's forty helpers would otherwise fill
every monster slot). Noise names then move below everything else, on any
sort (relevance also scores them down, as psDoom does).

Safety: this plugin is trusted; psDoom does not filter what it lists
(docs/script-backend.md, "Trust"). So it leaves out, itself:
- psDoom's protected, session-critical names (loginwindow, WindowServer,
  Dock, Finder, SystemUIServer, launchd, kernel_task) plus the games and
  the usual terminal apps;
- pid 0 and 1;
- itself and its own parent chain (the Summoner process running it);
- the host chain Summoner passes in $SUMMONER_HOST_PIDS: the `summon get`
  that psDoom ran, its shell, psDoom itself and the launchers above it
  (psDoom's native "launcher chain" guard).
Only your own processes are listed (`ps -x`). Then `group` gathers members together, ordered by each
group's best member, and `limit` cuts the list. `grade` is the item's
place on the primary dimension among the listed items (1 = the best end
of the requested direction), so the monsters spread over the whole ladder.

The key is "<pid>@<start time>", so an action re-reads the process's start
time first and refuses if the pid now names a different process. Neither
action is undoable: an unprivileged user cannot lower a nice value back, and a
terminated process cannot be restarted. Standard library only.
"""

import json
import os
import re
import signal
import subprocess
import sys
import time

# psDoom's protected, session-critical names (src/psdoom/proc_select.c
# select_protected_names), plus the games and the usual terminal apps that
# launch them. The launcher chain itself comes from SUMMONER_HOST_PIDS.
DENY = {"loginwindow", "WindowServer", "Dock", "Finder", "SystemUIServer",
        "launchd", "kernel_task", "psDoom", "psdoom", "crispy-doom",
        "Terminal", "iTerm2"}

PS_FIELDS = "pid=,ppid=,rss=,pcpu=,tt=,lstart=,comm="

# psDoom's native noise list (src/psdoom/proc_select.c), matched as
# case-insensitive substrings of the process name.
NOISE = ("com.apple", "mdworker", "cfprefsd", "distnoted", "trustd",
         "nsurlsession", "analyticsd", "knowledge", "spotlight", "coreaudiod",
         "diagnostic", "crashpad", "mtlcompiler", "gputools", "assetsd",
         "synthesizer", "agent", "helper", "service", "daemon", "xpc", "ausp")
MAX_SAME_NAME = 3          # listed processes per name; the rest are dropped

# psDoom's select_score weights (src/psdoom/proc_select.c).
SCORE_BASE = 100
SCORE_INTERACTIVE = 50     # has a controlling tty
SCORE_NOISE = 80           # penalty: looks like a system helper
SCORE_MEM_MAX = 120        # cap on the memory bonus (1 per 32 MiB)

DIMENSIONS = [
    {"id": "relevance", "label": "Relevance", "kind": "numeric",
     "dir": "desc", "unit": "score", "roles": ["sort"]},
    {"id": "mem", "label": "Memory", "kind": "numeric", "dir": "desc",
     "unit": "bytes", "roles": ["sort"]},
    {"id": "cpu", "label": "CPU", "kind": "numeric", "dir": "desc",
     "unit": "percent", "roles": ["sort"]},
    {"id": "age", "label": "Age", "kind": "numeric", "dir": "desc",
     "unit": "seconds", "roles": ["sort"]},
    {"id": "app", "label": "App", "kind": "category", "roles": ["group"]},
    {"id": "parent", "label": "Parent", "kind": "category",
     "roles": ["group"]},
]
NUMERIC = {d["id"]: d["dir"] for d in DIMENSIONS if d["kind"] == "numeric"}
GROUPS = {d["id"] for d in DIMENSIONS if d["kind"] == "category"}
DEFAULT_SORT = [("relevance", "desc")]
ID_RE = re.compile(r"[a-z][a-z0-9_]{0,15}\Z")


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
        "dimensions": DIMENSIONS,
        "default_sort": "relevance",
    }


def host_pids(env=None):
    """The host chain Summoner passed: comma-separated pids above 1."""
    text = (os.environ if env is None else env).get("SUMMONER_HOST_PIDS", "")
    return {int(x) for x in text.split(",")
            if x.isdigit() and 1 < int(x) <= 2147483647}


def parent_map():
    parent = {}
    for line in ps(["-axo", "pid=,ppid="]).splitlines():
        f = line.split()
        if len(f) == 2 and f[0].isdigit() and f[1].isdigit():
            parent[int(f[0])] = int(f[1])
    return parent


def chain_of(pid, parent):
    out = set()
    while pid > 1 and pid not in out and len(out) < 64:
        out.add(pid)
        pid = parent.get(pid, 0)
    return out


def parse_ps(text, self_pid, exclude=()):
    """Parse `ps -xo PS_FIELDS` output. lstart is five words
    ("Thu Nov 12 09:01:02 2026"); comm is the rest of the line. Leaves out
    pid <= 1, the protected names, `self_pid` and its parent chain, and
    every pid in `exclude` (the host chain)."""
    parent = {}
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 2 and f[0].isdigit() and f[1].isdigit():
            parent[int(f[0])] = int(f[1])
    skip = chain_of(self_pid, parent) | set(exclude)
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
        try:
            started = time.mktime(time.strptime(" ".join(f[5:10]),
                                                "%a %b %d %H:%M:%S %Y"))
        except ValueError:
            started = None
        name = os.path.basename(" ".join(f[10:]))
        if pid <= 1 or pid in skip or name in DENY:
            continue
        rows.append({"pid": pid, "ppid": ppid, "rss": rss, "cpu": cpu,
                     "tty": tt, "start": start, "name": name,
                     "started": started})
    return rows


def items(rows, now=None):
    now = time.time() if now is None else now
    keys = {r["pid"]: "%d@%s" % (r["pid"], r["start"]) for r in rows}
    names = {r["pid"]: r["name"] for r in rows}
    out = []
    for r in rows:
        age = None
        if r.get("started") is not None:
            age = max(0, int(now - r["started"]))
        it = {"key": keys[r["pid"]], "name": r["name"],
              "size": r["rss"] * 1024, "load": int(r["cpu"] + 0.5),
              "background": r["tty"] == "??",
              "pid": r["pid"],
              "values": {"relevance": relevance(r), "mem": r["rss"] * 1024,
                         "cpu": r["cpu"], "age": age},
              "categories": {"app": r["name"],
                             "parent": names.get(r["ppid"], "launchd")}}
        if r["ppid"] in keys:
            it["parent"] = keys[r["ppid"]]
        out.append(it)
    return out


def relevance(r):
    """psDoom's select_score for one ps row."""
    score = SCORE_BASE
    if r["tty"] != "??":
        score += SCORE_INTERACTIVE
    if is_noise(r["name"]):
        score -= SCORE_NOISE
    return score + min(SCORE_MEM_MAX, (r["rss"] * 1024) >> 25)


def parse_request(words):
    """sort=ID[:dir],.. group=ID limit=N, tolerant: anything unknown is
    ignored (Summoner has already checked the request)."""
    sort, group, limit = [], None, None
    for w in words:
        k, _, v = w.partition("=")
        if k == "sort":
            for part in v.split(","):
                did, _, ddir = part.partition(":")
                if did in NUMERIC and all(did != x for x, _ in sort):
                    sort.append((did, ddir if ddir in ("asc", "desc")
                                 else NUMERIC[did]))
        elif k == "group" and v in GROUPS:
            group = v
        elif k == "limit" and v.isdigit() and int(v) > 0:
            limit = int(v)
    return sort or list(DEFAULT_SORT), group, limit


def is_noise(name):
    low = name.lower()
    return any(n in low for n in NOISE)


def curate(its, sort, group=None, limit=None):
    """Sort, demote duplicates and noise, gather groups, cut, grade."""
    def key(it):
        k = []
        for did, ddir in sort:
            v = it["values"].get(did)
            missing = v is None
            v = 0 if missing else v
            k.append((missing, -v if ddir == "desc" else v))
        # psDoom's tiebreak: the newer (higher) pid first
        return k + [-it.get("pid", 0)]

    its = sorted((dict(it) for it in its), key=key)   # never edit the input
    seen = {}
    tiers = ([], [], [], [])      # first, dupe, noise first, noise dupe
    for it in its:
        n = it["name"]
        seen[n] = seen.get(n, 0) + 1
        if seen[n] > MAX_SAME_NAME:
            continue
        noise = 2 if is_noise(n) else 0
        tiers[noise + (0 if seen[n] == 1 else 1)].append(it)
    its = [it for t in tiers for it in t]
    if group:
        order, members = [], {}
        for it in its:
            g = it["categories"].get(group) or "-"
            if g not in members:
                order.append(g)
                members[g] = []
            members[g].append(it)
            it["group"] = g
        its = [it for g in order for it in members[g]]
    if limit:
        its = its[:limit]
    # grade: place on the primary dimension among the listed items
    did, ddir = sort[0]
    vals = sorted({it["values"][did] for it in its
                   if it["values"].get(did) is not None},
                  reverse=(ddir == "desc"))
    pos = {v: i for i, v in enumerate(vals)}
    for it in its:
        v = it["values"].get(did)
        if v is not None:
            it["grade"] = 1.0 if len(vals) < 2 else \
                round(1.0 - pos[v] / (len(vals) - 1), 4)
    for it in its:
        it.pop("categories", None)
        it.pop("pid", None)
    return its


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
        sort, group, limit = parse_request(argv[1:])
        listed = items(parse_ps(ps(["-xo", PS_FIELDS]), os.getpid(),
                                host_pids()))
        for it in curate(listed, sort, group, limit):
            print(json.dumps(it))
        return 0
    if verb in ("nudge", "remove") and len(argv) >= 2:
        pid = resolve(argv[1])
        if pid is None:
            return result(False, "process is gone or its pid was reused")
        # Never act on the caller's own chain (the host that sent the event
        # and its launchers, as Summoner reports them now), whatever an
        # older listing said: the list may have been made for another caller.
        if pid in host_pids() or pid in chain_of(os.getpid(), parent_map()):
            return result(False, "refused: part of the calling chain")
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
