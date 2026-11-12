#!/usr/bin/env python3
"""psDoom plugin harness -- one poll/respond pair for any list of things.

The harness sits between psDoom's script backend (docs/script-backend.md, the
v1 contract, unchanged) and a *plugin*: a small executable that can list
items, nudge one, and remove one. psDoom runs:

    PSDOOM_POLL_CMD    = python3 psdoom_harness.py --config CFG poll
    PSDOOM_RESPOND_CMD = python3 psdoom_harness.py --config CFG respond

and the harness adds what every plugin would otherwise rewrite: stable
integer ids, size/load scaling onto the games' ladders, 31-byte labels,
background caching under the 1-second poll timeout, and a safety layer
(dry run by default, per-plugin remove opt-in, rate limit, audit log, undo).

Design: docs/plugin-harness.md. Python 3.10+ standard library only.

Commands (all need --config, there is no default state location):

    poll                       print the v1 poll output from the cache
    respond <verb> <id> [k=v]  map wound->nudge, kill->remove; unknown -> no-op
    refresh                    run the plugin's list now and rewrite the cache
    undo <action-id>           replay a recorded undo through the plugin
    describe                   print the plugin's describe output, validated
"""

import errno
import fcntl
import json
import os
import secrets
import signal
import subprocess
import sys
import time
import unicodedata

PROTOCOL = 1
HARNESS_DIR = os.path.dirname(os.path.abspath(__file__))

# The games' ladders (docs/script-backend.md, "Size ladder" / "Load ladder").
SIZE_LADDER = [0, 16777216, 67108864, 201326592, 536870912, 1610612736,
               4294967296]
LOAD_LADDER = [0, 5, 25, 60, 120, 250, 500]
LABEL_MAX = 31            # bytes the games draw
ID_MIN = 2                # 0 and 1 are reserved by the contract
ID_MAX = 2147483647
KEY_MAX = 1024            # longest plugin key the harness accepts
OUTPUT_CAP = 8 * 1024 * 1024  # max bytes read from one plugin call
ID_MAP_CAP = 100000       # keys remembered; the id counter never goes back

VERB_MAP = {"wound": "nudge", "kill": "remove"}

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


class HarnessError(Exception):
    pass


# ---------------------------------------------------------------- config --

def load_config(path):
    """Read and validate a config file. The state root must be explicit."""
    if not path:
        raise HarnessError("--config is required")
    path = os.path.abspath(path)
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    if not isinstance(raw, dict):
        raise HarnessError("config must be a JSON object")
    cfg = dict(DEFAULTS)
    cfg.update(raw)
    rl = dict(DEFAULTS["rate_limit"])
    rl.update(raw.get("rate_limit") or {})
    cfg["rate_limit"] = rl
    plugin = dict(PLUGIN_DEFAULTS)
    plugin.update(raw.get("plugin") or {})
    cfg["plugin"] = plugin
    cfg["config_path"] = path

    state = cfg.get("state_dir")
    if not isinstance(state, str) or not state:
        raise HarnessError("config: state_dir is required (no default)")
    if not os.path.isabs(state):
        raise HarnessError("config: state_dir must be an absolute path")
    state = os.path.normpath(state)
    if state == os.sep:
        raise HarnessError("config: state_dir must not be /")
    cfg["state_dir"] = state

    cmd = plugin.get("command")
    if not isinstance(cmd, list) or not cmd or \
            not all(isinstance(a, str) and a for a in cmd):
        raise HarnessError("config: plugin.command must be a non-empty "
                           "list of strings")
    # ${HARNESS_DIR} expands to this file's directory, so shipped example
    # configs work from any checkout. Nothing is read from the environment.
    plugin["command"] = [a.replace("${HARNESS_DIR}", HARNESS_DIR) for a in cmd]
    if not isinstance(plugin.get("name"), str) or not plugin["name"]:
        raise HarnessError("config: plugin.name is required")
    if not isinstance(cfg["dry_run"], bool):
        raise HarnessError("config: dry_run must be true or false")
    if not isinstance(plugin["allow_remove"], bool):
        raise HarnessError("config: plugin.allow_remove must be true or false")
    tier = cfg["max_tier"]
    if not isinstance(tier, int) or isinstance(tier, bool) or \
            not 0 <= tier <= 6:
        raise HarnessError("config: max_tier must be an integer 0..6")
    return cfg


def ensure_state_dir(cfg):
    os.makedirs(cfg["state_dir"], mode=0o700, exist_ok=True)


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
                 os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
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

def run_plugin(cfg, args, stdin_data=None, timeout=None):
    """Run the plugin with an argument vector (never a shell). Returns
    (exit_code, stdout_text). Stdin is /dev/null unless data is given. The
    plugin gets its own process group, and on timeout that group -- which
    this function created and recorded -- is killed."""
    cmd = list(cfg["plugin"]["command"]) + list(args)
    if timeout is None:
        timeout = float(cfg["plugin"]["timeout_s"])
    proc = subprocess.Popen(
        cmd,
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
        raise HarnessError("plugin %s timed out after %ss" %
                           (args[0] if args else "?", timeout))
    if len(out) > OUTPUT_CAP:
        raise HarnessError("plugin output exceeds %d bytes" % OUTPUT_CAP)
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
        raise HarnessError("describe: not a JSON object")
    verbs = d.get("verbs")
    if not isinstance(verbs, list) or "list" not in verbs:
        raise HarnessError("describe: verbs must be a list including 'list'")
    for k in ("size", "load"):
        spec = d.get(k) or {}
        if not isinstance(spec, dict):
            raise HarnessError("describe: %s must be an object" % k)
        scale = spec.get("scale", "relative")
        if scale not in ("native", "linear", "relative"):
            raise HarnessError("describe: %s.scale %r unknown" % (k, scale))
        if scale == "linear" and not _is_num(spec.get("max")):
            raise HarnessError("describe: %s.scale linear needs max" % k)
    return d


def describe(cfg):
    code, out = run_plugin(cfg, ["describe"])
    if code != 0:
        raise HarnessError("plugin describe exited %d" % code)
    try:
        return validate_describe(json.loads(out))
    except ValueError:
        raise HarnessError("describe: output is not JSON")


# ---------------------------------------------------------- normalisation --

def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and \
        v == v and v not in (float("inf"), float("-inf"))


def sanitize_label(name, limit=LABEL_MAX):
    """Plugin name -> what the games draw: printable ASCII, whitespace
    (TAB and newline included) collapsed to single spaces, at most `limit`
    bytes. A TAB or newline can never survive, so a name can never add a
    field or a record to the v1 output."""
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


def normalize(items, desc, cfg):
    """Rank, truncate and scale plugin items. Returns dicts without ids."""
    max_tier = cfg["max_tier"]
    indexed = list(enumerate(items))

    def rank_of(pair):
        pos, it = pair
        r = it.get("rank")
        return (r if _is_num(r) else float(pos), pos)

    indexed.sort(key=rank_of)
    indexed = indexed[:max(0, int(cfg["max_items"]))]
    ordered = [it for _, it in indexed]
    n = len(ordered)

    sizes = []
    for pos, it in enumerate(ordered):
        v = it.get("size")
        sizes.append(float(v) if _is_num(v) and v >= 0 else None)
    size_spec = desc.get("size") or {}
    if all(v is None for v in sizes):
        # No sizes at all: the top-ranked item is the biggest.
        size_spec = {"scale": "relative"}
        sizes = [float(n - pos) for pos in range(n)]
    loads = []
    for it in ordered:
        v = it.get("load")
        loads.append(float(v) if _is_num(v) and v >= 0 else None)

    weights = scale_metric(sizes, size_spec, SIZE_LADDER, max_tier)
    load_vals = scale_metric(loads, desc.get("load"), LOAD_LADDER, 6)

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
            "parent_key": parent if isinstance(parent, str) else None,
        })
    return out


# ----------------------------------------------------------------- id map --

def assign_ids(cfg, items, now):
    """Give every item a stable id from the persisted map. Ids are handed out
    from a counter that never goes back, so an id is never reused for a
    different key, even after the key is forgotten. Call under state.lock."""
    path = state_path(cfg, "ids.json")
    m = read_json(path, None)
    if not isinstance(m, dict) or m.get("version") != 1:
        m = {"version": 1, "next": ID_MIN, "keys": {}}
    keys = m["keys"]
    for it in items:
        rec = keys.get(it["key"])
        if rec is None:
            if m["next"] > ID_MAX:
                raise HarnessError("id space exhausted")
            rec = {"id": m["next"]}
            m["next"] += 1
            keys[it["key"]] = rec
        rec["seen"] = now
        it["id"] = rec["id"]
    if len(keys) > ID_MAP_CAP:
        drop = sorted(keys, key=lambda k: keys[k].get("seen", 0))
        for k in drop[:len(keys) - ID_MAP_CAP]:
            del keys[k]
    write_json_atomic(path, m)
    by_key = {it["key"]: it["id"] for it in items}
    for it in items:
        it["parent"] = by_key.get(it.pop("parent_key"), 0)
        if it["parent"] == it["id"]:
            it["parent"] = 0


# ------------------------------------------------------------------ cache --

def refresh(cfg):
    """Run describe + list and rewrite the cache. Single-flight: if another
    refresher holds refresh.lock this returns False without running."""
    ensure_state_dir(cfg)
    with Locked(state_path(cfg, "refresh.lock"), blocking=False) as lk:
        if not lk.held:
            return False
        started = time.time()
        with open(state_path(cfg, "refresh.pid"), "w") as f:
            f.write("%d\n" % os.getpid())
        desc = describe(cfg)
        code, out = run_plugin(cfg, ["list"])
        if code != 0:
            raise HarnessError("plugin list exited %d" % code)
        raw, skipped = parse_list(out)
        items = normalize(raw, desc, cfg)
        now = round(time.time(), 3)
        with Locked(state_path(cfg, "state.lock")):
            assign_ids(cfg, items, now)
            write_json_atomic(state_path(cfg, "cache.json"), {
                "version": 1,
                "fetched_at": now,
                "describe": desc,
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


def spawn_refresher(cfg):
    """Start a detached refresher in its own session, so psDoom's poll
    timeout (which kills the poll's process group) cannot kill it, and with
    no handle on the poll's stdout, so the poll ends at once."""
    log = os.open(state_path(cfg, "refresh.log"),
                  os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        subprocess.Popen(
            [sys.executable, os.path.abspath(__file__),
             "--config", cfg["config_path"], "refresh"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log,
            close_fds=True, start_new_session=True)
    finally:
        os.close(log)


def render_v1(items):
    lines = ["#!psdoom v1"]
    for it in items:
        lines.append("id=%d\tparent=%d\tweight=%d\tload=%d\tflags=%d\tlabel=%s"
                     % (it["id"], it["parent"], it["weight"], it["load"],
                        it["flags"], it["label"]))
    return "\n".join(lines) + "\n"


def cmd_poll(cfg):
    ensure_state_dir(cfg)
    cache_path = state_path(cfg, "cache.json")
    cache = read_json(cache_path)
    now = time.time()
    dirty = os.path.exists(state_path(cfg, "dirty"))
    stale = (cache is None or dirty or
             now - cache.get("fetched_at", 0) >= cfg["cache_ttl_s"])
    try:
        last_try = os.stat(state_path(cfg, "refresh.pid")).st_mtime
    except FileNotFoundError:
        last_try = 0
    # A failing plugin is retried once per cache_ttl_s, not on every poll.
    backoff = not dirty and now - last_try < cfg["cache_ttl_s"]
    if stale and not backoff and not refresher_running(cfg):
        spawn_refresher(cfg)
    if cache is None:
        deadline = time.monotonic() + cfg["first_poll_wait_ms"] / 1000.0
        while cache is None and time.monotonic() < deadline:
            time.sleep(0.02)
            cache = read_json(cache_path)
    sys.stdout.write(render_v1(cache["items"] if cache else []))
    return 0


# ---------------------------------------------------------------- respond --

def rate_allow(cfg, now):
    """Sliding-window limit on executed actions. Call under state.lock."""
    rl = cfg["rate_limit"]
    limit, window = int(rl["actions"]), float(rl["per_s"])
    path = state_path(cfg, "rate.json")
    stamps = [t for t in (read_json(path, []) or []) if now - t < window]
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


def cmd_respond(cfg, argv):
    """Never raises; psDoom does not read the exit code, the audit log is the
    record of what happened."""
    ensure_state_dir(cfg)
    verb = argv[0] if argv else ""
    ident = argv[1] if len(argv) > 1 else ""
    extras = parse_extras(argv[2:])
    entry = {"verb": verb[:32], "id": ident[:16],
             "game_label": extras.get("label", "")[:64]}
    pverb = VERB_MAP.get(verb)
    if pverb is None:
        audit(cfg, dict(entry, outcome="ignored-verb"))
        return 0
    cache = read_json(state_path(cfg, "cache.json")) or {}
    item = None
    if ident.isdigit():
        for it in cache.get("items", []):
            if it["id"] == int(ident):
                item = it
                break
    if item is None:
        audit(cfg, dict(entry, outcome="unknown-id"))
        return 0
    entry.update(action=pverb, key=item["key"], name=item["name"])
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
    if cfg["dry_run"]:
        audit(cfg, dict(entry, outcome="dry-run"))
        return 0
    if pverb == "remove" and not cfg["plugin"]["allow_remove"]:
        audit(cfg, dict(entry, outcome="blocked"))
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
    except (HarnessError, OSError) as e:
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
                c = read_json(state_path(cfg, "cache.json"))
                if c:
                    c["items"] = [i for i in c["items"]
                                  if i["key"] != item["key"]]
                    write_json_atomic(state_path(cfg, "cache.json"), c)
            with open(state_path(cfg, "dirty"), "w"):
                pass
    return 0


def cmd_undo(cfg, action_id):
    ensure_state_dir(cfg)
    log = read_audit(cfg)
    target = None
    for e in log:
        if e.get("action_id") == action_id and e.get("outcome") == "ok":
            target = e
    if target is None:
        raise HarnessError("no successful action %r in the audit log"
                           % action_id)
    if any(e.get("verb") == "undo" and e.get("undoes") == action_id and
           e.get("outcome") == "ok" for e in log):
        raise HarnessError("action %r was already undone" % action_id)
    if not isinstance(target.get("undo"), dict):
        raise HarnessError("action %r has no undo record (the plugin "
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
        raise HarnessError("plugin restore failed: %s"
                           % res.get("message", "exit %d" % code))
    print("undone %s" % action_id)
    return 0


# ------------------------------------------------------------------- main --

USAGE = ("usage: psdoom_harness.py --config FILE "
         "(poll | respond VERB ID [k=v...] | refresh | undo ACTION_ID | "
         "describe)")


def main(argv):
    if len(argv) < 3 or argv[0] != "--config":
        sys.stderr.write(USAGE + "\n")
        return 2
    config_path, cmd, rest = argv[1], argv[2], argv[3:]
    if cmd == "poll":
        try:
            return cmd_poll(load_config(config_path))
        except Exception as e:   # never break the game's poll
            sys.stdout.write("#!psdoom v1\n# harness error: %s\n"
                             % sanitize_label(e, 200))
            return 0
    if cmd == "respond":
        try:
            return cmd_respond(load_config(config_path), rest)
        except Exception as e:
            sys.stderr.write("psdoom-harness: %s\n" % e)
            return 0
    try:
        cfg = load_config(config_path)
        if cmd == "refresh":
            refresh(cfg)
            return 0
        if cmd == "undo" and len(rest) == 1:
            return cmd_undo(cfg, rest[0])
        if cmd == "describe":
            print(json.dumps(describe(cfg), indent=2, sort_keys=True))
            return 0
    except (HarnessError, OSError, ValueError) as e:
        sys.stderr.write("psdoom-harness: %s\n" % e)
        return 1
    sys.stderr.write(USAGE + "\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
