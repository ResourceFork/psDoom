#!/usr/bin/env python3
"""Summoner (`summon`) -- one poll/respond pair for any list of things.

Summoner sits between a ps-app's script backend (the script contract,
psDoom's docs/script-backend.md) and a *plugin*: a small
executable that can list items, nudge one, and remove one. psDoom runs:

    PSDOOM_POLL_CMD    = summon get
    PSDOOM_RESPOND_CMD = summon          (psDoom appends post|delete <id>)

and Summoner adds what every plugin would otherwise rewrite: stable
integer ids, size/load scaling onto the games' ladders, 31-byte labels,
background caching under the 1-second poll timeout, and a safety layer
(dry run by default, per-plugin remove opt-in, rate limit, audit log, undo).

Design: psDoom docs/summoner.md. Python 3.10+ standard library only.
Self-contained: nothing here imports or reads from outside summoner/.

    summon [--root DIR] [--plugin NAME | --config FILE] COMMAND ...

    get [sort=ID[:asc|desc][,ID..]] [group=ID] [limit=N]
                               print the poll output, in the plugin's order
                               for that request (see `describe` for the IDs)
    post <id> [k=v...]         nudge: lower its priority
    delete <id> [k=v...]       remove it
    use <name>                 set and persist the current plugin
    plugins                    list known plugins, marking the current one
    refresh [request words]    run the plugin's list now
    describe                   print the plugin's describe, validated
    undo <action-id>           replay a recorded undo through the plugin

The root holds `current` and `plugins/<name>.json`; it is --root, else
$SUMMONER_ROOT, else ~/.config/summoner. A plugin's state defaults to
<root>/state/<name>. `ps` (your processes) is built in, dry run.
"""

import errno
import fcntl
import json
import os
import secrets
import re
import signal
import stat
import subprocess
import sys
import time
import unicodedata

PROTOCOL = 1
SUMMONER_DIR = os.path.dirname(os.path.abspath(__file__))

# The games' ladders (the script contract's "Size ladder" / "Load ladder").
SIZE_LADDER = [0, 16777216, 67108864, 201326592, 536870912, 1610612736,
               4294967296]
LOAD_LADDER = [0, 5, 25, 60, 120, 250, 500]
LABEL_MAX = 31            # bytes the games draw
ID_MIN = 2                # 0 and 1 are reserved by the contract
ID_MAX = 2147483647
KEY_MAX = 1024            # longest plugin key Summoner accepts
OUTPUT_CAP = 8 * 1024 * 1024  # max bytes read from one plugin call
ID_MAP_CAP = 100000       # keys remembered; the id counter never goes back

# The words the game (and you) send map to plugin verbs.
VERB_MAP = {"post": "nudge", "delete": "remove"}

PLUGIN_NAME_RE = None   # compiled below, after `import re`
ROOT_ENV = "SUMMONER_ROOT"
DEFAULT_PLUGIN = "ps"
# Built-in plugin configs, used when <root>/plugins/<name>.json is absent.
BUILTINS = {
    "ps": {"plugin": {"name": "ps",
                      "command": [sys.executable,
                                  "${SUMMONER_DIR}/plugins/processes.py"]}},
}

DEFAULTS = {
    "dry_run": True,
    "cache_ttl_s": 2.0,
    "first_poll_wait_ms": 700,
    "max_items": 256,
    "max_tier": 6,
    "rate_limit": {"actions": 10, "per_s": 60},
}
PLUGIN_DEFAULTS = {
    "allow_remove": False,
    "timeout_s": 10.0,
}


class SummonerError(Exception):
    pass


# ---------------------------------------------------------------- config --

def check_private(path, what):
    """Refuse a file or directory that someone else could have written:
    owned by another user, or writable by group or others. A plugin config
    names a command to run, and the state feeds respond its keys."""
    st = os.stat(path)
    if st.st_uid != os.getuid():
        raise SummonerError("%s is not owned by this user: %s" % (what, path))
    if st.st_mode & 0o022:
        raise SummonerError("%s is writable by group or others: %s"
                           % (what, path))


def load_config(path, default_state=None, self_args=None):
    """Read and validate a config file."""
    if not path:
        raise SummonerError("--config is required")
    path = os.path.abspath(path)
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as e:
        if e.errno == errno.ELOOP:
            raise SummonerError("config is a symlink: %s" % path)
        raise
    with os.fdopen(fd, "r", encoding="utf-8") as f:
        # Check the file actually opened, not the path again: nothing can be
        # swapped in between.
        st = os.fstat(f.fileno())
        if not stat.S_ISREG(st.st_mode):
            raise SummonerError("config is not a regular file: %s" % path)
        if st.st_uid != os.getuid():
            raise SummonerError("config is not owned by this user: %s" % path)
        if st.st_mode & 0o022:
            raise SummonerError("config is writable by group or others: %s"
                               % path)
        try:
            raw = json.load(f)
        except ValueError:
            raise SummonerError("config is not valid JSON: %s" % path)
    return config_from_dict(raw, path, default_state,
                            self_args or ["--config", path])


def config_from_dict(raw, path, default_state, self_args):
    """Validate a config. state_dir comes from the config, else from the
    root (named plugins); a bare --config file must name its own."""
    if not isinstance(raw, dict):
        raise SummonerError("config must be a JSON object")
    cfg = dict(DEFAULTS)
    cfg.update(raw)
    rl = dict(DEFAULTS["rate_limit"])
    rl.update(raw.get("rate_limit") or {})
    cfg["rate_limit"] = rl
    plugin = dict(PLUGIN_DEFAULTS)
    plugin.update(raw.get("plugin") or {})
    cfg["plugin"] = plugin
    cfg["config_path"] = path
    cfg["self_args"] = list(self_args)

    state = cfg.get("state_dir")
    if state is None and default_state is not None:
        state = default_state
    if not isinstance(state, str) or not state:
        raise SummonerError("config: state_dir is required (no default)")
    if not os.path.isabs(state):
        raise SummonerError("config: state_dir must be an absolute path")
    state = os.path.normpath(state)
    if state == os.sep:
        raise SummonerError("config: state_dir must not be /")
    cfg["state_dir"] = state

    cmd = plugin.get("command")
    if not isinstance(cmd, list) or not cmd or \
            not all(isinstance(a, str) and a for a in cmd):
        raise SummonerError("config: plugin.command must be a non-empty "
                           "list of strings")
    # ${SUMMONER_DIR} expands to this file's directory, so shipped example
    # configs work from any checkout. Nothing is read from the environment.
    plugin["command"] = [a.replace("${SUMMONER_DIR}", SUMMONER_DIR) for a in cmd]
    if not isinstance(plugin.get("name"), str) or not plugin["name"]:
        raise SummonerError("config: plugin.name is required")
    if not isinstance(cfg["dry_run"], bool):
        raise SummonerError("config: dry_run must be true or false")
    if not isinstance(plugin["allow_remove"], bool):
        raise SummonerError("config: plugin.allow_remove must be true or false")
    tier = cfg["max_tier"]
    if not isinstance(tier, int) or isinstance(tier, bool) or \
            not 0 <= tier <= 6:
        raise SummonerError("config: max_tier must be an integer 0..6")
    return cfg


def make_private_dirs(d):
    """makedirs with every directory it creates at 0700, whatever the umask
    (os.makedirs applies its mode to the last one only)."""
    old = os.umask(0o077)
    try:
        os.makedirs(d, mode=0o700, exist_ok=True)
    finally:
        os.umask(old)


def ensure_state_dir(cfg):
    """Create the state dir 0700, then refuse it unless it is a real
    directory (not a symlink) owned by this user and not writable by group
    or others, so nobody else can plant a cache, an id map or an undo
    record that respond would trust."""
    d = cfg["state_dir"]
    make_private_dirs(d)
    st = os.lstat(d)
    if not stat.S_ISDIR(st.st_mode):
        raise SummonerError("state_dir is not a directory (or is a symlink)")
    if st.st_uid != os.getuid():
        raise SummonerError("state_dir is not owned by this user")
    if st.st_mode & 0o022:
        raise SummonerError("state_dir is writable by group or others")


def state_path(cfg, name):
    return os.path.join(cfg["state_dir"], name)


# ----------------------------------------------------------- file helpers --

class Locked:
    """Exclusive flock on a lock file in the state dir (blocking)."""

    def __init__(self, path, blocking=True):
        self.path = path
        self.blocking = blocking
        self.fd = None
        self.held = False

    def __enter__(self):
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)
        flags = fcntl.LOCK_EX | (0 if self.blocking else fcntl.LOCK_NB)
        try:
            fcntl.flock(self.fd, flags)
            self.held = True
        except OSError as e:
            if e.errno not in (errno.EWOULDBLOCK, errno.EAGAIN):
                raise
        return self

    def __exit__(self, *exc):
        if self.fd is not None:
            os.close(self.fd)   # closing drops the lock
            self.fd = None
        return False


def read_json(path, default=None):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, ValueError):
        return default


def read_cache(cfg):
    c = read_json(state_path(cfg, "cache.json"))
    if not isinstance(c, dict) or not isinstance(c.get("items"), list):
        return None
    return c


def write_json_atomic(path, obj):
    tmp = "%s.tmp.%d" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, sort_keys=True)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def audit(cfg, entry):
    """Append one JSON line to the audit log. JSON escaping keeps any label,
    key or plugin message inert (no raw newline can forge a second entry)."""
    entry = dict(entry)
    entry.setdefault("ts", round(time.time(), 3))
    entry.setdefault("plugin", cfg["plugin"]["name"])
    line = json.dumps(entry, sort_keys=True, ensure_ascii=True) + "\n"
    fd = os.open(state_path(cfg, "audit.jsonl"),
                 os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW,
                 0o600)
    try:
        os.write(fd, line.encode("ascii"))
    finally:
        os.close(fd)


def read_audit(cfg):
    out = []
    try:
        with open(state_path(cfg, "audit.jsonl"), "r", encoding="ascii") as f:
            for line in f:
                try:
                    out.append(json.loads(line))
                except ValueError:
                    continue
    except FileNotFoundError:
        pass
    return out


# --------------------------------------------------------- plugin running --

HOST_PIDS_ENV = "SUMMONER_HOST_PIDS"


def ancestor_pids(pid):
    """pid and its ancestors (above 1), from one `ps` listing. Summoner
    hands these to the plugin so it can leave out the host that called
    Summoner (psDoom, its shell, its launchers) and Summoner itself."""
    try:
        out = subprocess.run(["ps", "-axo", "pid=,ppid="],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return [pid]
    parent = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 2 and f[0].isdigit() and f[1].isdigit():
            parent[int(f[0])] = int(f[1])
    chain, cur = [], pid
    while cur > 1 and cur not in chain and len(chain) < 64:
        chain.append(cur)
        cur = parent.get(cur, 0)
    return chain


def parse_pid_list(text):
    return [int(x) for x in (text or "").split(",")
            if x.isdigit() and 1 < int(x) <= 2147483647][:256]


def run_plugin(cfg, args, stdin_data=None, timeout=None):
    """Run the plugin with an argument vector (never a shell). Returns
    (exit_code, stdout_text). Stdin is /dev/null unless data is given. The
    plugin gets its own process group, and on timeout that group -- which
    this function created and recorded -- is killed."""
    cmd = list(cfg["plugin"]["command"]) + list(args)
    if timeout is None:
        timeout = float(cfg["plugin"]["timeout_s"])
    env = dict(os.environ)
    host = sorted(set(cfg.get("host_pids", [])) | set(ancestor_pids(os.getpid())))
    env[HOST_PIDS_ENV] = ",".join(str(p) for p in host)
    proc = subprocess.Popen(
        cmd,
        env=env,
        stdin=subprocess.PIPE if stdin_data is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        close_fds=True,
        start_new_session=True,
    )
    try:
        out, _ = proc.communicate(
            input=stdin_data.encode("utf-8") if stdin_data is not None
            else None,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.communicate()
        raise SummonerError("plugin %s timed out after %ss" %
                           (args[0] if args else "?", timeout))
    if len(out) > OUTPUT_CAP:
        raise SummonerError("plugin output exceeds %d bytes" % OUTPUT_CAP)
    return proc.returncode, out.decode("utf-8", errors="replace")


def plugin_result(text):
    """A nudge/remove/restore result is the last non-empty stdout line,
    parsed as a JSON object."""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        return {}
    try:
        obj = json.loads(lines[-1])
    except ValueError:
        return {}
    return obj if isinstance(obj, dict) else {}


def validate_describe(d):
    if not isinstance(d, dict):
        raise SummonerError("describe: not a JSON object")
    verbs = d.get("verbs")
    if not isinstance(verbs, list) or "list" not in verbs:
        raise SummonerError("describe: verbs must be a list including 'list'")
    for k in ("size", "load"):
        spec = d.get(k) or {}
        if not isinstance(spec, dict):
            raise SummonerError("describe: %s must be an object" % k)
        scale = spec.get("scale", "relative")
        if scale not in ("native", "linear", "relative"):
            raise SummonerError("describe: %s.scale %r unknown" % (k, scale))
        if scale == "linear" and not _is_num(spec.get("max")):
            raise SummonerError("describe: %s.scale linear needs max" % k)
    d["dimensions"] = validate_dimensions(d.get("dimensions"))
    ds = d.get("default_sort")
    sortable = {x["id"] for x in d["dimensions"] if "sort" in x["roles"]}
    if ds is not None and ds not in sortable:
        raise SummonerError("describe: default_sort %r is not a sort "
                            "dimension" % (ds,))
    return d


DIM_ID_RE = re.compile(r"[a-z][a-z0-9_]{0,15}\Z")
DIM_KINDS = ("numeric", "category")
DIRS = ("asc", "desc")
LIMIT_MAX = 4096


def validate_dimensions(dims):
    """The dimensions a plugin can sort or group by. Each: id (short, safe
    to pass on a command line), label (for a game menu), kind numeric or
    category, dir (numeric default direction), unit, roles (sort, group)."""
    if dims is None:
        return []
    if not isinstance(dims, list):
        raise SummonerError("describe: dimensions must be a list")
    out, seen = [], set()
    for d in dims:
        if not isinstance(d, dict):
            raise SummonerError("describe: a dimension is not an object")
        did = d.get("id")
        if not isinstance(did, str) or not DIM_ID_RE.match(did) or \
                did in seen:
            raise SummonerError("describe: bad or repeated dimension id %r"
                                % (did,))
        kind = d.get("kind")
        if kind not in DIM_KINDS:
            raise SummonerError("describe: dimension %s: kind must be "
                                "numeric or category" % did)
        default_roles = ["sort"] if kind == "numeric" else ["group"]
        roles = d.get("roles", default_roles)
        if not isinstance(roles, list) or not roles or \
                not set(roles) <= {"sort", "group"}:
            raise SummonerError("describe: dimension %s: roles must be "
                                "sort and/or group" % did)
        ddir = d.get("dir", "desc")
        if ddir not in DIRS:
            raise SummonerError("describe: dimension %s: dir must be asc "
                                "or desc" % did)
        seen.add(did)
        out.append({"id": did,
                    "label": sanitize_label(d.get("label", did)) or did,
                    "kind": kind, "dir": ddir,
                    "unit": sanitize_label(d.get("unit", ""), 16),
                    "roles": sorted(set(roles))})
    return out


def canonical_request(words):
    """The request words as given (sort=, group=, limit=), syntactically
    cleaned and put in a fixed order, so the cache can be keyed by them.
    Not yet checked against any plugin. Unknown words are returned apart."""
    req = {"sort": [], "group": None, "limit": None}
    unknown = []
    for w in words:
        k, sep, v = w.partition("=")
        if not sep:
            unknown.append(w)
        elif k == "sort":
            req["sort"] = []
            for part in v.split(","):
                did, _, ddir = part.partition(":")
                if DIM_ID_RE.match(did) and ddir in ("", "asc", "desc"):
                    req["sort"].append([did, ddir or None])
                else:
                    unknown.append("sort=" + part)
        elif k == "group":
            if DIM_ID_RE.match(v):
                req["group"] = v
            else:
                unknown.append(w)
        elif k == "limit":
            if v.isdigit() and 0 < int(v) <= LIMIT_MAX:
                req["limit"] = int(v)
            else:
                unknown.append(w)
        else:
            unknown.append(w)
    return req, unknown


def request_key(req):
    return json.dumps(req, sort_keys=True)


def apply_request(req, desc, unknown=()):
    """Check a canonical request against the plugin's dimensions. Returns
    (applied request, notes, plugin words). An unknown or wrong-kind id is
    dropped with a note, and the plugin's default sort is used in its
    place; the poll itself never fails because of a request."""
    dims = {d["id"]: d for d in desc.get("dimensions", [])}
    notes = ["ignored %s (not understood)" % sanitize_label(u, 40)
             for u in unknown]
    sort = []
    for did, ddir in req["sort"]:
        d = dims.get(did)
        if d is None or "sort" not in d["roles"]:
            notes.append("ignored sort=%s (not a sort dimension)" % did)
            continue
        if any(did == x[0] for x in sort):
            continue
        sort.append([did, ddir or d["dir"]])
    if not sort and desc.get("default_sort"):
        d = dims[desc["default_sort"]]
        sort = [[d["id"], d["dir"]]]
        if req["sort"]:
            notes.append("using sort=%s" % d["id"])
    group = req["group"]
    if group is not None and (group not in dims or
                              "group" not in dims[group]["roles"]):
        notes.append("ignored group=%s (not a group dimension)" % group)
        group = None
    applied = {"sort": sort, "group": group, "limit": req["limit"]}
    words = []
    if sort:
        words.append("sort=" + ",".join("%s:%s" % (a, b) for a, b in sort))
    if group:
        words.append("group=" + group)
    if req["limit"]:
        words.append("limit=%d" % req["limit"])
    return applied, notes, words


def describe(cfg):
    code, out = run_plugin(cfg, ["describe"])
    if code != 0:
        raise SummonerError("plugin describe exited %d" % code)
    try:
        return validate_describe(json.loads(out))
    except ValueError:
        raise SummonerError("describe: output is not JSON")


# ---------------------------------------------------------- normalisation --

def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and \
        v == v and v not in (float("inf"), float("-inf"))


def sanitize_label(name, limit=LABEL_MAX):
    """Plugin name -> what the games draw: printable ASCII, whitespace
    (TAB and newline included) collapsed to single spaces, at most `limit`
    bytes. A TAB or newline can never survive, so a name can never add a
    field or a record to the poll output."""
    if name is None:
        return ""
    s = unicodedata.normalize("NFKD", str(name))
    out = []
    for ch in s:
        o = ord(ch)
        if 0x20 <= o < 0x7F:
            out.append(ch)
        elif unicodedata.combining(ch):
            continue
        else:
            out.append(" ")
    s = " ".join("".join(out).split())
    return s[:limit].rstrip()


def tier_value(fraction, ladder, max_tier):
    f = min(1.0, max(0.0, fraction))
    return ladder[int(round(f * max_tier))]


def scale_metric(values, spec, ladder, max_tier):
    """values: list of float-or-None per item. Returns ints on the ladder.
    native  -> pass through (clamped to [0, ladder[max_tier]])
    linear  -> (v - min) / (max - min) of the declared range, onto tiers
    relative-> min..max of the values in this listing, onto tiers"""
    spec = spec or {}
    scale = spec.get("scale", "relative")
    top = ladder[max_tier]
    if scale == "native":
        return [0 if v is None else int(min(max(v, 0), top)) for v in values]
    if scale == "linear":
        lo = float(spec.get("min", 0))
        hi = float(spec["max"])
    else:
        present = [v for v in values if v is not None]
        lo = min(present) if present else 0.0
        hi = max(present) if present else 0.0
    span = hi - lo
    out = []
    for v in values:
        if v is None or span <= 0:
            out.append(ladder[0])
        else:
            out.append(tier_value((v - lo) / span, ladder, max_tier))
    return out


def scale_fraction(values, spec):
    """values -> 0..1 per item with the declared scale (None where there is
    no value, and for a native scale, which has no range of its own)."""
    spec = spec or {}
    scale = spec.get("scale", "relative")
    if scale == "native":
        return [None] * len(values)
    if scale == "linear":
        lo = float(spec.get("min", 0))
        hi = float(spec["max"])
    else:
        present = [v for v in values if v is not None]
        lo = min(present) if present else 0.0
        hi = max(present) if present else 0.0
    span = hi - lo
    out = []
    for v in values:
        if v is None:
            out.append(None)
        elif span <= 0:
            out.append(0.0)
        else:
            out.append(min(1.0, max(0.0, (v - lo) / span)))
    return out


def parse_list(text):
    """Plugin `list` output: JSON Lines, one object per item. Bad lines and
    items without a usable key are skipped; the first copy of a key wins."""
    items = []
    seen = set()
    skipped = 0
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            skipped += 1
            continue
        if not isinstance(obj, dict):
            skipped += 1
            continue
        key = obj.get("key")
        if not isinstance(key, str) or not key or len(key) > KEY_MAX or \
                "\x00" in key or key.startswith("-") or key in seen:
            skipped += 1
            continue
        seen.add(key)
        items.append(obj)
    return items, skipped


def normalize(items, desc, cfg, primary=None):
    """Truncate and scale plugin items, keeping the plugin's order (the
    plugin curates: it sorts, trims and grades). `primary` is the applied
    primary sort dimension id, used for a grade the plugin did not send.
    Returns dicts without ids."""
    max_tier = cfg["max_tier"]
    ordered = list(items)[:max(0, int(cfg["max_items"]))]
    n = len(ordered)
    primary_dir = "desc"
    for d in desc.get("dimensions", []):
        if d["id"] == primary:
            primary_dir = d["dir"]
    sort_dir = (cfg.get("_applied_dirs") or {}).get(primary, primary_dir)

    sizes = []
    for pos, it in enumerate(ordered):
        v = it.get("size")
        sizes.append(float(v) if _is_num(v) and v >= 0 else None)
    size_spec = desc.get("size") or {}
    has_size = [v is not None for v in sizes]
    rank_based = not any(has_size)
    if rank_based:
        # No sizes at all: the top-ranked item is the biggest.
        size_spec = {"scale": "relative"}
        sizes = [float(n - pos) for pos in range(n)]
    loads = []
    for it in ordered:
        v = it.get("load")
        loads.append(float(v) if _is_num(v) and v >= 0 else None)

    load_spec = desc.get("load") or {}
    weights = scale_metric(sizes, size_spec, SIZE_LADDER, max_tier)
    load_vals = scale_metric(loads, load_spec, LOAD_LADDER, 6)
    # The grade, 0..1 over the item's own range: the plugin's own `grade` if
    # it sent one; else its size on the declared scale; else its load; else
    # its rank. A native size scale is already in the game's units, so those
    # items get no computed grade and the game's Classify by applies, as
    # with the native backend. Below 6, max_tier scales every grade by
    # (max_tier + 0.5) / 7: exact on the full ladder, and never above the
    # capped monster on a WAD with fewer drawable tiers (it may land lower).
    none = [None] * n
    if (desc.get("size") or {}).get("scale") == "native":
        computed = none
    else:
        size_g = none if rank_based else scale_fraction(sizes, size_spec)
        load_g = none if load_spec.get("scale") == "native" \
            else scale_fraction(loads, load_spec)
        rank_g = scale_fraction(sizes, size_spec) if rank_based else none
        computed = [a if a is not None else (b if b is not None else c)
                    for a, b, c in zip(size_g, load_g, rank_g)]
    # A grade from the primary sort dimension's values, when the plugin
    # reports them: the "best" end of the requested direction is 1.
    pv = []
    for it in ordered:
        vals = it.get("values")
        v = vals.get(primary) if isinstance(vals, dict) and primary else None
        pv.append(float(v) if _is_num(v) else None)
    primary_g = scale_fraction(pv, {"scale": "relative"})
    if sort_dir == "asc":
        primary_g = [None if g is None else 1.0 - g for g in primary_g]
    cap = 1.0 if max_tier >= 6 else (max_tier + 0.5) / 7.0
    grades = []
    for i, it in enumerate(ordered):
        g = it.get("grade")
        if _is_num(g):
            g = min(1.0, max(0.0, float(g)))
            if not has_size[i]:
                weights[i] = tier_value(g, SIZE_LADDER, max_tier)
            if loads[i] is None:
                load_vals[i] = tier_value(g, LOAD_LADDER, 6)
        elif primary_g[i] is not None:
            g = primary_g[i]
        else:
            g = computed[i]
        grades.append(None if g is None else g * cap)

    out = []
    for i, it in enumerate(ordered):
        parent = it.get("parent")
        out.append({
            "key": it["key"],
            "name": str(it.get("name", ""))[:256],
            "label": sanitize_label(it.get("name", it["key"])),
            "weight": weights[i],
            "load": load_vals[i],
            "flags": 1 if it.get("background") is True else 0,
            "group": sanitize_label(it["group"])
            if isinstance(it.get("group"), str) else "",
            "grade": None if grades[i] is None else round(grades[i], 4),
            "parent_key": parent if isinstance(parent, str) else None,
        })
    return out


# ----------------------------------------------------------------- id map --

class _NoLock:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _id_map_is_fresh(cfg, path):
    """A missing id map is only new if nothing was ever listed with it: no
    cache in this state dir, and for a root-wide map, none in any plugin's
    state dir under the root."""
    if os.path.exists(path):
        return False
    if os.path.exists(state_path(cfg, "cache.json")):
        return False
    root = cfg.get("root")
    if root:
        state_root = os.path.join(root, "state")
        try:
            for name in os.listdir(state_root):
                if os.path.exists(os.path.join(state_root, name,
                                               "cache.json")):
                    return False
        except FileNotFoundError:
            pass
    return True


def assign_ids(cfg, items, now):
    """Give every item a stable id from the persisted map. Ids are handed out
    from a counter that never goes back, so an id is never reused for a
    different key, even after the key is forgotten. Call under state.lock.

    Named plugins share one map for the whole root (<root>/ids.json, keys
    namespaced "<plugin>/<key>"), so ids are unique across plugins: after
    `use` switches plugin, an id the game still holds is simply not listed
    any more, and the game sends nothing for it. A bare --config file keeps
    its own map in its state dir."""
    root = cfg.get("root")
    path = os.path.join(root, "ids.json") if root \
        else state_path(cfg, "ids.json")
    # The namespace is the validated plugin file name (no "/"), never the
    # free-text plugin.name inside the config, which two configs may share.
    ns = (cfg["plugin_id"] + "/") if root else ""
    with Locked(path + ".lock") if root else _NoLock():
        m = read_json(path, None)
        if m is None and _id_map_is_fresh(cfg, path):
            m = {"version": 1, "next": ID_MIN, "keys": {}}
            # A counter from an earlier layout is never gone back below.
            old = read_json(os.path.join(root, "ids-next.json"), {}) \
                if root else {}
            nxt = old.get("next") if isinstance(old, dict) else None
            if isinstance(nxt, int) and not isinstance(nxt, bool) and \
                    ID_MIN < nxt <= ID_MAX + 1:
                m["next"] = nxt
        if not isinstance(m, dict) or m.get("version") != 1 or \
                not isinstance(m.get("next"), int) or \
                not isinstance(m.get("keys"), dict):
            # Fail closed: restarting the counter could hand an id the game
            # still holds to a different item.
            raise SummonerError(
                "%s is missing or damaged; ids could be reused. Delete %s "
                "to start over." % (path, "the root's state/ and ids.json"
                                    if root else "the whole state_dir"))
        keys = m["keys"]
        for it in items:
            k = ns + it["key"]
            rec = keys.get(k)
            if rec is None:
                if m["next"] > ID_MAX:
                    raise SummonerError("id space exhausted")
                rec = {"id": m["next"]}
                m["next"] += 1
                keys[k] = rec
            rec["seen"] = now
            it["id"] = rec["id"]
        _finish_id_map(m, keys, path)
    _link_parents(items)


def _finish_id_map(m, keys, path):
    if len(keys) > ID_MAP_CAP:
        drop = sorted(keys, key=lambda k: keys[k].get("seen", 0))
        for k in drop[:len(keys) - ID_MAP_CAP]:
            del keys[k]
    write_json_atomic(path, m)


def _link_parents(items):
    by_key = {it["key"]: it["id"] for it in items}
    for it in items:
        it["parent"] = by_key.get(it.pop("parent_key"), 0)
        if it["parent"] == it["id"]:
            it["parent"] = 0


# ------------------------------------------------------------------ cache --

def refresh(cfg, words=()):
    """Run describe + list (with the host's request, checked against the
    plugin's dimensions) and rewrite the cache. Single-flight: if another
    refresher holds refresh.lock this returns False without running."""
    words = list(words)
    host = [w for w in words if w.startswith("host_pids=")]
    words = [w for w in words if not w.startswith("host_pids=")]
    if host:
        cfg = dict(cfg, host_pids=parse_pid_list(host[-1][10:]))
    req, unknown = canonical_request(words)
    ensure_state_dir(cfg)
    with Locked(state_path(cfg, "refresh.lock"), blocking=False) as lk:
        if not lk.held:
            return False
        started = time.time()
        with open(state_path(cfg, "refresh.pid"), "w") as f:
            f.write("%d\n" % os.getpid())
        desc = describe(cfg)
        applied, notes, pwords = apply_request(req, desc, unknown)
        # A plugin that declares no dimensions gets the plain `list`.
        code, out = run_plugin(cfg, ["list"] + (pwords if desc["dimensions"]
                                                else []))
        if code != 0:
            raise SummonerError("plugin list exited %d" % code)
        raw, skipped = parse_list(out)
        primary = applied["sort"][0][0] if applied["sort"] else None
        cfg = dict(cfg, _applied_dirs={a: b for a, b in applied["sort"]})
        items = normalize(raw, desc, cfg, primary)
        if applied["limit"]:
            items = items[:applied["limit"]]
        now = round(time.time(), 3)
        with Locked(state_path(cfg, "state.lock")):
            assign_ids(cfg, items, now)
            write_json_atomic(state_path(cfg, "cache.json"), {
                "version": 1,
                "fetched_at": now,
                "describe": desc,
                "request": request_key(req),
                "applied": applied,
                "notes": notes,
                "items": items,
                "skipped": skipped,
            })
            try:
                # An action that landed while list ran keeps the cache dirty,
                # so the next poll refreshes again.
                if os.stat(state_path(cfg, "dirty")).st_mtime < started:
                    os.unlink(state_path(cfg, "dirty"))
            except FileNotFoundError:
                pass
    return True


def refresher_running(cfg):
    with Locked(state_path(cfg, "refresh.lock"), blocking=False) as lk:
        return not lk.held


def spawn_refresher(cfg, words=()):
    """Start a detached refresher in its own session, so psDoom's poll
    timeout (which kills the poll's process group) cannot kill it, and with
    no handle on the poll's stdout, so the poll ends at once."""
    log = os.open(state_path(cfg, "refresh.log"),
                  os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        subprocess.Popen(
            [sys.executable, os.path.abspath(__file__)] +
            cfg["self_args"] + ["refresh"] + list(words),
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log,
            close_fds=True, start_new_session=True)
    finally:
        os.close(log)


def render(items, cache=None):
    """Header comment lines, then one TAB-separated key=value line per item.

    Header (each a `#` comment, so any reader may skip it):
      # dim id=<id> kind=<numeric|category> dir=<asc|desc> roles=<r,..> unit=<u> label=<text>
      # sort id=<id> dir=<asc|desc>        the applied primary sort, if any
      # group id=<id>                      the applied grouping, if any
      # summon: <note>                     a request word that was ignored
    Items carry `grade` (0..1) and `group` (the item's group label) where
    they have one. The label stays last so it never hides a field from a
    reader that splits on TAB."""
    lines = []
    if cache:
        for d in (cache.get("describe") or {}).get("dimensions", []):
            lines.append("# dim id=%s kind=%s dir=%s roles=%s unit=%s "
                         "label=%s" % (d["id"], d["kind"], d["dir"],
                                       ",".join(d["roles"]), d["unit"],
                                       d["label"]))
        applied = cache.get("applied") or {}
        if applied.get("sort"):
            lines.append("# sort id=%s dir=%s" % tuple(applied["sort"][0]))
        if applied.get("group"):
            lines.append("# group id=%s" % applied["group"])
        for note in cache.get("notes") or []:
            lines.append("# summon: %s" % sanitize_label(note, 120))
    for it in items:
        g = it.get("grade")
        grade = "" if g is None else "\tgrade=%.4f" % g
        group = "\tgroup=%s" % it["group"] if it.get("group") else ""
        lines.append("id=%d\tparent=%d\tweight=%d\tload=%d\tflags=%d%s%s"
                     "\tlabel=%s"
                     % (it["id"], it["parent"], it["weight"], it["load"],
                        it["flags"], grade, group, it["label"]))
    return "".join(line + "\n" for line in lines)


def cmd_poll(cfg, words=()):
    ensure_state_dir(cfg)
    # host_pids is Summoner's own word between get and its refresher.
    words = [w for w in words if not w.startswith("host_pids=")]
    req, _ = canonical_request(words)
    key = request_key(req)
    cache = read_cache(cfg)
    now = time.time()
    dirty = os.path.exists(state_path(cfg, "dirty"))
    # A different request makes the cache stale at once: the order and the
    # grades it holds answer another question.
    changed = cache is not None and cache.get("request") != key
    stale = (cache is None or dirty or changed or
             now - cache.get("fetched_at", 0) >= cfg["cache_ttl_s"])
    try:
        last_try = os.stat(state_path(cfg, "refresh.pid")).st_mtime
    except FileNotFoundError:
        last_try = 0
    # A failing plugin is retried once per cache_ttl_s, not on every poll.
    backoff = not dirty and not changed and \
        now - last_try < cfg["cache_ttl_s"]
    if stale and not backoff and not refresher_running(cfg):
        # The refresher runs detached, so it cannot see who called get:
        # hand it this get's ancestors (the shell, the host, its launchers).
        chain = ancestor_pids(os.getpid())
        spawn_refresher(cfg, list(words) +
                        ["host_pids=" + ",".join(str(p) for p in chain)])
    if cache is None or changed:
        deadline = time.monotonic() + cfg["first_poll_wait_ms"] / 1000.0
        while time.monotonic() < deadline:
            fresh = read_cache(cfg)
            if fresh is not None and fresh.get("request") == key:
                cache = fresh
                break
            time.sleep(0.02)
    sys.stdout.write(render(cache["items"] if cache else [], cache))
    return 0


# ---------------------------------------------------------------- respond --

def rate_allow(cfg, now):
    """Sliding-window limit on executed actions. Call under state.lock."""
    rl = cfg["rate_limit"]
    limit, window = int(rl["actions"]), float(rl["per_s"])
    path = state_path(cfg, "rate.json")
    stamps = read_json(path, [])
    stamps = [t for t in (stamps if isinstance(stamps, list) else [])
              if _is_num(t) and now - t < window]
    if len(stamps) >= limit:
        write_json_atomic(path, stamps)
        return False
    stamps.append(now)
    write_json_atomic(path, stamps)
    return True


def parse_extras(extras):
    out = {}
    for kv in extras:
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


ID_RE = re.compile(r"[0-9]{1,10}\Z")


def cmd_respond(cfg, argv):
    """Never raises; psDoom does not read the exit code, the audit log is the
    record of what happened."""
    ensure_state_dir(cfg)
    try:
        return _respond(cfg, argv)
    except Exception as e:
        audit(cfg, {"verb": (argv[0] if argv else "")[:32],
                    "id": (argv[1] if len(argv) > 1 else "")[:16],
                    "outcome": "error", "detail": str(e)[:200]})
        return 0


HOST_MODES = ("dryrun", "post-only", "delete")


def _respond(cfg, argv):
    verb = argv[0] if argv else ""
    ident = argv[1] if len(argv) > 1 else ""
    extras = parse_extras(argv[2:])
    # The host's effect mode. Missing or unknown counts as dryrun: an
    # event never acts unless the host said it may.
    mode = extras.get("mode")
    if mode not in HOST_MODES:
        mode = "dryrun"
    entry = {"verb": verb[:32], "id": ident[:16],
             "game_label": extras.get("label", "")[:64], "mode": mode}
    pverb = VERB_MAP.get(verb)
    if pverb is None:
        audit(cfg, dict(entry, outcome="ignored-verb"))
        return 0
    cache = read_cache(cfg) or {}
    item = None
    if ID_RE.match(ident):
        for it in cache.get("items", []):
            if it["id"] == int(ident):
                item = it
                break
    if item is None:
        audit(cfg, dict(entry, outcome="unknown-id"))
        return 0
    entry.update(action=pverb, key=item["key"], name=item["name"])
    game_label = extras.get("label")
    if game_label is not None and game_label != item["label"]:
        # The game names the monster it hit; if this id now lists something
        # else (another plugin, a stale snapshot), act on nothing.
        audit(cfg, dict(entry, outcome="label-mismatch"))
        return 0
    key = item["key"]
    if not isinstance(key, str) or not key or key.startswith("-") or \
            "\x00" in key or len(key) > KEY_MAX:
        audit(cfg, dict(entry, outcome="bad-key"))
        return 0
    desc = cache.get("describe") or {}
    if pverb not in (desc.get("verbs") or []):
        audit(cfg, dict(entry, outcome="unsupported"))
        return 0
    args = [pverb, item["key"]]
    if pverb == "nudge":
        try:
            amount = int(extras.get("amount", "1"))
        except ValueError:
            amount = 1
        amount = min(max(amount, 1), 100)
        args.append("amount=%d" % amount)
        entry["amount"] = amount
    # The effective action is the stricter of the host's mode and this
    # plugin's own config (dry_run, allow_remove).
    if mode == "dryrun":
        audit(cfg, dict(entry, outcome="dry-run", reason="host mode"))
        return 0
    if cfg["dry_run"]:
        audit(cfg, dict(entry, outcome="dry-run", reason="plugin config"))
        return 0
    if pverb == "remove" and mode != "delete":
        audit(cfg, dict(entry, outcome="blocked", reason="host mode"))
        return 0
    if pverb == "remove" and not cfg["plugin"]["allow_remove"]:
        audit(cfg, dict(entry, outcome="blocked", reason="plugin config"))
        return 0
    with Locked(state_path(cfg, "state.lock")):
        allowed = rate_allow(cfg, time.time())
    if not allowed:
        audit(cfg, dict(entry, outcome="rate-limited"))
        return 0
    action_id = secrets.token_hex(6)
    entry["action_id"] = action_id
    try:
        code, out = run_plugin(cfg, args)
    except (SummonerError, OSError) as e:
        audit(cfg, dict(entry, outcome="error", detail=str(e)[:200]))
        return 0
    res = plugin_result(out)
    ok = code == 0 and res.get("ok") is True
    undo = res.get("undo") if ok and isinstance(res.get("undo"), dict) \
        else None
    audit(cfg, dict(entry, outcome="ok" if ok else "error",
                    exit=code, detail=str(res.get("message", ""))[:200],
                    undo=undo))
    if ok:
        with Locked(state_path(cfg, "state.lock")):
            if pverb == "remove":
                # Drop it now so the next poll already shows it gone.
                c = read_cache(cfg)
                if c:
                    c["items"] = [i for i in c["items"]
                                  if i["key"] != item["key"]]
                    write_json_atomic(state_path(cfg, "cache.json"), c)
            with open(state_path(cfg, "dirty"), "w"):
                pass
    return 0


def cmd_undo(cfg, action_id):
    ensure_state_dir(cfg)
    desc = (read_cache(cfg) or {}).get("describe") or {}
    if "restore" not in (desc.get("verbs") or []):
        raise SummonerError("the plugin does not list restore")
    # One undo at a time, so the same action cannot be restored twice.
    with Locked(state_path(cfg, "undo.lock")):
        return _undo(cfg, action_id)


def _undo(cfg, action_id):
    log = read_audit(cfg)
    target = None
    for e in log:
        if e.get("action_id") == action_id and e.get("outcome") == "ok":
            target = e
    if target is None:
        raise SummonerError("no successful action %r in the audit log"
                           % action_id)
    if any(e.get("verb") == "undo" and e.get("undoes") == action_id and
           e.get("outcome") == "ok" for e in log):
        raise SummonerError("action %r was already undone" % action_id)
    if not isinstance(target.get("undo"), dict):
        raise SummonerError("action %r has no undo record (the plugin "
                           "reported it as not undoable)" % action_id)
    code, out = run_plugin(cfg, ["restore"],
                           stdin_data=json.dumps(target["undo"]))
    res = plugin_result(out)
    ok = code == 0 and res.get("ok") is True
    audit(cfg, {"verb": "undo", "undoes": action_id, "key": target.get("key"),
                "outcome": "ok" if ok else "error", "exit": code,
                "detail": str(res.get("message", ""))[:200]})
    with open(state_path(cfg, "dirty"), "w"):
        pass
    if not ok:
        raise SummonerError("plugin restore failed: %s"
                           % res.get("message", "exit %d" % code))
    print("undone %s" % action_id)
    return 0


# ------------------------------------------------------------------- main --

PLUGIN_NAME_RE = re.compile(r"[a-z0-9][a-z0-9_-]{0,31}\Z")

USAGE = ("usage: summon [--root DIR] [--plugin NAME | --config FILE] "
         "(get [sort=ID[:dir],..] [group=ID] [limit=N] | post ID [k=v...] "
         "| delete ID [k=v...] | use NAME | plugins "
         "| refresh | describe | undo ACTION_ID)")


def resolve_root(opt_root):
    """--root, else $SUMMONER_ROOT, else ~/.config/summoner."""
    root = opt_root or os.environ.get(ROOT_ENV) or \
        os.path.join(os.path.expanduser("~"), ".config", "summoner")
    if not os.path.isabs(root):
        raise SummonerError("root must be an absolute path: %s" % root)
    root = os.path.normpath(root)
    if root == os.sep:
        raise SummonerError("root must not be /")
    return root


def check_root(root):
    if os.path.exists(root):
        check_private(os.path.realpath(root), "root")


def plugin_config_path(root, name):
    return os.path.join(root, "plugins", name + ".json")


def check_name(name):
    if not isinstance(name, str) or not PLUGIN_NAME_RE.match(name):
        raise SummonerError("bad plugin name %r (lowercase letters, digits, "
                           "- and _; up to 32)" % (name,))
    return name


def known_plugins(root):
    names = set(BUILTINS)
    d = os.path.join(root, "plugins")
    try:
        for f in os.listdir(d):
            if f.endswith(".json") and PLUGIN_NAME_RE.match(f[:-5]):
                names.add(f[:-5])
    except FileNotFoundError:
        pass
    return sorted(names)


def current_plugin(root):
    try:
        with open(os.path.join(root, "current"), "r", encoding="ascii") as f:
            name = f.read().strip()
    except (FileNotFoundError, UnicodeDecodeError):
        return DEFAULT_PLUGIN
    return name or DEFAULT_PLUGIN


def load_named(root, name):
    """A named plugin: <root>/plugins/<name>.json, else a built-in."""
    check_name(name)
    check_root(root)
    plugins_dir = os.path.join(root, "plugins")
    if os.path.exists(plugins_dir):
        check_private(plugins_dir, "plugins directory")
    path = plugin_config_path(root, name)
    default_state = os.path.join(root, "state", name)
    self_args = ["--root", root, "--plugin", name]
    cfg = None
    if os.path.lexists(path):
        cfg = load_config(path, default_state, self_args)
        if cfg.get("state_dir") != os.path.normpath(default_state):
            # Every named plugin keeps its state under <root>/state/, so the
            # root's fail-closed id-map check can see all of it.
            raise SummonerError("%s: a named plugin's state is always %s; "
                               "remove state_dir from it (or use --config)"
                               % (path, default_state))
    elif name in BUILTINS:
        cfg = config_from_dict(json.loads(json.dumps(BUILTINS[name])),
                               path, default_state, self_args)
    if cfg is not None:
        # One id map for the whole root (see assign_ids).
        cfg["root"] = root
        cfg["plugin_id"] = name
        return cfg
    raise SummonerError("unknown plugin %r: no %s, and it is not built in "
                       "(known: %s)" % (name, path,
                                        ", ".join(known_plugins(root))))


def parse_global(argv):
    """Leading options, then the command and its arguments."""
    opts = {"root": None, "plugin": None, "config": None}
    i = 0
    while i < len(argv) and argv[i] in ("--root", "--plugin", "--config"):
        if i + 1 >= len(argv):
            raise SummonerError("%s needs a value" % argv[i])
        opts[argv[i][2:]] = argv[i + 1]
        i += 2
    if opts["plugin"] and opts["config"]:
        raise SummonerError("give --plugin or --config, not both")
    return opts, argv[i:]


def select_config(opts):
    """--config FILE > --plugin NAME > the current plugin > ps."""
    if opts["config"]:
        return load_config(opts["config"])
    root = resolve_root(opts["root"])
    return load_named(root, opts["plugin"] or current_plugin(root))


def cmd_use(opts, rest):
    if len(rest) != 1:
        raise SummonerError("usage: summon use NAME")
    if opts["config"] or opts["plugin"]:
        raise SummonerError("use takes no --plugin or --config")
    root = resolve_root(opts["root"])
    load_named(root, rest[0])          # refuse a name that cannot load
    make_private_dirs(root)
    check_root(root)
    path = os.path.join(root, "current")
    tmp = "%s.tmp.%d" % (path, os.getpid())
    with open(tmp, "w", encoding="ascii") as f:
        f.write(rest[0] + "\n")
    os.replace(tmp, path)
    print("current plugin: %s" % rest[0])
    return 0


def cmd_plugins(opts):
    root = resolve_root(opts["root"])
    check_root(root)
    cur = opts["plugin"] or current_plugin(root)
    for name in known_plugins(root):
        where = "config" if os.path.exists(plugin_config_path(root, name)) \
            else "built-in"
        print("%s %-16s %s" % ("*" if name == cur else " ", name, where))
    if cur not in known_plugins(root):
        print("* %-16s missing (no config)" % cur)
    return 0


def poll_error(e):
    sys.stdout.write("# summon error: %s\n" % sanitize_label(e, 200))
    return 0


def main(argv):
    try:
        opts, rest = parse_global(argv)
    except SummonerError as e:
        sys.stderr.write("summon: %s\n%s\n" % (e, USAGE))
        return 2
    if not rest:
        sys.stderr.write(USAGE + "\n")
        return 2
    cmd, rest = rest[0], rest[1:]
    if cmd == "get":
        try:
            return cmd_poll(select_config(opts), rest)
        except Exception as e:   # never break the game's poll
            return poll_error(e)
    if cmd in VERB_MAP:
        # post/delete never fail loudly: the game does not read the exit
        # code; the audit log is the record.
        try:
            return cmd_respond(select_config(opts), [cmd] + rest)
        except Exception as e:
            sys.stderr.write("summon: %s\n" % e)
            return 0
    try:
        if cmd == "use":
            return cmd_use(opts, rest)
        if cmd == "plugins":
            return cmd_plugins(opts)
        if cmd in ("refresh", "describe", "undo"):
            cfg = select_config(opts)
            if cmd == "refresh":
                refresh(cfg, rest)
                return 0
            if cmd == "describe":
                print(json.dumps(describe(cfg), indent=2, sort_keys=True))
                return 0
            if len(rest) == 1:
                return cmd_undo(cfg, rest[0])
            raise SummonerError("usage: summon undo ACTION_ID")
    except (SummonerError, OSError, ValueError) as e:
        sys.stderr.write("summon: %s\n" % e)
        return 1
    # Any other word: a newer game's verb, or a typo. A no-op that exits 0,
    # as the script contract asks of responders; audited as ignored-verb when
    # a plugin config loads.
    sys.stderr.write("summon: unknown command %r (ignored)\n%s\n"
                     % (cmd, USAGE))
    try:
        cmd_respond(select_config(opts), [cmd] + rest)
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
