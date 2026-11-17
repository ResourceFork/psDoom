"""Tests for Summoner and its two plugins.

Rules these tests keep:
- every state root is a fresh directory under SCRATCH (set by run_tests.py);
- the processes plugin is only ever exercised read-only (`describe`, `list`)
  or through Summoner in dry-run, so no real process is signalled or
  reniced; the real-action path is tested through the CSV plugin on CSV
  files these tests create, and the built-in `ps` config's actions through
  a fake plugin swapped in for its command, in this process;
- any refresher Summoner detaches is waited for, and only killed by the
  pid it recorded after checking that pid still runs this test's config.
"""

import fcntl
import importlib.util
import io
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

SCRATCH = None   # set by run_tests.py

SUMMONER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUMMONER_PY = os.path.join(SUMMONER_DIR, "summoner.py")
CSV_PLUGIN = os.path.join(SUMMONER_DIR, "plugins", "csv_rows.py")
PROC_PLUGIN = os.path.join(SUMMONER_DIR, "plugins", "processes.py")
SUMMON_CMD = os.path.join(SUMMONER_DIR, "summon")
PY = sys.executable


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


H = _load("summoner", SUMMONER_PY)
P = _load("processes_plugin", PROC_PLUGIN)

# A scriptable fake plugin: behaviour comes from a JSON spec file named on
# its command line (never from the environment), and every call is appended
# to a calls file so tests can prove what was, and was not, invoked.
FAKE_PLUGIN = r'''
import json, sys, time
spec = json.load(open(sys.argv[1]))
verb = sys.argv[2] if len(sys.argv) > 2 else ""
with open(spec["calls"], "a") as f:
    f.write(json.dumps(sys.argv[2:]) + "\n")
if verb == "describe":
    print(json.dumps(spec.get("describe", {"verbs": ["list", "nudge",
                                                     "remove"]})))
elif verb == "list":
    time.sleep(spec.get("list_sleep", 0))
    if spec.get("list_fail"):
        sys.exit(3)
    for it in spec["items"]:
        print(json.dumps(it))
elif verb in ("nudge", "remove"):
    print(json.dumps({"ok": True, "undo": None}))
'''


HOST_MODE = "delete"   # the helpers act as a host in its fullest mode


def with_mode(args):
    """Add `mode=delete` to a post/delete call that names no mode, as a host
    in its fullest mode would, so a test's plugin config decides."""
    args = list(args)
    i = 0
    while i < len(args) and args[i] in ("--root", "--plugin", "--config"):
        i += 2
    if i < len(args) and args[i] in ("post", "delete") and \
            not any(a.startswith("mode=") for a in args[i:]):
        args.append("mode=" + HOST_MODE)
    return args


def parse_v1(text):
    """Minimal reader of the v1 poll format, as the game splits it."""
    out = []
    for line in text.split("\n"):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        rec = {}
        for field in line.split("\t"):
            if "=" in field:
                k, v = field.split("=", 1)
                rec[k] = v
        out.append(rec)
    return out


class Base(unittest.TestCase):

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="t-", dir=SCRATCH)
        self.state = os.path.join(self.root, "state")
        self.configs = []

    def tearDown(self):
        for cfg_path in self.configs:
            self._reap_refresher(cfg_path)
        # A refresher the last poll spawned may not hold its lock yet; wait
        # (read-only, nothing is signalled) until no process still names
        # this test's own folder, so nothing writes into it during removal.
        self._wait_for_quiet(self.root)
        root = os.path.realpath(self.root)
        assert root.startswith(os.path.realpath(SCRATCH) + os.sep), root
        shutil.rmtree(root)

    def _wait_for_quiet(self, path, seconds=15):
        me = str(os.getpid())
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            out = subprocess.run(["ps", "-xo", "pid=,command="],
                                 capture_output=True, text=True).stdout
            busy = [ln for ln in out.splitlines()
                    if path in ln and ln.split(None, 1)[0] != me]
            if not busy:
                return
            time.sleep(0.05)

    def _reap_refresher(self, cfg_path):
        with open(cfg_path) as f:
            state = json.load(f)["state_dir"]
        lock = os.path.join(state, "refresh.lock")
        if not os.path.exists(lock):
            return
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            fd = os.open(lock, os.O_RDWR)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return            # nobody holds it: no refresher running
            except OSError:
                pass
            finally:
                os.close(fd)
            time.sleep(0.05)
        # Still running: kill only the group we can prove is ours.
        with open(os.path.join(state, "refresh.pid")) as f:
            pid = int(f.read().strip())
        cmd = subprocess.run(["ps", "-o", "command=", "-p", str(pid)],
                             capture_output=True, text=True).stdout
        if cfg_path in cmd and "refresh" in cmd:
            os.killpg(pid, signal.SIGKILL)

    # -- config / running ---------------------------------------------------

    def config(self, plugin_cmd, name="test", **over):
        cfg = {"state_dir": self.state,
               "plugin": {"name": name, "command": plugin_cmd}}
        plugin_over = over.pop("plugin", {})
        cfg["plugin"].update(plugin_over)
        cfg.update(over)
        path = os.path.join(self.root, "cfg-%d.json" % len(self.configs))
        with open(path, "w") as f:
            json.dump(cfg, f)
        self.configs.append(path)
        return path

    def env(self, **extra):
        """A child environment that can never reach the real home: HOME is
        a fake one under this test's root, and SUMMONER_ROOT is unset
        unless a test sets it on purpose."""
        env = {k: v for k, v in os.environ.items() if k != "SUMMONER_ROOT"}
        env["HOME"] = os.path.join(self.root, "fakehome")
        env.update(extra)
        return env

    def summon(self, cfg, *args, timeout=20, raw=False):
        args = list(args) if raw else with_mode(args)
        if args and args[0] == "poll":
            args[0] = "get"          # the tests' shorthand for the game's get
        t0 = time.monotonic()
        r = subprocess.run([PY, SUMMONER_PY, "--config", cfg] + args,
                           stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, timeout=timeout, cwd=self.root,
                           env=self.env())
        return r, time.monotonic() - t0

    def cli(self, *args, env=None, exe=None, timeout=20):
        """Run the summon launcher itself (its shebang interpreter is
        bypassed so the suite's own Python runs it)."""
        r = subprocess.run([PY, exe or SUMMON_CMD] + with_mode(args),
                           stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, timeout=timeout, cwd=self.root,
                           env=env or self.env())
        return r

    def refresh(self, cfg):
        r, _ = self.summon(cfg, "refresh")
        self.assertEqual(r.returncode, 0, r.stderr)

    def poll(self, cfg):
        r, _ = self.summon(cfg, "poll")
        self.assertEqual(r.returncode, 0, r.stderr)
        return parse_v1(r.stdout)

    def audit(self):
        path = os.path.join(self.state, "audit.jsonl")
        if not os.path.exists(path):
            return []
        with open(path) as f:
            return [json.loads(line) for line in f]

    def fake(self, items, spec_name="default", **spec):
        # One spec file per name: calling fake() again with the same name
        # changes what that plugin lists (the id tests rely on it).
        spec_path = os.path.join(self.root, "fake-spec-%s.json" % spec_name)
        calls = os.path.join(self.root, "fake-calls.jsonl")
        plugin = os.path.join(self.root, "fake.py")
        with open(plugin, "w") as f:
            f.write(FAKE_PLUGIN)
        spec.update(items=items, calls=calls)
        with open(spec_path, "w") as f:
            json.dump(spec, f)
        self.calls_path = calls
        return [PY, plugin, spec_path]

    def calls(self):
        if not os.path.exists(self.calls_path):
            return []
        with open(self.calls_path) as f:
            return [json.loads(line) for line in f]

    def csv_file(self, text, name="list.csv"):
        path = os.path.join(self.root, name)
        with open(path, "w", newline="") as f:
            f.write(text)
        return path

    def csv_cmd(self, path, *extra):
        return [PY, CSV_PLUGIN, "--file", path] + list(extra)

    def read(self, path):
        with open(path, newline="") as f:
            return f.read()

    def id_for(self, rows, label):
        for r in rows:
            if r["label"] == label:
                return r["id"]
        self.fail("no entity labelled %r in %r" % (label, rows))


# ------------------------------------------------------------- unit tests --

class LabelTests(unittest.TestCase):

    def test_trimmed_to_31_bytes(self):
        s = H.sanitize_label("x" * 50)
        self.assertEqual(len(s.encode()), 31)

    def test_non_ascii_folded_or_spaced(self):
        self.assertEqual(H.sanitize_label("Café ☕ review"), "Cafe review")
        self.assertEqual(len(H.sanitize_label("é" * 40).encode()), 31)

    def test_tab_newline_control_collapse(self):
        s = H.sanitize_label("a\tb\nc\r\x00d\x1be")
        self.assertNotIn("\t", s)
        self.assertNotIn("\n", s)
        self.assertEqual(s, "a b c d e")


class LadderTests(unittest.TestCase):

    def test_native_passes_through_and_clamps(self):
        out = H.scale_metric([5.0, None, 9e12], {"scale": "native"},
                             H.SIZE_LADDER, 6)
        self.assertEqual(out, [5, 0, H.SIZE_LADDER[6]])
        out = H.scale_metric([9e12], {"scale": "native"}, H.SIZE_LADDER, 3)
        self.assertEqual(out, [H.SIZE_LADDER[3]])

    def test_relative_spans_the_ladder(self):
        out = H.scale_metric([1.0, 5.5, 10.0], {"scale": "relative"},
                             H.SIZE_LADDER, 6)
        self.assertEqual(out, [H.SIZE_LADDER[0], H.SIZE_LADDER[3],
                               H.SIZE_LADDER[6]])
        out = H.scale_metric([0.0, 100.0], {}, H.LOAD_LADDER, 6)
        self.assertEqual(out, [0, 500])

    def test_relative_all_equal_is_bottom_tier(self):
        out = H.scale_metric([3.0, 3.0], {"scale": "relative"},
                             H.SIZE_LADDER, 6)
        self.assertEqual(out, [0, 0])

    def test_linear_uses_declared_range(self):
        out = H.scale_metric([0.0, 5.0, 10.0, 20.0],
                             {"scale": "linear", "min": 0, "max": 10},
                             H.LOAD_LADDER, 6)
        self.assertEqual(out, [0, 60, 500, 500])

    def test_max_tier_caps_relative(self):
        out = H.scale_metric([0.0, 1.0], {"scale": "relative"},
                             H.SIZE_LADDER, 5)
        self.assertEqual(out[1], H.SIZE_LADDER[5])

    def test_every_value_lands_on_a_rung(self):
        for v in H.scale_metric([float(i) for i in range(37)], {},
                                H.SIZE_LADDER, 6):
            self.assertIn(v, H.SIZE_LADDER)

    def test_missing_sizes_follow_plugin_order(self):
        # rank is no longer read: the plugin's order is the order
        items = [{"key": "c", "rank": 2}, {"key": "a", "rank": 0},
                 {"key": "b", "rank": 1}]
        cfg = {"max_tier": 6, "max_items": 10}
        out = H.normalize(items, {"verbs": ["list"]}, cfg)
        self.assertEqual([i["key"] for i in out], ["c", "a", "b"])
        self.assertEqual([i["weight"] for i in out],
                         [H.SIZE_LADDER[6], H.SIZE_LADDER[3], 0])

    def test_order_preserved_and_max_items_keeps_the_head(self):
        items = [{"key": str(i), "size": i} for i in range(10)]   # ascending
        out = H.normalize(items, {}, {"max_tier": 6, "max_items": 3})
        self.assertEqual([i["key"] for i in out], ["0", "1", "2"])

    def test_grade_from_primary_values_and_direction(self):
        desc = {"dimensions": H.validate_dimensions(
            [{"id": "hrs", "kind": "numeric", "dir": "desc"}])}
        items = [{"key": "a", "values": {"hrs": 10}},
                 {"key": "b", "values": {"hrs": 5}},
                 {"key": "c", "values": {"hrs": 0}}]
        cfg = {"max_tier": 6, "max_items": 10}
        out = H.normalize(items, desc, cfg, "hrs")
        self.assertEqual([i["grade"] for i in out], [1.0, 0.5, 0.0])
        cfg["_applied_dirs"] = {"hrs": "asc"}
        out = H.normalize(items, desc, cfg, "hrs")
        self.assertEqual([i["grade"] for i in out], [0.0, 0.5, 1.0])
        # the plugin's own grade still wins
        items[2]["grade"] = 0.9
        out = H.normalize(items, desc, cfg, "hrs")
        self.assertEqual(out[2]["grade"], 0.9)


class DimensionTests(unittest.TestCase):

    def test_validation(self):
        good = H.validate_dimensions([
            {"id": "mem", "kind": "numeric", "label": "Mem\tory"},
            {"id": "app", "kind": "category"}])
        self.assertEqual(good[0]["roles"], ["sort"])
        self.assertEqual(good[0]["dir"], "desc")
        self.assertEqual(good[0]["label"], "Mem ory")     # sanitized
        self.assertEqual(good[1]["roles"], ["group"])
        self.assertEqual(H.validate_dimensions(None), [])
        for bad in ([{"id": "Mem", "kind": "numeric"}],
                    [{"id": "a b", "kind": "numeric"}],
                    [{"id": "a;rm", "kind": "numeric"}],
                    [{"id": "x" * 17, "kind": "numeric"}],
                    [{"id": "m", "kind": "weird"}],
                    [{"id": "m", "kind": "numeric", "dir": "up"}],
                    [{"id": "m", "kind": "numeric", "roles": ["kill"]}],
                    [{"id": "m", "kind": "numeric"},
                     {"id": "m", "kind": "category"}],
                    "nope"):
            with self.assertRaises(H.SummonerError):
                H.validate_dimensions(bad)
        with self.assertRaises(H.SummonerError):
            H.validate_describe({"verbs": ["list"], "default_sort": "x",
                                 "dimensions": [{"id": "y",
                                                 "kind": "numeric"}]})

    def desc(self):
        return H.validate_describe({
            "verbs": ["list"], "default_sort": "mem",
            "dimensions": [{"id": "mem", "kind": "numeric"},
                           {"id": "cpu", "kind": "numeric", "dir": "desc"},
                           {"id": "age", "kind": "numeric", "dir": "asc"},
                           {"id": "app", "kind": "category"}]})

    def apply(self, *words):
        req, unknown = H.canonical_request(words)
        return H.apply_request(req, self.desc(), unknown)

    def test_request_applied_and_passed_on(self):
        applied, notes, words = self.apply("sort=cpu,age,mem:asc",
                                           "group=app", "limit=10")
        self.assertEqual(applied["sort"],
                         [["cpu", "desc"], ["age", "asc"], ["mem", "asc"]])
        self.assertEqual(words, ["sort=cpu:desc,age:asc,mem:asc",
                                 "group=app", "limit=10"])
        self.assertEqual(notes, [])

    def test_unknown_ids_fall_back_with_a_note(self):
        applied, notes, words = self.apply("sort=bogus", "group=mem",
                                           "limit=0", "colour=red")
        self.assertEqual(applied["sort"], [["mem", "desc"]])   # the default
        self.assertIsNone(applied["group"])        # mem is not a group dim
        self.assertIsNone(applied["limit"])
        self.assertEqual(words, ["sort=mem:desc"])
        joined = " ".join(notes)
        for frag in ("sort=bogus", "group=mem", "limit=0", "colour=red",
                     "using sort=mem"):
            self.assertIn(frag, joined)
        # nothing unsafe can reach the plugin's argv
        applied, notes, words = self.apply("sort=$(touch x)", "group=;rm")
        self.assertEqual(words, ["sort=mem:desc"])

    def test_request_key_is_canonical(self):
        a, _ = H.canonical_request(["limit=5", "sort=cpu"])
        b, _ = H.canonical_request(["sort=cpu", "limit=5"])
        self.assertEqual(H.request_key(a), H.request_key(b))
        c, _ = H.canonical_request(["sort=mem"])
        self.assertNotEqual(H.request_key(a), H.request_key(c))


class ParseListTests(unittest.TestCase):

    def test_bad_lines_and_keys_skipped(self):
        text = "\n".join([
            '{"key": "ok", "name": "fine"}',
            'not json',
            '[1, 2]',
            '{"name": "no key"}',
            '{"key": ""}',
            '{"key": "-rf"}',
            '{"key": 7}',
            '{"key": "ok", "name": "duplicate"}',
            '# comment',
        ])
        items, skipped = H.parse_list(text)
        self.assertEqual([i["name"] for i in items], ["fine"])
        self.assertEqual(skipped, 7)


class ConfigTests(Base):

    def test_state_dir_must_be_explicit_and_absolute(self):
        for bad in ({}, {"state_dir": ""}, {"state_dir": "rel/state"},
                    {"state_dir": "/"}):
            path = os.path.join(self.root, "bad.json")
            with open(path, "w") as f:
                json.dump(dict(bad, plugin={"name": "x", "command": ["x"]}),
                          f)
            with self.assertRaises(H.SummonerError):
                H.load_config(path)

    def test_bad_config_poll_still_speaks_the_contract(self):
        path = os.path.join(self.root, "bad.json")
        with open(path, "w") as f:
            f.write("{")
        r, _ = self.summon(path, "poll")
        self.assertEqual(r.returncode, 0)
        self.assertFalse(r.stdout.startswith("#!"))   # no header
        self.assertEqual(parse_v1(r.stdout), [])

    def test_shipped_examples_need_a_real_state_dir(self):
        ex_dir = os.path.join(SUMMONER_DIR, "examples")
        for name in ("processes.json", "csv.json"):
            with self.assertRaises(H.SummonerError):
                H.load_config(os.path.join(ex_dir, name))
        with open(os.path.join(ex_dir, "processes.json")) as f:
            c = json.load(f)
        c["state_dir"] = self.state
        path = os.path.join(self.root, "ex.json")
        with open(path, "w") as f:
            json.dump(c, f)
        cfg = H.load_config(path)
        self.assertTrue(cfg["dry_run"])
        self.assertFalse(cfg["plugin"]["allow_remove"])
        r, _ = self.summon(path, "describe")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout)["name"], "processes")

    def test_summoner_dir_placeholder(self):
        cfg = self.config(["${SUMMONER_DIR}/plugins/csv_rows.py"])
        self.assertEqual(H.load_config(cfg)["plugin"]["command"][0],
                         CSV_PLUGIN)

    def test_python_placeholder_is_summoners_interpreter(self):
        cfg = self.config(["${PYTHON}", "${SUMMONER_DIR}/plugins/csv_rows.py"])
        cmd = H.load_config(cfg)["plugin"]["command"]
        self.assertEqual(cmd, [sys.executable, CSV_PLUGIN])


# ---------------------------------------------------- Summoner behaviour --

class IdTests(Base):

    def test_ids_stable_across_polls_and_never_reused(self):
        cmd = self.fake([{"key": "a"}, {"key": "b"}])
        cfg = self.config(cmd)
        self.refresh(cfg)
        first = {r["label"]: r["id"] for r in self.poll(cfg)}
        self.refresh(cfg)
        self.assertEqual(first, {r["label"]: r["id"] for r in self.poll(cfg)})
        # b leaves, c arrives: c gets a fresh id, never b's.
        self.fake([{"key": "a"}, {"key": "c"}])
        self.refresh(cfg)
        second = {r["label"]: r["id"] for r in self.poll(cfg)}
        self.assertEqual(second["a"], first["a"])
        self.assertNotIn(second["c"], first.values())
        # b comes back: it keeps its old id.
        self.fake([{"key": "b"}, {"key": "a"}, {"key": "c"}])
        self.refresh(cfg)
        third = {r["label"]: r["id"] for r in self.poll(cfg)}
        self.assertEqual(third["b"], first["b"])
        for v in third.values():
            self.assertGreaterEqual(int(v), 2)

    def test_ids_survive_a_restart_of_the_state(self):
        cmd = self.fake([{"key": "x"}])
        cfg = self.config(cmd)
        self.refresh(cfg)
        before = self.poll(cfg)[0]["id"]
        os.unlink(os.path.join(self.state, "cache.json"))
        self.refresh(cfg)
        self.assertEqual(self.poll(cfg)[0]["id"], before)

    def test_parent_key_maps_to_parent_id(self):
        cmd = self.fake([{"key": "p", "name": "parent"},
                         {"key": "c", "name": "child", "parent": "p"},
                         {"key": "o", "name": "orphan", "parent": "gone"}])
        cfg = self.config(cmd)
        self.refresh(cfg)
        rows = {r["label"]: r for r in self.poll(cfg)}
        self.assertEqual(rows["child"]["parent"], rows["parent"]["id"])
        self.assertEqual(rows["orphan"]["parent"], "0")


class CacheTests(Base):

    def test_slow_plugin_never_blocks_the_poll(self):
        cmd = self.fake([{"key": "slow", "name": "slow one"}], list_sleep=2.5)
        cfg = self.config(cmd, cache_ttl_s=0)
        # No cache yet: the poll waits at most first_poll_wait_ms, then
        # answers with an empty (but valid) listing.
        r, took = self.summon(cfg, "poll")
        self.assertLess(took, 1.0)
        self.assertEqual(parse_v1(r.stdout), [])
        self.assertFalse(r.stdout.startswith("#!"))   # no header
        # The detached refresher finishes on its own.
        deadline = time.monotonic() + 10
        rows = []
        while not rows and time.monotonic() < deadline:
            time.sleep(0.2)
            rows = self.poll(cfg)
        self.assertEqual([r["label"] for r in rows], ["slow one"])
        # With a cache, a stale poll answers at once from it.
        r, took = self.summon(cfg, "poll")
        self.assertLess(took, 1.0)
        self.assertEqual(len(parse_v1(r.stdout)), 1)

    def test_one_refresher_at_a_time(self):
        cmd = self.fake([{"key": "k"}], list_sleep=1.0)
        cfg = self.config(cmd, cache_ttl_s=0, first_poll_wait_ms=0)
        for _ in range(5):
            self.summon(cfg, "poll")
        self._reap_refresher(cfg)
        lists = [c for c in self.calls() if c and c[0] == "list"]
        self.assertEqual(len(lists), 1)

    def test_plugin_timeout_is_bounded(self):
        cmd = self.fake([{"key": "k"}], list_sleep=30)
        cfg = self.config(cmd, plugin={"timeout_s": 0.5})
        r, took = self.summon(cfg, "refresh")
        self.assertEqual(r.returncode, 1)
        self.assertIn("timed out", r.stderr)
        self.assertLess(took, 5)


class RespondTests(Base):

    def setUp(self):
        super().setUp()
        self.csv = self.csv_file("id,name,priority\n"
                                 "a,Alpha,5\nb,Bravo,9\nc,Charlie,1\n")
        self.original = self.read(self.csv)

    def csv_config(self, **over):
        cfg = self.config(self.csv_cmd(self.csv, "--priority-column",
                                       "priority"), name="csv", **over)
        self.refresh(cfg)
        return cfg

    def respond(self, cfg, *args):
        r, _ = self.summon(cfg, *args)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_host_mode_decides_and_stricter_of_wins(self):
        cfg = self.csv_config(dry_run=False, plugin={"allow_remove": True})
        rows = self.poll(cfg)
        a, b = self.id_for(rows, "Alpha"), self.id_for(rows, "Bravo")
        for mode in (None, "dryrun", "bogus"):
            extra = [] if mode is None else ["mode=" + mode]
            self.summon(cfg, "post", a, "amount=4", *extra, raw=True)
            self.summon(cfg, "delete", b, *extra, raw=True)
        self.assertEqual(self.read(self.csv), self.original)   # nothing ran
        self.summon(cfg, "delete", b, "mode=post-only", raw=True)
        self.assertIn("Bravo", self.read(self.csv))           # refused
        self.summon(cfg, "post", a, "amount=4", "mode=post-only", raw=True)
        self.assertIn("a,Alpha,1", self.read(self.csv))       # post runs
        self.summon(cfg, "delete", b, "mode=delete", raw=True)
        self.assertNotIn("Bravo", self.read(self.csv))        # delete runs
        log = self.audit()
        self.assertEqual([(e["mode"], e["outcome"], e.get("reason"))
                          for e in log],
                         [("dryrun", "dry-run", "host mode")] * 6 +
                         [("post-only", "blocked", "host mode"),
                          ("post-only", "ok", None),
                          ("delete", "ok", None)])

    def test_plugin_config_is_the_stricter_limit(self):
        # host says delete, the plugin config says dry run / remove off
        cfg = self.csv_config()                       # dry_run default
        ident = self.id_for(self.poll(cfg), "Bravo")
        self.summon(cfg, "delete", ident, "mode=delete", raw=True)
        cfg2 = self.config(self.csv_cmd(self.csv, "--priority-column",
                                        "priority"), name="csv",
                           dry_run=False,
                           state_dir=os.path.join(self.root, "s2"))
        self.summon(cfg2, "refresh")
        ident = self.id_for(parse_v1(self.summon(cfg2, "get")[0].stdout),
                            "Bravo")
        self.summon(cfg2, "delete", ident, "mode=delete", raw=True)
        self.assertEqual(self.read(self.csv), self.original)
        self.assertEqual(self.audit()[-1]["reason"], "plugin config")
        with open(os.path.join(self.root, "s2", "audit.jsonl")) as f:
            e = json.loads(f.read().splitlines()[-1])
        self.assertEqual((e["outcome"], e["reason"]),
                         ("blocked", "plugin config"))

    def test_dry_run_is_the_default(self):
        cfg = self.csv_config(plugin={"allow_remove": True})
        rows = self.poll(cfg)
        self.respond(cfg, "delete", self.id_for(rows, "Bravo"))
        self.respond(cfg, "post", self.id_for(rows, "Alpha"), "amount=4")
        self.assertEqual(self.read(self.csv), self.original)
        self.assertEqual([e["outcome"] for e in self.audit()],
                         ["dry-run", "dry-run"])

    def test_remove_needs_opt_in(self):
        cfg = self.csv_config(dry_run=False)
        rows = self.poll(cfg)
        self.respond(cfg, "delete", self.id_for(rows, "Bravo"))
        self.assertEqual(self.read(self.csv), self.original)
        self.assertEqual(self.audit()[-1]["outcome"], "blocked")
        # nudge is not gated by allow_remove
        self.respond(cfg, "post", self.id_for(rows, "Bravo"), "amount=4")
        self.assertIn("b,Bravo,5", self.read(self.csv))
        self.assertEqual(self.audit()[-1]["outcome"], "ok")

    def test_remove_when_enabled(self):
        cfg = self.csv_config(dry_run=False, plugin={"allow_remove": True})
        rows = self.poll(cfg)
        self.respond(cfg, "delete", self.id_for(rows, "Bravo"))
        self.assertNotIn("Bravo", self.read(self.csv))
        # gone from the very next poll, before any refresh
        self.assertNotIn("Bravo", [r["label"] for r in self.poll(cfg)])

    def test_unknown_verb_and_unknown_id_are_noops(self):
        cmd = self.fake([{"key": "a", "name": "A"}])
        cfg = self.config(cmd, dry_run=False, plugin={"allow_remove": True})
        self.refresh(cfg)
        ident = self.poll(cfg)[0]["id"]
        before = len(self.calls())
        self.respond(cfg, "explode", ident, "amount=9")
        self.respond(cfg, "", ident)
        self.respond(cfg, "delete", "99999")
        self.respond(cfg, "delete", "not-a-number")
        self.respond(cfg, "wound", ident, "amount=4")   # not a verb now
        self.assertEqual(len(self.calls()), before)   # plugin never called
        self.assertEqual([e["outcome"] for e in self.audit()],
                         ["ignored-verb", "ignored-verb", "unknown-id",
                          "unknown-id", "ignored-verb"])

    def test_unsupported_verb_not_called(self):
        cmd = self.fake([{"key": "a", "name": "A"}],
                        describe={"verbs": ["list"]})
        cfg = self.config(cmd, dry_run=False, plugin={"allow_remove": True})
        self.refresh(cfg)
        before = len(self.calls())
        self.respond(cfg, "delete", self.poll(cfg)[0]["id"])
        self.assertEqual(len(self.calls()), before)
        self.assertEqual(self.audit()[-1]["outcome"], "unsupported")

    def test_rate_limit(self):
        cfg = self.csv_config(dry_run=False,
                              rate_limit={"actions": 2, "per_s": 60})
        ident = self.id_for(self.poll(cfg), "Alpha")
        for _ in range(4):
            self.respond(cfg, "post", ident, "amount=1")
        self.assertEqual([e["outcome"] for e in self.audit()],
                         ["ok", "ok", "rate-limited", "rate-limited"])
        self.assertIn("a,Alpha,3", self.read(self.csv))

    def test_audit_log_records_every_action(self):
        cfg = self.csv_config(dry_run=False, plugin={"allow_remove": True})
        rows = self.poll(cfg)
        self.respond(cfg, "post", self.id_for(rows, "Alpha"), "amount=2",
                     "label=Alpha")
        self.respond(cfg, "delete", self.id_for(rows, "Charlie"))
        log = self.audit()
        self.assertEqual(len(log), 2)
        w, k = log
        self.assertEqual((w["verb"], w["action"], w["key"], w["amount"],
                          w["outcome"]), ("post", "nudge", "a", 2, "ok"))
        self.assertEqual(w["undo"], {"op": "priority", "key": "a",
                                     "old": "5"})
        self.assertEqual((k["verb"], k["action"], k["key"], k["outcome"]),
                         ("delete", "remove", "c", "ok"))
        for e in log:
            self.assertEqual(e["plugin"], "csv")
            self.assertTrue(e["action_id"])
            self.assertIsInstance(e["ts"], float)

    def test_amount_is_clamped(self):
        cfg = self.csv_config(dry_run=False)
        ident = self.id_for(self.poll(cfg), "Bravo")
        self.respond(cfg, "post", ident, "amount=100000")
        self.respond(cfg, "post", ident, "amount=-3")
        self.assertEqual([e["amount"] for e in self.audit()], [100, 1])


class CsvPluginTests(Base):

    def run_plugin(self, cmd, *args, stdin=None):
        r = subprocess.run(cmd + list(args), input=stdin, capture_output=True,
                           text=True, timeout=10)
        return r.returncode, json.loads(r.stdout.strip().splitlines()[-1]) \
            if r.stdout.strip() else None

    def test_list_ranks_by_priority(self):
        path = self.csv_file("id,name,priority\na,A,1\nb,B,9\nc,C,5\n")
        r = subprocess.run(self.csv_cmd(path, "--priority-column",
                                        "priority", "list"),
                           capture_output=True, text=True)
        items = [json.loads(x) for x in r.stdout.splitlines()]
        self.assertEqual([i["key"] for i in items], ["b", "c", "a"])
        self.assertEqual([i["grade"] for i in items], [1.0, 0.5, 0.0])

    def test_dimensions_sort_group_limit(self):
        path = self.csv_file("id,name,priority,Team Name,hours,note\n"
                             "a,A,5,red,3,x\nb,B,9,blue,1,\nc,C,1,red,8,y\n"
                             "d,D,2,blue,8,z\n")
        r = subprocess.run(self.csv_cmd(path, "--priority-column",
                                        "priority", "describe"),
                           capture_output=True, text=True)
        d = json.loads(r.stdout)
        H.validate_describe(d)
        kinds = {x["id"]: x["kind"] for x in d["dimensions"]}
        self.assertEqual(kinds, {"name": "category", "priority": "numeric",
                                 "team_name": "category",
                                 "hours": "numeric", "note": "category"})
        self.assertEqual(d["default_sort"], "priority")
        r = subprocess.run(self.csv_cmd(path, "--priority-column", "priority",
                                        "list", "sort=hours:desc",
                                        "group=team_name", "limit=3"),
                           capture_output=True, text=True)
        items = [json.loads(x) for x in r.stdout.splitlines()]
        # hours desc: c(8) d(8) a(3) b(1); grouped by team in that order:
        # red [c, a], blue [d, b]; cut to 3
        self.assertEqual([i["key"] for i in items], ["c", "a", "d"])
        self.assertEqual([i["group"] for i in items], ["red", "red", "blue"])
        self.assertEqual(items[0]["grade"], 1.0)

    def test_undo_remove_restores_row_in_place(self):
        path = self.csv_file("id,name,priority,extra\r\n"
                             "a,A,1,x\r\nb,\"B, quoted\",9,y,spill\r\n"
                             "c,C,5,z\r\n")
        original = self.read(path)
        cfg = self.config(self.csv_cmd(path), name="csv", dry_run=False,
                          plugin={"allow_remove": True})
        self.refresh(cfg)
        ident = self.id_for(self.poll(cfg), "B, quoted")
        r, _ = self.summon(cfg, "delete", ident)
        self.assertNotIn("quoted", self.read(path))
        action = self.audit()[-1]["action_id"]
        r, _ = self.summon(cfg, "undo", action)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.read(path), original)   # byte-identical
        # a second undo of the same action is refused
        r, _ = self.summon(cfg, "undo", action)
        self.assertEqual(r.returncode, 1)
        self.assertIn("already undone", r.stderr)
        self.assertEqual(self.read(path), original)

    def test_undo_priority_nudge(self):
        path = self.csv_file("id,name,priority\na,A,7\n")
        cfg = self.config(self.csv_cmd(path, "--priority-column", "priority"),
                          name="csv", dry_run=False)
        self.refresh(cfg)
        self.summon(cfg, "post", self.poll(cfg)[0]["id"],
                     "amount=4")
        self.assertEqual(self.read(path), "id,name,priority\na,A,3\n")
        r, _ = self.summon(cfg, "undo", self.audit()[-1]["action_id"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.read(path), "id,name,priority\na,A,7\n")

    def test_nudge_without_priority_moves_row_down_and_undoes(self):
        text = "id,name\na,A\nb,B\nc,C\nd,D\n"
        path = self.csv_file(text)
        cmd = self.csv_cmd(path)
        code, res = self.run_plugin(cmd, "nudge", "a", "amount=2")
        self.assertEqual(code, 0)
        self.assertEqual(self.read(path), "id,name\nb,B\nc,C\na,A\nd,D\n")
        code, res = self.run_plugin(cmd, "restore",
                                    stdin=json.dumps(res["undo"]))
        self.assertEqual(code, 0)
        self.assertEqual(self.read(path), text)

    def test_restore_refuses_when_key_is_back(self):
        path = self.csv_file("id,name\na,A\n")
        cmd = self.csv_cmd(path)
        code, res = self.run_plugin(cmd, "remove", "a")
        with open(path, "a") as f:
            f.write("a,A again\n")
        code, out = self.run_plugin(cmd, "restore",
                                    stdin=json.dumps(res["undo"]))
        self.assertEqual(code, 1)
        self.assertFalse(out["ok"])

    def test_bom_is_kept(self):
        path = self.csv_file("\ufeffid,name\na,A\nb,B\n")
        self.run_plugin(self.csv_cmd(path), "remove", "a")
        self.assertEqual(self.read(path), "\ufeffid,name\nb,B\n")

    def test_missing_key_reports_not_ok(self):
        path = self.csv_file("id,name\na,A\n")
        code, res = self.run_plugin(self.csv_cmd(path), "remove", "zz")
        self.assertEqual(code, 1)
        self.assertFalse(res["ok"])
        self.assertEqual(self.read(path), "id,name\na,A\n")


class CliTests(Base):
    """The summon command line: verbs, aliases, plugin selection."""

    def setUp(self):
        super().setUp()
        self.hroot = os.path.join(self.root, "hroot")
        os.makedirs(os.path.join(self.hroot, "plugins"))

    def named(self, name, items, **over):
        """Write <hroot>/plugins/<name>.json for a fake plugin. No state_dir:
        it defaults to <hroot>/state/<name>."""
        cmd = self.fake(items, spec_name=name, **over.pop("spec", {}))
        cfg = {"plugin": {"name": name, "command": cmd}}
        cfg["plugin"].update(over.pop("plugin", {}))
        cfg.update(over)
        path = os.path.join(self.hroot, "plugins", name + ".json")
        with open(path, "w") as f:
            json.dump(cfg, f)
        self.configs.append(path)
        self.state = os.path.join(self.hroot, "state", name)
        return path

    def _reap_refresher(self, cfg_path):
        with open(cfg_path) as f:
            c = json.load(f)
        if "state_dir" in c:
            return super()._reap_refresher(cfg_path)
        name = os.path.basename(cfg_path)[:-5]
        state = os.path.join(self.hroot, "state", name)
        tmp = os.path.join(self.root, "reap-%s.json" % name)
        with open(tmp, "w") as f:
            json.dump({"state_dir": state}, f)
        super()._reap_refresher(tmp)

    def r(self, *args, **kw):
        return self.cli("--root", self.hroot, *args, **kw)

    def get(self, *pre, words=()):
        res = self.r(*pre, "get", *words)
        self.assertEqual(res.returncode, 0, res.stderr)
        return parse_v1(res.stdout)

    def wait_rows(self, *pre, words=()):
        deadline = time.monotonic() + 10
        rows = []
        while not rows and time.monotonic() < deadline:
            rows = self.get(*pre, words=words)
            if not rows:
                time.sleep(0.1)
        return rows

    def test_every_command(self):
        self.named("fake", [{"key": "a", "name": "A"}], dry_run=False,
                   plugin={"allow_remove": True})
        self.assertEqual(self.r("use", "fake").returncode, 0)
        self.assertEqual(self.r("refresh").returncode, 0)
        rows = self.get()
        ident = rows[0]["id"]
        for args in (["post", ident, "amount=2"], ["delete", ident],
                     ["post", ident, "label=A"], ["delete", ident, "label=A"]):
            # A removed item leaves the cache at once; the fake still lists
            # it, so a refresh brings it back (same key, same id).
            self.assertEqual(self.r("refresh").returncode, 0)
            self.assertEqual(self.get()[0]["id"], ident)
            res = self.r(*args)
            self.assertEqual(res.returncode, 0, res.stderr)
        # wound/kill, the old aliases and anything else: no-ops, exit 0.
        for args in (["wound", ident], ["kill", ident], ["explode", ident],
                     ["respond", "delete", ident], ["poll"]):
            res = self.r(*args)
            self.assertEqual(res.returncode, 0, args)
            self.assertEqual(res.stdout, "", args)
        log = self.audit()
        self.assertEqual([(e["verb"], e.get("action"), e["outcome"])
                          for e in log],
                         [("post", "nudge", "ok"), ("delete", "remove", "ok"),
                          ("post", "nudge", "ok"), ("delete", "remove", "ok"),
                          ("wound", None, "ignored-verb"),
                          ("kill", None, "ignored-verb"),
                          ("explode", None, "ignored-verb"),
                          ("respond", None, "ignored-verb"),
                          ("poll", None, "ignored-verb")])
        acts = [c for c in self.calls() if c and c[0] in ("nudge", "remove")]
        self.assertEqual(acts, [["nudge", "a", "amount=2"], ["remove", "a"],
                                ["nudge", "a", "amount=1"], ["remove", "a"]])
        res = self.r("describe")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("verbs", json.loads(res.stdout))
        res = self.r("undo", "abc")           # the fake lists no restore
        self.assertEqual(res.returncode, 1)
        self.assertIn("restore", res.stderr)
        res = self.r("plugins")
        self.assertEqual(res.returncode, 0)
        self.assertIn("* fake", res.stdout)
        self.assertIn("  ps", res.stdout)
        res = self.r()
        self.assertEqual(res.returncode, 2)               # no command
        self.assertIn("usage", res.stderr)
        res = self.r("--contract", "v1", "get")           # no such switch
        self.assertEqual(res.returncode, 0)
        self.assertEqual(res.stdout, "")

    def dims_spec(self):
        return {"describe": {"verbs": ["list"], "default_sort": "size",
                             "dimensions": [
                                 {"id": "size", "kind": "numeric"},
                                 {"id": "team", "kind": "category"}]}}

    def test_request_reaches_plugin_and_header_is_printed(self):
        self.named("dim", [{"key": "a", "name": "A", "group": "red\tx"},
                           {"key": "b", "name": "B"}], spec=self.dims_spec())
        self.r("use", "dim")
        rows = self.wait_rows(words=("sort=size:asc", "group=team",
                                     "limit=5", "sort2=x"))
        lists = [c for c in self.calls() if c and c[0] == "list"]
        self.assertEqual(lists[-1], ["list", "sort=size:asc", "group=team",
                                     "limit=5"])
        res = self.r("get", "sort=size:asc", "group=team", "limit=5",
                     "sort2=x")
        head = [ln for ln in res.stdout.splitlines() if ln.startswith("#")]
        self.assertIn("# dim id=size kind=numeric dir=desc roles=sort "
                      "unit= label=size", head)
        self.assertIn("# sort id=size dir=asc", head)
        self.assertIn("# group id=team", head)
        self.assertTrue(any("ignored sort2=x" in h for h in head))
        # the plugin's order is kept, and a hostile group label stays inert
        self.assertEqual([r["label"] for r in rows], ["A", "B"])
        self.assertEqual(rows[0]["group"], "red x")
        self.assertNotIn("group", rows[1])

    def test_failing_plugin_backs_off_even_for_a_new_request(self):
        spec = self.dims_spec()
        spec["list_fail"] = True
        self.named("bad", [{"key": "a"}], spec=spec, cache_ttl_s=30)
        self.r("use", "bad")
        self.r("get", "sort=size")
        self._reap_refresher(self.configs[-1])
        t0 = time.monotonic()
        for w in ("sort=size:asc", "sort=size:desc", "limit=3"):
            res = self.r("get", w)
            self.assertEqual(res.returncode, 0)
        took = time.monotonic() - t0
        self._reap_refresher(self.configs[-1])
        lists = [c for c in self.calls() if c and c[0] == "list"]
        self.assertEqual(len(lists), 1)          # no respawn per poll
        self.assertLess(took, 3 * 0.7)           # and no 700 ms wait each

    def test_unknown_chain_fails_closed(self):
        from unittest import mock
        self.named("pids", [{"key": "p", "name": "Proc", "pid": 4242},
                            {"key": "row", "name": "Row"}])
        self.r("use", "pids")
        self.wait_rows()               # a cache exists
        cfg = H.load_named(self.hroot, "pids")
        with mock.patch.object(H, "ancestor_pids", return_value=None), \
                mock.patch.object(H, "spawn_refresher"), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            H.cmd_poll(cfg)
        labels = [r["label"] for r in parse_v1(out.getvalue())]
        self.assertEqual(labels, ["Row"])     # nothing with a pid

    def test_dirty_cache_does_not_rearm_a_failing_refresh(self):
        spec = self.dims_spec()
        spec["list_fail"] = True
        self.named("bad", [{"key": "a"}], spec=spec, cache_ttl_s=30)
        self.r("use", "bad")
        self.r("get")
        self._reap_refresher(self.configs[-1])
        state = os.path.join(self.hroot, "state", "bad")
        self.assertTrue(os.path.exists(os.path.join(state, "refresh.failed")))
        dirty = os.path.join(state, "dirty")
        open(dirty, "w").close()
        os.utime(dirty, (time.time() - 60, time.time() - 60))   # an old action
        for _ in range(3):
            self.r("get")
        self._reap_refresher(self.configs[-1])
        lists = [c for c in self.calls() if c and c[0] == "list"]
        self.assertEqual(len(lists), 1)

    def test_get_never_serves_its_own_calling_chain(self):
        # Items naming the test process (which runs `summon get`) are
        # dropped at serve time, even from a cache built for another caller.
        me = os.getpid()
        self.named("pids", [{"key": "me", "name": "Me", "pid": me},
                            {"key": "other", "name": "Other", "pid": 2},
                            {"key": "none", "name": "None"},
                            {"key": "bogus", "name": "Bogus", "pid": "x"}])
        self.r("use", "pids")
        rows = self.wait_rows()
        self.assertEqual([r["label"] for r in rows],
                         ["Other", "None", "Bogus"])
        self.assertTrue(any(i.get("key") == "me" for i in json.load(open(
            os.path.join(self.state, "cache.json")))["items"]))

    def test_plain_plugin_gets_plain_list(self):
        self.named("plain", [{"key": "a", "name": "A"}])
        self.r("use", "plain")
        self.wait_rows(words=("sort=size",))
        lists = [c for c in self.calls() if c and c[0] == "list"]
        self.assertTrue(lists and all(c == ["list"] for c in lists))

    def test_cache_is_keyed_by_request(self):
        # The fake lists in the order its spec says; a new request must not
        # be answered from the old request's cache.
        self.named("dim", [{"key": "a", "name": "A"},
                           {"key": "b", "name": "B"}], spec=self.dims_spec())
        self.r("use", "dim")
        self.wait_rows(words=("sort=size:desc",))
        before = len([c for c in self.calls() if c and c[0] == "list"])
        res = self.r("get", "sort=size:asc")      # different request
        lists = [c for c in self.calls() if c and c[0] == "list"]
        self.assertGreater(len(lists), before)
        self.assertEqual(lists[-1][1], "sort=size:asc")
        self.assertIn("# sort id=size dir=asc", res.stdout)
        # the same request again, within the TTL, is served from cache
        n = len(lists)
        self.r("get", "sort=size:asc")
        self._reap_refresher(self.configs[-1])
        lists = [c for c in self.calls() if c and c[0] == "list"]
        self.assertLessEqual(len(lists), n + 1)

    def _ranked(self, n=12):
        return [{"key": "k%02d" % i, "name": "P%02d" % i, "size": 100 - i}
                for i in range(n)]

    def test_lower_limit_returns_top_of_previous_list(self):
        # The game sends its monster count as limit=: lowering it from 10 to
        # 3 must list exactly the first 3 of the 10, in the same order.
        self.named("dim", self._ranked(), spec=self.dims_spec())
        self.r("use", "dim")
        ten = self.wait_rows(words=("limit=10",))
        self.assertEqual(len(ten), 10)
        three = self.get(words=("limit=3",))
        self.assertEqual([r["label"] for r in three],
                         [r["label"] for r in ten[:3]])
        self.assertEqual([r["id"] for r in three],
                         [r["id"] for r in ten[:3]])
        self._reap_refresher(self.configs[-1])

    def test_lower_limit_is_never_served_whole_from_the_old_entry(self):
        # The cache is keyed by the request. A lowered limit whose refresh
        # is slow must still list only the top 3 at once: the old entry
        # (made for limit=10, same sort) is cut, never served whole.
        self.named("dim", self._ranked(), spec=self.dims_spec())
        self.r("use", "dim")
        ten = self.wait_rows(words=("limit=10",))
        self._reap_refresher(self.configs[-1])
        spec = self.dims_spec()
        spec["list_sleep"] = 3
        self.named("dim", self._ranked(), spec=spec)   # the next list is slow
        started = time.monotonic()
        three = self.get(words=("limit=3",))
        elapsed = time.monotonic() - started
        self.assertEqual([r["label"] for r in three],
                         [r["label"] for r in ten[:3]])
        # Answered from the old entry, so it does not wait for the refresh.
        self.assertLess(elapsed, 0.6)
        self._reap_refresher(self.configs[-1])

    def test_changed_sort_is_not_answered_by_a_cut(self):
        # Only a lower limit with the same sort and group is answered by
        # the old entry's head; a new sort waits for its own refresh.
        req = lambda *w: H.canonical_request(list(w))[0]
        entry = {"request": H.request_key(req("sort=size:desc", "limit=10"))}
        self.assertTrue(H.answers_by_cut(entry, req("sort=size:desc", "limit=3")))
        self.assertFalse(H.answers_by_cut(entry, req("sort=size:asc", "limit=3")))
        self.assertFalse(H.answers_by_cut(entry, req("limit=3")))
        self.assertFalse(H.answers_by_cut(
            entry, req("sort=size:desc", "group=team", "limit=3")))
        self.assertFalse(H.answers_by_cut(entry, req("sort=size:desc", "limit=12")))
        self.assertFalse(H.answers_by_cut(entry, req("sort=size:desc")))
        whole = {"request": H.request_key(req("sort=size:desc"))}
        self.assertTrue(H.answers_by_cut(whole, req("sort=size:desc", "limit=3")))
        self.assertFalse(H.answers_by_cut({"request": "not json"}, req("limit=3")))
        self.assertFalse(H.answers_by_cut({}, req("limit=3")))

    def test_cut_is_made_after_the_callers_chain_is_dropped(self):
        # An entry item standing for the caller itself is dropped first, so a
        # cut entry still lists `limit` items.
        me = os.getpid()
        items = [{"key": "me", "name": "Me", "pid": me, "size": 200}] + \
            self._ranked()
        self.named("dim", items, spec=self.dims_spec())
        self.r("use", "dim")
        self.wait_rows(words=("limit=10",))
        self._reap_refresher(self.configs[-1])
        spec = self.dims_spec()
        spec["list_sleep"] = 3
        self.named("dim", items, spec=spec)
        three = self.get(words=("limit=3",))
        self.assertEqual([r["label"] for r in three], ["P00", "P01", "P02"])
        self._reap_refresher(self.configs[-1])

    def test_raised_limit_is_not_answered_from_a_smaller_entry(self):
        # A raised limit cannot be answered from an entry made for fewer
        # items: the refresh runs and its longer list is served.
        self.named("dim", self._ranked(), spec=self.dims_spec())
        self.r("use", "dim")
        self.wait_rows(words=("limit=3",))
        self._reap_refresher(self.configs[-1])
        deadline = time.monotonic() + 10
        rows = []
        while len(rows) != 10 and time.monotonic() < deadline:
            rows = self.get(words=("limit=10",))
            time.sleep(0.05)
        self.assertEqual([r["label"] for r in rows],
                         ["P%02d" % i for i in range(10)])
        self._reap_refresher(self.configs[-1])

    def test_get_carries_grade(self):
        self.named("fake", [{"key": "a", "name": "Top", "size": 9},
                            {"key": "b", "name": "Mid", "size": 5},
                            {"key": "c", "name": "Low", "size": 1}])
        self.r("use", "fake")
        self.r("refresh")
        res = self.r("get")
        self.assertFalse(res.stdout.startswith("#"))       # no header
        rows = {r["label"]: r for r in parse_v1(res.stdout)}
        self.assertEqual({k: float(v["grade"]) for k, v in rows.items()},
                         {"Top": 1.0, "Mid": 0.5, "Low": 0.0})
        for line in res.stdout.splitlines():
            self.assertTrue(line.split("\t")[-1].startswith("label="))
        res = self.r("--plugin", "nope", "get")
        self.assertTrue(res.stdout.startswith("# summon error"))
        self.assertEqual(parse_v1(res.stdout), [])

    def test_grade_source_order_and_max_tier(self):
        # load only (no sizes): the load decides, not the rank
        self.named("loads", [{"key": "a", "name": "Busy", "load": 90},
                             {"key": "b", "name": "Idle", "load": 5}])
        rows = {r["label"]: r for r in self.wait_rows("--plugin", "loads")}
        self.assertEqual(float(rows["Busy"]["grade"]), 1.0)
        self.assertEqual(float(rows["Idle"]["grade"]), 0.0)
        # native size + relative load: no computed grade at all
        self.named("nat", [{"key": "a", "name": "N1", "size": 5, "load": 90},
                           {"key": "b", "name": "N2", "size": 9, "load": 5}],
                   spec={"describe": {"verbs": ["list"],
                                      "size": {"scale": "native"},
                                      "load": {"scale": "relative"}}})
        res = self.wait_rows("--plugin", "nat")
        self.assertTrue(all("grade" not in r for r in res))
        # max_tier 5 (no Cyberdemon) scales every grade by 5.5 / 7
        self.named("capped", [{"key": "a", "name": "Big", "size": 9},
                              {"key": "b", "name": "Given", "grade": 1},
                              {"key": "c", "name": "Small", "size": 1}],
                   max_tier=5)
        rows = {r["label"]: r for r in self.wait_rows("--plugin", "capped")}
        self.assertAlmostEqual(float(rows["Big"]["grade"]), 5.5 / 7, places=3)
        self.assertAlmostEqual(float(rows["Given"]["grade"]), 5.5 / 7,
                               places=3)
        # the default (6) leaves grades alone
        self.named("open", [{"key": "a", "name": "Top", "grade": 1}])
        rows = self.wait_rows("--plugin", "open")
        self.assertEqual(float(rows[0]["grade"]), 1.0)

    def test_max_tier_never_exceeds_the_cap_on_any_wad(self):
        # psDoom's mapping (psdoom_grade.c), mirrored: tier over drawable.
        def pick(grade, drawable):
            idx = [i for i in range(7) if i == 0 or drawable[i]]
            n = len(idx)
            t = n - 1 if grade >= 1 else min(int(grade * n), n - 1)
            return idx[t]

        def capped_at(cap_tier, drawable):
            # what the v1 ladder's cap degrades to on this WAD
            return max(i for i in range(cap_tier + 1)
                       if i == 0 or drawable[i])

        wads = {"full": [1] * 7,
                "no-knight": [1, 1, 1, 1, 0, 1, 1],
                "shareware": [1, 1, 1, 1, 0, 1, 0]}
        for name, drawable in wads.items():
            for max_tier in range(7):
                cap = 1.0 if max_tier >= 6 else (max_tier + 0.5) / 7.0
                top = pick(round(1.0 * cap, 4), drawable)
                self.assertLessEqual(top, capped_at(max_tier, drawable),
                                     (name, max_tier))
                if name == "full":
                    self.assertEqual(top, max_tier, max_tier)   # exact

    def test_root_id_map_is_shared_and_fails_closed(self):
        self.named("a", [{"key": "x", "name": "alpha"}])
        self.named("b", [{"key": "x", "name": "bravo"}])   # same key
        ida = self.wait_rows("--plugin", "a")[0]["id"]
        idb = self.wait_rows("--plugin", "b")[0]["id"]
        self.assertNotEqual(ida, idb)
        with open(os.path.join(self.hroot, "ids.json")) as f:
            keys = json.load(f)["keys"]
        self.assertEqual(sorted(keys), ["a/x", "b/x"])
        # no per-plugin maps for named plugins
        self.assertFalse(os.path.exists(
            os.path.join(self.hroot, "state", "a", "ids.json")))
        # losing the root map while any plugin has listed: refused
        os.unlink(os.path.join(self.hroot, "ids.json"))
        res = self.r("--plugin", "a", "refresh")
        self.assertEqual(res.returncode, 1)
        self.assertIn("ids could be reused", res.stderr)
        res = self.r("--plugin", "b", "refresh")
        self.assertEqual(res.returncode, 1)

    def test_namespace_is_the_file_name_not_plugin_name(self):
        # Two configs copied from one example keep "name": "csv" inside.
        self.named("alpha", [{"key": "row1", "name": "A row"}],
                   plugin={"name": "shared"})
        self.named("beta", [{"key": "row1", "name": "B row"}],
                   plugin={"name": "shared"})
        ida = self.wait_rows("--plugin", "alpha")[0]["id"]
        idb = self.wait_rows("--plugin", "beta")[0]["id"]
        self.assertNotEqual(ida, idb)
        with open(os.path.join(self.hroot, "ids.json")) as f:
            self.assertEqual(sorted(json.load(f)["keys"]),
                             ["alpha/row1", "beta/row1"])

    def test_named_plugin_may_not_move_its_state(self):
        path = self.named("gamma", [{"key": "a"}],
                          state_dir=os.path.join(self.root, "elsewhere"))
        self.assertTrue(path.endswith("gamma.json"))
        res = self.r("--plugin", "gamma", "refresh")
        self.assertEqual(res.returncode, 1)
        self.assertIn("remove state_dir", res.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.root, "elsewhere")))

    def test_old_counter_file_is_never_gone_below(self):
        with open(os.path.join(self.hroot, "ids-next.json"), "w") as f:
            json.dump({"next": 500}, f)
        self.named("a", [{"key": "x"}])
        self.assertEqual(self.wait_rows("--plugin", "a")[0]["id"], "500")

    def test_switching_plugins_never_reuses_a_held_id(self):
        self.named("a", [{"key": "x", "name": "alpha"}])
        self.named("b", [{"key": "x1", "name": "important"}], dry_run=False,
                   plugin={"allow_remove": True})
        self.r("use", "a")
        held = self.wait_rows()[0]["id"]
        self.r("use", "b")
        self.state = os.path.join(self.hroot, "state", "b")
        rows = self.wait_rows()
        self.assertNotEqual(rows[0]["id"], held)          # one counter per root
        # the game still holds `held` for "alpha" and hits it: nothing acts
        self.assertEqual(self.r("delete", held, "label=alpha").returncode, 0)
        self.assertEqual([e["outcome"] for e in self.audit()], ["unknown-id"])
        # even an id that does map, with another item's label, is refused
        self.assertEqual(self.r("delete", rows[0]["id"],
                                "label=alpha").returncode, 0)
        self.assertEqual(self.audit()[-1]["outcome"], "label-mismatch")
        self.assertEqual([c for c in self.calls() if c and c[0] == "remove"],
                         [])

    def test_loose_umask_still_makes_private_dirs(self):
        root = os.path.join(self.root, "umaskroot")
        res = subprocess.run(
            ["/bin/sh", "-c", 'umask 002; exec "$0" "$@"', PY, SUMMON_CMD,
             "--root", root, "get"],
            capture_output=True, text=True, env=self.env(), cwd=self.root)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.configs.append(self._ps_reap_at(root))
        for d in (root, os.path.join(root, "state"),
                  os.path.join(root, "state", "ps")):
            self.assertEqual(os.stat(d).st_mode & 0o777, 0o700, d)
        res = self.cli("--root", root, "get")
        self.assertNotIn("summon error", res.stdout)

    def _ps_reap_at(self, root):
        path = os.path.join(self.root, "reap-%d.json" % len(self.configs))
        with open(path, "w") as f:
            json.dump({"state_dir": os.path.join(root, "state", "ps")}, f)
        return path

    def test_config_symlink_and_loose_plugins_dir_refused(self):
        real = self.named("fake", [{"key": "a"}])
        link = os.path.join(self.hroot, "plugins", "linked.json")
        os.symlink(real, link)
        res = self.r("--plugin", "linked", "refresh")
        self.assertEqual(res.returncode, 1)
        self.assertIn("symlink", res.stderr)
        os.chmod(os.path.join(self.hroot, "plugins"), 0o777)
        res = self.r("--plugin", "fake", "refresh")
        self.assertEqual(res.returncode, 1)
        self.assertIn("plugins directory is writable", res.stderr)
        os.chmod(os.path.join(self.hroot, "plugins"), 0o755)

    def test_plugin_grade_wins_and_is_clamped(self):
        self.named("fake", [{"key": "a", "name": "Given", "grade": 0.3,
                             "size": 999},
                            {"key": "b", "name": "Over", "grade": 7},
                            {"key": "c", "name": "Under", "grade": -2},
                            {"key": "d", "name": "Junk", "grade": "x",
                             "size": 1}])
        self.r("use", "fake")
        self.r("refresh")
        rows = {r["label"]: r for r in self.get()}
        self.assertEqual(float(rows["Given"]["grade"]), 0.3)
        self.assertEqual(float(rows["Over"]["grade"]), 1.0)
        self.assertEqual(float(rows["Under"]["grade"]), 0.0)
        # a junk grade falls back to the size-derived one
        self.assertIn(float(rows["Junk"]["grade"]), (0.0,))
        # weight/load follow the grade when the plugin sent no size/load
        self.assertEqual(int(rows["Over"]["weight"]), H.SIZE_LADDER[6])
        self.assertEqual(int(rows["Over"]["load"]), H.LOAD_LADDER[6])
        self.assertEqual(int(rows["Under"]["weight"]), 0)

    def test_native_scale_gets_no_grade(self):
        self.named("fake", [{"key": "a", "name": "N", "size": 70000000,
                             "load": 30}],
                   spec={"describe": {"verbs": ["list"],
                                      "size": {"scale": "native"},
                                      "load": {"scale": "native"}}})
        self.r("use", "fake")
        self.r("refresh")
        res = self.r("get")
        self.assertNotIn("grade=", res.stdout)
        row = parse_v1(res.stdout)[0]
        self.assertEqual((row["weight"], row["load"]), ("70000000", "30"))

    def test_post_and_delete_are_dry_run_by_default(self):
        self.named("fake", [{"key": "a", "name": "A"}],
                   plugin={"allow_remove": True})
        rows = self.wait_rows("--plugin", "fake")
        self.r("--plugin", "fake", "delete", rows[0]["id"])
        self.r("--plugin", "fake", "post", rows[0]["id"])
        self.assertEqual([e["outcome"] for e in self.audit()],
                         ["dry-run", "dry-run"])
        self.assertEqual([c for c in self.calls()
                          if c and c[0] in ("nudge", "remove")], [])

    def test_use_persists_across_calls(self):
        self.named("one", [{"key": "a", "name": "From one"}])
        self.named("two", [{"key": "b", "name": "From two"}])
        self.assertEqual(self.r("use", "two").returncode, 0)
        with open(os.path.join(self.hroot, "current")) as f:
            self.assertEqual(f.read().strip(), "two")
        self.state = os.path.join(self.hroot, "state", "two")
        self.assertEqual([r["label"] for r in self.wait_rows()], ["From two"])
        self.assertIn("* two", self.r("plugins").stdout)
        self.assertEqual(self.r("use", "one").returncode, 0)
        self.assertEqual([r["label"] for r in self.wait_rows()], ["From one"])

    def test_default_plugin_is_ps(self):
        res = self.r("plugins")
        self.assertEqual(res.returncode, 0)
        self.assertIn("* ps", res.stdout)
        cfg = H.load_named(self.hroot, "ps")
        self.assertFalse(cfg["dry_run"])          # the host's mode governs
        self.assertTrue(cfg["plugin"]["allow_remove"])
        self.assertEqual(cfg["state_dir"],
                         os.path.join(self.hroot, "state", "ps"))
        self.assertEqual(cfg["plugin"]["command"][-1], PROC_PLUGIN)
        self.configs.append(self._ps_reap_config())
        rows = self.wait_rows()                           # read-only listing
        self.assertGreater(len(rows), 0)

    def _ps_reap_config(self):
        path = os.path.join(self.root, "ps-reap.json")
        with open(path, "w") as f:
            json.dump({"state_dir": os.path.join(self.hroot, "state", "ps")},
                      f)
        return path

    def test_plugin_and_config_precedence(self):
        self.named("cur", [{"key": "c", "name": "Current"}])
        self.named("other", [{"key": "o", "name": "Other"}])
        self.r("use", "cur")
        filecfg = self.config(self.fake([{"key": "f", "name": "File"}],
                                        spec_name="file"),
                              state_dir=os.path.join(self.root, "filestate"))
        self.assertEqual([r["label"] for r in self.wait_rows()], ["Current"])
        self.assertEqual([r["label"] for r in self.wait_rows(
            "--plugin", "other")], ["Other"])
        self.assertEqual([r["label"] for r in self.wait_rows(
            "--config", filecfg)], ["File"])
        res = self.r("--plugin", "other", "--config", filecfg, "get")
        self.assertEqual(res.returncode, 2)
        self.assertIn("not both", res.stderr)
        # the overrides did not change the persisted choice
        self.assertIn("* cur", self.r("plugins").stdout)

    def test_unknown_plugin_fails_cleanly(self):
        res = self.r("use", "nope")
        self.assertEqual(res.returncode, 1)
        self.assertIn("unknown plugin 'nope'", res.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.hroot, "current")))
        res = self.r("--plugin", "nope", "get")
        self.assertEqual(res.returncode, 0)                # never break poll
        self.assertTrue(res.stdout.startswith("# summon error"))
        self.assertIn("unknown plugin", res.stdout)
        self.assertEqual(parse_v1(res.stdout), [])
        self.assertEqual(self.r("--plugin", "nope", "delete", "5").returncode,
                         0)
        for bad in ("../x", "A", "x/y", "", "-rf", "a" * 40):
            res = self.r("use", bad)
            self.assertEqual(res.returncode, 1, bad)
        self.assertFalse(os.path.exists(os.path.join(self.root, "x.json")))

    def test_root_resolution(self):
        envroot = os.path.join(self.root, "envroot")
        fakehome = os.path.join(self.root, "fakehome")
        # neither --root nor the variable: ~/.config/summoner (fake home)
        self.assertEqual(self.cli("use", "ps").returncode, 0)
        self.assertTrue(os.path.exists(os.path.join(
            fakehome, ".config", "summoner", "current")))
        # the variable
        self.assertEqual(self.cli("use", "ps", env=self.env(
            SUMMONER_ROOT=envroot)).returncode, 0)
        self.assertTrue(os.path.exists(os.path.join(envroot, "current")))
        # --root beats the variable
        other = os.path.join(self.root, "optroot")
        self.assertEqual(self.cli("--root", other, "use", "ps", env=self.env(
            SUMMONER_ROOT=envroot)).returncode, 0)
        self.assertTrue(os.path.exists(os.path.join(other, "current")))
        res = self.cli("--root", "relative/root", "use", "ps")
        self.assertEqual(res.returncode, 1)
        self.assertIn("absolute", res.stderr)

    def test_root_and_configs_must_be_private(self):
        path = self.named("fake", [{"key": "a"}])
        os.chmod(path, 0o666)
        res = self.r("--plugin", "fake", "refresh")
        self.assertEqual(res.returncode, 1)
        self.assertIn("writable by group or others", res.stderr)
        os.chmod(path, 0o644)
        os.chmod(self.hroot, 0o777)
        res = self.r("--plugin", "fake", "refresh")
        self.assertEqual(res.returncode, 1)
        self.assertIn("root is writable", res.stderr)
        os.chmod(self.hroot, 0o755)

    def test_launcher_works_through_a_symlink(self):
        self.named("fake", [{"key": "a", "name": "Linked"}])
        bindir = os.path.join(self.root, "bin")
        os.makedirs(bindir)
        link = os.path.join(bindir, "summon")
        os.symlink(SUMMON_CMD, link)
        self.assertTrue(os.access(SUMMON_CMD, os.X_OK))
        deadline = time.monotonic() + 10
        rows = []
        while not rows and time.monotonic() < deadline:
            res = self.cli("--root", self.hroot, "--plugin", "fake", "get",
                           exe=link)
            self.assertEqual(res.returncode, 0, res.stderr)
            rows = parse_v1(res.stdout)
            time.sleep(0.1)
        self.assertEqual([r["label"] for r in rows], ["Linked"])


class ReviewFixTests(Base):
    """Regression checks for the adversarial review's findings."""

    def test_duplicate_csv_key_lists_and_acts_on_the_same_row(self):
        path = self.csv_file("id,name,priority\n"
                             "r1,FirstRow,1\nr1,SecondRow,9\nr2,Other,5\n")
        cfg = self.config(self.csv_cmd(path, "--priority-column", "priority"),
                          name="csv", dry_run=False,
                          plugin={"allow_remove": True})
        self.refresh(cfg)
        rows = self.poll(cfg)
        self.assertNotIn("SecondRow", [r["label"] for r in rows])
        self.summon(cfg, "delete", self.id_for(rows, "FirstRow"))
        text = self.read(path)
        self.assertNotIn("FirstRow", text)
        self.assertIn("SecondRow", text)

    def test_lost_id_map_fails_closed(self):
        cfg = self.config(self.fake([{"key": "a"}]))
        self.refresh(cfg)
        os.unlink(os.path.join(self.state, "ids.json"))
        r, _ = self.summon(cfg, "refresh")
        self.assertEqual(r.returncode, 1)
        self.assertIn("ids.json", r.stderr)
        with open(os.path.join(self.state, "ids.json"), "w") as f:
            f.write("{not json")
        r, _ = self.summon(cfg, "refresh")
        self.assertEqual(r.returncode, 1)

    def test_state_dir_must_be_private(self):
        os.makedirs(self.state, mode=0o700)
        os.chmod(self.state, 0o777)
        cfg = self.config(self.fake([{"key": "a"}]))
        r, _ = self.summon(cfg, "refresh")
        self.assertEqual(r.returncode, 1)
        self.assertIn("writable by group or others", r.stderr)
        os.chmod(self.state, 0o700)
        link = os.path.join(self.root, "link-state")
        os.symlink(self.state, link)
        with open(cfg) as f:
            c = json.load(f)
        c["state_dir"] = link
        with open(cfg, "w") as f:
            json.dump(c, f)
        r, _ = self.summon(cfg, "refresh")
        self.assertEqual(r.returncode, 1)
        self.assertIn("symlink", r.stderr)

    def test_csv_mode_symlink_and_endings_kept(self):
        path = self.csv_file('id,name\na,A\nb,"two\r\nlines"\nc,C')
        os.chmod(path, 0o600)
        link = os.path.join(self.root, "link.csv")
        os.symlink(path, link)
        r = subprocess.run(self.csv_cmd(link, "remove", "a"),
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(os.path.islink(link))
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        self.assertEqual(self.read(path), 'id,name\nb,"two\r\nlines"\nc,C')

    def test_odd_ids_and_bad_rate_state_are_audited(self):
        cfg = self.config(self.fake([{"key": "a"}]), dry_run=False)
        self.refresh(cfg)
        self.summon(cfg, "delete", "²")
        with open(os.path.join(self.state, "rate.json"), "w") as f:
            json.dump(["x", None, 1], f)
        self.summon(cfg, "post", self.poll(cfg)[0]["id"])
        self.assertEqual([e["outcome"] for e in self.audit()],
                         ["unknown-id", "ok"])

    def test_non_object_cache_recovers(self):
        cfg = self.config(self.fake([{"key": "a", "name": "A"}]),
                          cache_ttl_s=0)
        self.refresh(cfg)
        with open(os.path.join(self.state, "cache.json"), "w") as f:
            f.write("[]")
        deadline = time.monotonic() + 10
        rows = []
        while not rows and time.monotonic() < deadline:
            rows = self.poll(cfg)
            time.sleep(0.1)
        self.assertEqual([r["label"] for r in rows], ["A"])

    def test_undo_needs_restore_verb(self):
        cfg = self.config(self.fake([{"key": "a"}]))
        self.refresh(cfg)
        r, _ = self.summon(cfg, "undo", "abc")
        self.assertEqual(r.returncode, 1)
        self.assertIn("restore", r.stderr)


class HostileTests(Base):

    HOSTILE = [
        "evil\tid=99\tweight=4294967296",
        "line one\nid=100\tlabel=forged",
        "$(touch PWNED_SUBST)",
        "`touch PWNED_TICK`",
        "; touch PWNED_SEMI; rm -rf ~",
        "#!psdoom header-looking name",
        "\x1b[2J\x07bell",
    ]

    def assert_no_pwned(self):
        for d in (self.root, SCRATCH, os.getcwd()):
            for name in os.listdir(d):
                self.assertNotIn("PWNED", name, d)

    def test_hostile_names_stay_inside_their_label(self):
        items = [{"key": "k%d" % i, "name": n}
                 for i, n in enumerate(self.HOSTILE)]
        cfg = self.config(self.fake(items))
        self.refresh(cfg)
        r, _ = self.summon(cfg, "poll")
        lines = [ln for ln in r.stdout.split("\n")
                 if ln and not ln.startswith("#")]
        self.assertEqual(len(lines), len(self.HOSTILE))   # no forged records
        for ln in lines:
            fields = ln.split("\t")
            keys = [f.split("=", 1)[0] for f in fields]
            self.assertEqual([k for k in keys if k != "grade"],
                             ["id", "parent", "weight", "load", "flags",
                              "label"])
            self.assertEqual(keys[-1], "label")
            self.assertTrue(all(0x20 <= ord(c) < 0x7F for c in ln
                                if c != "\t"))
        ids = [rec["id"] for rec in parse_v1(r.stdout)]
        self.assertNotIn("99", ids)
        self.assertNotIn("100", ids)
        self.assert_no_pwned()

    def test_hostile_keys_and_labels_inert_on_respond(self):
        rows = ["id,name"]
        keys = ["$(touch PWNED_K1)", "; touch PWNED_K2", "`touch PWNED_K3`",
                "a\"b'c"]
        for i, k in enumerate(keys):
            esc = k.replace('"', '""')
            rows.append('"%s",n%d' % (esc, i))
        path = self.csv_file("\n".join(rows) + "\n")
        cfg = self.config(self.csv_cmd(path), name="csv", dry_run=False,
                          plugin={"allow_remove": True})
        self.refresh(cfg)
        rows = self.poll(cfg)
        for rec in rows:
            # A label that is not the item's: refused, nothing happens.
            self.summon(cfg, "delete", rec["id"],
                         "label=$(touch PWNED_L)", "amount=`touch PWNED_A`")
        self.assertEqual({e["outcome"] for e in self.audit()},
                         {"label-mismatch"})
        self.assertEqual(self.read(path).count("\n"), len(keys) + 1)
        for rec in rows:
            self.summon(cfg, "delete", rec["id"], "label=" + rec["label"],
                         "amount=`touch PWNED_A`")
        # every row removed by its exact key, nothing executed
        self.assertEqual(self.read(path), "id,name\n")
        self.assertEqual(sorted(e["key"] for e in self.audit()
                                if e["outcome"] == "ok"), sorted(keys))
        self.assert_no_pwned()

    def test_audit_lines_stay_one_per_entry(self):
        cfg = self.config(self.fake([{"key": "k\nforged", "name": "a\nb"}]),
                          dry_run=True)
        self.refresh(cfg)
        self.summon(cfg, "delete", self.poll(cfg)[0]["id"],
                     "label=x\ny")
        with open(os.path.join(self.state, "audit.jsonl")) as f:
            text = f.read()
        self.assertEqual(text.count("\n"), 1)
        self.assertEqual(json.loads(text)["key"], "k\nforged")


class ProcessesPluginTests(Base):
    """Read-only and dry-run only: nothing here signals or renices."""

    PS_SAMPLE = (
        "  501     1 501 501  10240   0.0 ??       Thu Nov 12 09:01:02 2026 "
        "/usr/sbin/cfprefsd\n"
        "  777   501 501 501  20480  12.6 ttys001  Thu Nov 12 09:05:00 2026 "
        "/Applications/My App.app/Contents/MacOS/My App\n"
        "  888     1 501 501    100   0.0 ??       Thu Nov 12 09:00:00 2026 "
        "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock\n"
        "    1     0 501 501    100   0.0 ??       Thu Nov 12 08:00:00 2026 "
        "/sbin/launchd\n"
        "  999     1 501 501    100   0.0 ??       Thu Nov 12 09:06:00 2026 "
        "/bin/self\n"
        "garbage line\n")

    def test_parse_and_items(self):
        rows = P.parse_ps(self.PS_SAMPLE, self_pid=999, uid=501)
        self.assertEqual([r["pid"] for r in rows], [501, 777])  # deny, self
        its = P.items(rows)
        self.assertEqual(its[0]["key"], "501@Thu-Nov-12-09:01:02-2026")
        self.assertEqual(its[1]["name"], "My App")
        self.assertEqual(its[1]["size"], 20480 * 1024)
        self.assertEqual(its[1]["load"], 13)
        self.assertEqual(its[1]["parent"], its[0]["key"])
        self.assertTrue(its[0]["background"])
        self.assertFalse(its[1]["background"])

    def ps_rows(self, specs):
        rows = []
        for pid, name, kib, tty, cpu in specs:
            rows.append({"pid": pid, "ppid": 1, "rss": kib, "cpu": cpu,
                         "tty": tty, "start": "s%d" % pid, "name": name,
                         "started": 0})
        return rows

    def test_excludes_protected_own_chain_and_host(self):
        text = (
            "  300     1 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /Apps/Term\n"
            "  301   300 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /bin/zsh\n"
            "  302   301 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /Apps/psDoom\n"
            "  303   302 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /bin/sh\n"
            "  400     1 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /py/summon\n"
            "  401   400 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /py/plugin\n"
            "  500     1 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /Apps/Editor\n"
            "  501     1 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 "
            "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock\n"
            "  502     1 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 "
            "/System/Library/CoreServices/loginwindow\n"
            "    1     0 501 501  100 0.0 ?? Thu Nov 12 09:00:00 2026 /sbin/launchd\n")
        host = P.host_pids({"SUMMONER_HOST_PIDS": "303,302,301,300,x,1,0"})
        self.assertEqual(host, {300, 301, 302, 303})
        rows = P.parse_ps(text, self_pid=401, exclude=host, uid=501)
        self.assertEqual([r["name"] for r in rows], ["Editor"])
        # without the host chain, only the plugin's own chain is left out
        rows = P.parse_ps(text, self_pid=401, uid=501)
        self.assertNotIn("plugin", [r["name"] for r in rows])
        self.assertNotIn("summon", [r["name"] for r in rows])
        # the launchers are left out only by the host chain; psDoom's own
        # name is on the deny list either way
        self.assertIn("Term", [r["name"] for r in rows])
        self.assertNotIn("psDoom", [r["name"] for r in rows])

    def test_summoner_passes_the_host_chain_to_the_plugin(self):
        out = os.path.join(self.root, "env.txt")
        plugin = os.path.join(self.root, "envplug.py")
        with open(plugin, "w") as f:
            f.write("import json, os, sys\n"
                    "if sys.argv[1] == 'describe':\n"
                    "    print(json.dumps({'verbs': ['list']}))\n"
                    "else:\n"
                    "    open(%r, 'a').write(os.environ.get("
                    "'SUMMONER_HOST_PIDS', '') + '\\n')\n" % out)
        cfg = self.config([PY, plugin])
        self.summon(cfg, "poll")
        self._reap_refresher(cfg)
        with open(out) as f:
            pids = {int(x) for x in f.read().split()[-1].split(",")}
        # the test process ran `summon get`: it is in the host chain
        self.assertIn(os.getpid(), pids)

    def test_relevance_matches_psdooms_score(self):
        mem = 64 * 1024        # KiB: 64 MiB -> +2
        rows = self.ps_rows([(10, "Real App", mem, "??", 0),
                             (11, "com.apple.WebKit", mem, "??", 0),
                             (12, "Term Tool", mem, "s001", 0)])
        vals = {r["name"]: P.relevance(r) for r in rows}
        self.assertEqual(vals["Real App"], 100 + 2)
        self.assertEqual(vals["com.apple.WebKit"], 100 - 80 + 2)
        self.assertEqual(vals["Term Tool"], 100 + 50 + 2)
        big = self.ps_rows([(13, "Hog", 64 * 1024 * 1024, "??", 0)])[0]
        self.assertEqual(P.relevance(big), 100 + 120)        # capped

    def test_noise_and_duplicates_rank_below_a_real_app(self):
        mem = 512 * 1024
        rows = self.ps_rows([(20, "Chrome", mem, "??", 1),
                             (21, "Chrome", mem, "??", 1),
                             (22, "Chrome", mem, "??", 1),
                             (23, "Chrome", mem, "??", 1),
                             (24, "Chrome", mem, "??", 1),
                             (25, "Some Helper", mem, "??", 1),
                             (26, "Editor", mem, "??", 1)])
        its = P.items(rows, now=0)
        for sort in ([("relevance", "desc")], [("mem", "desc")],
                     [("cpu", "desc")]):
            out = [i["name"] for i in P.curate(its, sort)]
            # equal memory: the Editor (newest pid) and the first Chrome
            # lead; the helper and the extra Chromes sit below; beyond 3
            # Chromes are dropped
            self.assertEqual(out[:2], ["Editor", "Chrome"], sort)
            self.assertEqual(out.count("Chrome"), 3, sort)
            self.assertLess(out.index("Editor"), out.index("Some Helper"))
        out = P.curate(its, [("relevance", "desc")])
        self.assertEqual([i["name"] for i in out][-1], "Some Helper")
        self.assertEqual(out[0]["grade"], 1.0)

    def test_group_and_limit(self):
        rows = self.ps_rows([(30, "A", 100, "??", 9), (31, "B", 100, "??", 5),
                             (32, "A", 100, "??", 1)])
        out = P.curate(P.items(rows, now=0), [("cpu", "desc")], "app", 2)
        # groups are gathered, then the limit cuts: both A's, no B
        self.assertEqual([(i["name"], i["group"]) for i in out],
                         [("A", "A"), ("A", "A")])
        sort, group, limit = P.parse_request(["sort=cpu,bogus:up,mem:asc",
                                              "group=app", "limit=3",
                                              "x=y"])
        self.assertEqual(sort, [("cpu", "desc"), ("mem", "asc")])
        self.assertEqual((group, limit), ("app", 3))
        self.assertEqual(P.parse_request([])[0], [("relevance", "desc")])

    def test_describe_is_native_scale_and_not_undoable(self):
        d = H.validate_describe(P.describe())
        self.assertEqual(d["size"]["scale"], "native")
        self.assertFalse(d["remove"]["undoable"])

    def test_actions_refuse_the_calling_chain(self):
        # In-process, with resolve, kill and setpriority replaced: nothing
        # real is ever signalled or reniced here.
        from unittest import mock
        host = 424242
        fired = []
        with mock.patch.object(P, "resolve", return_value=host), \
                mock.patch.object(P.os, "kill",
                                  side_effect=lambda *a: fired.append(a)), \
                mock.patch.object(P.os, "setpriority",
                                  side_effect=lambda *a: fired.append(a)), \
                mock.patch.object(P.os, "getpriority", return_value=0), \
                mock.patch.object(P, "parent_map", return_value={}), \
                mock.patch.dict(P.os.environ,
                                {"SUMMONER_HOST_PIDS": "%d,77" % host}), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(P.main(["remove", "k"]), 1)
            self.assertEqual(P.main(["nudge", "k", "amount=4"]), 1)
            self.assertEqual(fired, [])
            self.assertIn("calling chain", out.getvalue())
            # a pid outside the chain goes through (to the stubs)
            with mock.patch.object(P, "resolve", return_value=55):
                self.assertEqual(P.main(["remove", "k"]), 0)
            self.assertEqual(fired, [(55, P.signal.SIGTERM)])

    def test_resolve_rechecks_the_exclusions_at_action_time(self):
        # ps is replaced in-process: resolve reads only the fake row.
        from unittest import mock
        me = os.getuid()
        row = ("  4242   1 %d %d  100   0.0 ??       Thu Nov 12 09:01:02 2026 "
               "%s\n")
        key = "4242@Thu-Nov-12-09:01:02-2026"
        cases = [
            (row % (me, me, "/Applications/Editor.app/Contents/MacOS/Editor"),
             {}, 4242),
            (row % (me, me, "/usr/bin/login"), {}, None),       # protected
            (row % (me, me, "/System/Library/CoreServices/Dock.app/"
                    "Contents/MacOS/Dock"), {}, None),
            (row % (0, me, "/usr/bin/Editor"), {}, None),       # euid
            (row % (me, 0, "/usr/bin/Editor"), {}, None),       # ruid
            ((row % (me, me, "/usr/bin/Editor")).replace("09:01:02",
                                                         "09:01:03"),
             {}, None),                                          # reused
            (row % (me, me, "/usr/bin/Editor"),
             {"SUMMONER_HOST_PIDS": "4242"}, None),               # host
            ("", {}, None),                                       # gone
        ]
        for text, env, want in cases:
            with mock.patch.object(P, "ps", return_value=text), \
                    mock.patch.dict(P.os.environ, env):
                self.assertEqual(P.resolve(key), want, (text, env))

    def test_malformed_key_never_resolves(self):
        for key in ("notapid@x", "1@x", "0@x", "12", "@", ""):
            self.assertIsNone(P.resolve(key))

    def test_list_through_summoner_is_on_the_ladder(self):
        cfg = self.config([PY, PROC_PLUGIN], name="processes")
        self.refresh(cfg)
        rows = self.poll(cfg)
        self.assertGreater(len(rows), 0)
        for r in rows:
            self.assertLessEqual(len(r["label"].encode()), 31)
            self.assertGreaterEqual(int(r["id"]), 2)
        self.assertNotIn("launchd", [r["label"] for r in rows])

    def test_actions_are_dry_run_only(self):
        # The real plugin lists; then the config's command is swapped for a
        # tripwire that only records calls, so even a broken dry-run could
        # not reach the real processes plugin's nudge or remove.
        cfg = self.config([PY, PROC_PLUGIN], name="processes",
                          plugin={"allow_remove": True})   # dry_run default
        self.refresh(cfg)
        rows = self.poll(cfg)
        self.assertGreater(len(rows), 0)
        with open(cfg) as f:
            c = json.load(f)
        c["plugin"]["command"] = self.fake([])
        with open(cfg, "w") as f:
            json.dump(c, f)
        for r in rows[:3]:
            self.summon(cfg, "delete", r["id"])
            self.summon(cfg, "post", r["id"], "amount=4")
        outcomes = [e["outcome"] for e in self.audit()]
        self.assertEqual(set(outcomes), {"dry-run"})
        self.assertEqual(len(outcomes), 2 * min(3, len(rows)))
        self.assertEqual(self.calls(), [])


class BuiltinPsTests(Base):
    """The built-in ps follows the host's mode with no config file, and a
    <root>/plugins/ps.json can only restrict it. The real processes plugin
    never acts here: its command is swapped for a fake, in this process,
    before any post or delete."""

    def setUp(self):
        super().setUp()
        self.hroot = os.path.join(self.root, "hroot")
        os.makedirs(os.path.join(self.hroot, "plugins"))

    def ps_with_fake(self, items, over=None):
        if over is not None:
            path = os.path.join(self.hroot, "plugins", "ps.json")
            with open(path, "w") as f:
                json.dump(over, f)
        cfg = H.load_named(self.hroot, "ps")
        self.assertEqual(cfg["plugin"]["command"][-1], PROC_PLUGIN)
        cfg["plugin"]["command"] = self.fake(items)   # never the real one
        self.state = cfg["state_dir"]
        H.ensure_state_dir(cfg)
        H.refresh(cfg)
        cache = H.read_cache(cfg)
        return cfg, {it["key"]: it for it in cache["items"]}

    def act(self, cfg, item, mode):
        lab = "label=%s" % item["label"]
        for verb in ("post", "delete"):
            args = [verb, str(item["id"]), lab]
            if mode is not None:
                args.append("mode=" + mode)
            H._respond(cfg, args)

    def acted(self):
        return [c[0] for c in self.calls() if c and c[0] in ("nudge",
                                                            "remove")]

    def test_no_config_the_mode_governs(self):
        cfg, items = self.ps_with_fake([{"key": "a", "name": "A"}])
        a = items["a"]
        self.act(cfg, a, None)            # missing mode: dryrun
        self.act(cfg, a, "dryrun")
        self.assertEqual(self.acted(), [])
        self.act(cfg, a, "post-only")
        self.assertEqual(self.acted(), ["nudge"])
        self.act(cfg, a, "delete")
        self.assertEqual(self.acted(), ["nudge", "nudge", "remove"])
        outcomes = [(e["verb"], e["outcome"], e.get("reason"))
                    for e in self.audit()]
        self.assertEqual(outcomes, [
            ("post", "dry-run", "host mode"),
            ("delete", "dry-run", "host mode"),
            ("post", "dry-run", "host mode"),
            ("delete", "dry-run", "host mode"),
            ("post", "ok", None),
            ("delete", "blocked", "host mode"),
            ("post", "ok", None),
            ("delete", "ok", None)])

    def test_a_config_can_only_restrict(self):
        def load(over):
            path = os.path.join(self.hroot, "plugins", "ps.json")
            with open(path, "w") as f:
                json.dump(over, f)
            return H.load_named(self.hroot, "ps")
        # Asking for what is already allowed changes nothing.
        cfg = load({"dry_run": False, "plugin": {"allow_remove": True}})
        self.assertFalse(cfg["dry_run"])
        self.assertTrue(cfg["plugin"]["allow_remove"])
        # Each restriction holds.
        self.assertTrue(load({"dry_run": True})["dry_run"])
        cfg = load({"plugin": {"allow_remove": False}})
        self.assertFalse(cfg["plugin"]["allow_remove"])
        self.assertFalse(cfg["dry_run"])
        # The rate limit only tightens.
        rl = load({"rate_limit": {"actions": 1000, "per_s": 1}})["rate_limit"]
        self.assertEqual(rl, H.DEFAULTS["rate_limit"])
        rl = load({"rate_limit": {"actions": 2, "per_s": 600}})["rate_limit"]
        self.assertEqual(rl, {"actions": 2, "per_s": 600})
        # Other keys pass through.
        self.assertEqual(load({"max_items": 5})["max_items"], 5)
        # The command cannot be changed, nor the name or the state dir.
        for bad in ({"plugin": {"command": [PY, CSV_PLUGIN]}},
                    {"plugin": {"name": "other"}},
                    {"state_dir": os.path.join(self.root, "s")},
                    {"dry_run": "no"},
                    {"plugin": {"allow_remove": 1}},
                    {"rate_limit": {"actions": -1}}):
            with self.assertRaises(H.SummonerError, msg=bad):
                load(bad)
        # The config the README once told people to write still loads, as
        # the restriction it always was (remove off).
        cfg = load({"dry_run": False, "plugin": {
            "name": "ps",
            "command": ["${PYTHON}", "${SUMMONER_DIR}/plugins/processes.py"],
            "allow_remove": False}})
        self.assertFalse(cfg["dry_run"])
        self.assertFalse(cfg["plugin"]["allow_remove"])
        self.assertEqual(cfg["plugin"]["command"][-1], PROC_PLUGIN)

    def test_a_restricting_config_holds_at_action_time(self):
        cfg, items = self.ps_with_fake([{"key": "a", "name": "A"}],
                                       over={"plugin": {"allow_remove": False}})
        self.act(cfg, items["a"], "delete")
        self.assertEqual(self.acted(), ["nudge"])
        self.assertEqual([(e["outcome"], e.get("reason"))
                          for e in self.audit()][-1],
                         ("blocked", "plugin config"))
        cfg, items = self.ps_with_fake([{"key": "a", "name": "A"}],
                                       over={"dry_run": True})
        self.act(cfg, items["a"], "delete")
        self.assertEqual(self.acted(), ["nudge"])        # no new call

    def test_an_old_ps_json_naming_the_default_state_still_loads(self):
        path = os.path.join(self.hroot, "plugins", "ps.json")
        with open(path, "w") as f:
            json.dump({"state_dir": os.path.join(self.hroot, "state", "ps"),
                       "plugin": {"allow_remove": False}}, f)
        cfg = H.load_named(self.hroot, "ps")
        self.assertFalse(cfg["plugin"]["allow_remove"])
        with open(path, "w") as f:
            json.dump({"state_dir": os.path.join(self.root, "elsewhere")}, f)
        with self.assertRaises(H.SummonerError):
            H.load_named(self.hroot, "ps")

    def test_the_games_are_protected_names(self):
        for name in ("psDoom", "crispy-doom", "psQuake", "quakespasm"):
            self.assertIn(name, P.DENY)

    def test_other_plugins_stay_dry_run_by_default(self):
        cfg = H.config_from_dict({"plugin": {"name": "x", "command": ["x"]}},
                                 "x.json", self.state, [])
        self.assertTrue(cfg["dry_run"])
        self.assertFalse(cfg["plugin"]["allow_remove"])


class EffectTests(unittest.TestCase):
    """A plugin's plain words for what a wound and a kill do."""

    def test_processes_plugin_declares_them(self):
        d = H.validate_describe(P.describe())
        self.assertEqual(d["effects"], {"post": "lowers its priority",
                                        "delete": "quits the process"})

    def test_only_listed_verbs_and_strings_count(self):
        d = H.validate_describe({
            "verbs": ["list", "nudge"],
            "nudge": {"effect": "  slows\tit\ndown  "},
            "remove": {"effect": "quits it"}})
        self.assertEqual(d["effects"], {"post": "slows it down"})
        d = H.validate_describe({"verbs": ["list", "nudge", "remove"],
                                 "nudge": {"effect": 3},
                                 "remove": "quits"})
        self.assertEqual(d["effects"], {})
        d = H.validate_describe({"verbs": ["list", "remove"],
                                 "remove": {"effect": "x" * 100}})
        self.assertEqual(len(d["effects"]["delete"]), H.EFFECT_MAX)

    def test_words_only_for_what_the_config_lets_run(self):
        desc = H.validate_describe(P.describe())
        cache = {"describe": desc}
        def lines(cfg):
            out = H.render([], H.effects_for(cfg, cache))
            return [ln for ln in out.split("\n") if ln.startswith("# effect")]
        both = {"dry_run": False, "plugin": {"allow_remove": True}}
        self.assertEqual(len(lines(both)), 2)
        self.assertEqual(lines({"dry_run": False,
                                "plugin": {"allow_remove": False}}),
                         ["# effect verb=post text=lowers its priority",
                          "# effect verb=delete text=nothing"])
        self.assertEqual(lines({"dry_run": True,
                                "plugin": {"allow_remove": True}}),
                         ["# effect verb=post text=nothing",
                          "# effect verb=delete text=nothing"])
        self.assertEqual(desc["effects"]["delete"], "quits the process")

    def test_the_poll_carries_them_as_header_lines(self):
        desc = H.validate_describe(P.describe())
        out = H.render([], {"describe": desc})
        lines = [ln for ln in out.split("\n") if ln.startswith("# effect")]
        self.assertEqual(lines, [
            "# effect verb=post text=lowers its priority",
            "# effect verb=delete text=quits the process"])
        self.assertEqual(parse_v1(out), [])


if __name__ == "__main__":
    unittest.main()
