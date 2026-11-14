#!/usr/bin/env python3
"""Summoner plugin: rows of a CSV file.

    csv_rows.py --file F [--key-column id] [--name-column name]
                [--priority-column P] [--size-column S] [--load-column L]
                (describe | list [sort=ID[:dir],..] [group=ID] [limit=N] |
                 nudge <key> amount=N | remove <key> | restore < undo.json)

Each row with a non-empty key cell is an item. Every other column is a
dimension (`describe` lists them): numeric columns sort, text columns group.
`list` sorts by the request (default: the priority column, highest first;
without one, file order), gathers groups together, cuts to `limit`, and
grades each row by its place on the primary sort column. With a priority
column `nudge` lowers it by `amount`; without one, `nudge` moves the row down
`amount` places. `remove` deletes the row. Both return an undo record, and
`restore` (the record on stdin) puts things back.

Writes are atomic (a temp file in the same directory, then rename) and are
serialised by an flock on a sidecar ".<file>.lock". A symlink is written
through to its target and the file's mode is kept; a hard link to the file
is not (rename gives it a new inode). A key names the first row that has it;
later rows with the same key are not listed. The file is read and
written as lists, so extra cells and column order survive; only the quoting
style may change. Line endings and a UTF-8 byte-order mark are kept.
Standard library only.
"""

import argparse
import csv
import fcntl
import io
import json
import os
import re
import stat
import sys

BOM = "\ufeff"


def parse_args(argv):
    p = argparse.ArgumentParser(prog="csv_rows.py")
    p.add_argument("--file", required=True)
    p.add_argument("--key-column", default="id")
    p.add_argument("--name-column", default="name")
    p.add_argument("--priority-column")
    p.add_argument("--size-column")
    p.add_argument("--load-column")
    p.add_argument("verb")
    p.add_argument("rest", nargs="*")
    return p.parse_args(argv)


class Sheet:
    def __init__(self, path):
        # Write through a symlink to its target, so the link survives.
        self.path = os.path.realpath(path)
        with open(self.path, "r", encoding="utf-8", newline="") as f:
            text = f.read()
        self.bom = text.startswith(BOM)
        if self.bom:
            text = text[1:]
        # The header line's ending is the file's style (a CRLF inside a
        # quoted cell does not count); a missing final newline stays missing.
        first = text.find("\n")
        self.eol = "\r\n" if first > 0 and text[first - 1] == "\r" \
            else "\n"
        self.final_eol = text.endswith("\n") or not text
        rows = list(csv.reader(io.StringIO(text)))
        self.header = rows[0] if rows else []
        self.rows = rows[1:]

    def col(self, name, required=True):
        if name is None:
            return None
        if name not in self.header:
            if required:
                raise SystemExit("column %r not in header" % name)
            return None
        return self.header.index(name)

    def cell(self, row, idx):
        return row[idx] if idx is not None and idx < len(row) else ""

    def find(self, kcol, key):
        for i, row in enumerate(self.rows):
            if self.cell(row, kcol) == key:
                return i
        return -1

    def save(self):
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator=self.eol)
        w.writerow(self.header)
        w.writerows(self.rows)
        body = buf.getvalue()
        if not self.final_eol and body.endswith(self.eol):
            body = body[:-len(self.eol)]
        d = os.path.dirname(self.path)
        tmp = os.path.join(d, ".%s.tmp.%d" % (os.path.basename(self.path),
                                             os.getpid()))
        mode = stat.S_IMODE(os.stat(self.path).st_mode)
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write((BOM if self.bom else "") + body)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode)   # keep the file's permissions (a 0600 stays)
        os.replace(tmp, self.path)


class FileLock:
    def __init__(self, path):
        d, b = os.path.split(os.path.abspath(path))
        self.lock_path = os.path.join(d, ".%s.lock" % b)
        self.fd = None

    def __enter__(self):
        self.fd = os.open(self.lock_path, os.O_RDWR | os.O_CREAT, 0o600)
        fcntl.flock(self.fd, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        os.close(self.fd)
        return False


def num(s):
    try:
        v = float(s)
    except (TypeError, ValueError):
        return None
    return v if v == v and abs(v) != float("inf") else None


def fmt_num(v):
    return str(int(v)) if float(v).is_integer() else repr(v)


def dimensions(a, sh):
    """Every column but the key is a dimension: numeric when each non-empty
    cell is a number (sortable), else a category (groupable). Ids are the
    column names folded to [a-z0-9_], at most 16, made unique."""
    kcol = sh.col(a.key_column)
    out, used = [], set()
    for idx, head in enumerate(sh.header):
        if idx == kcol:
            continue
        cells = [sh.cell(r, idx) for r in sh.rows if sh.cell(r, idx) != ""]
        numeric = bool(cells) and all(num(c) is not None for c in cells)
        base = re.sub(r"[^a-z0-9_]+", "_", head.strip().lower()).strip("_")
        if not base or not base[0].isalpha():
            base = "c_" + base
        base = base[:16]
        did, n = base, 2
        while did in used:
            suffix = "_%d" % n
            did, n = base[:16 - len(suffix)] + suffix, n + 1
        used.add(did)
        out.append({"id": did, "label": head.strip() or did,
                    "kind": "numeric" if numeric else "category",
                    "dir": "desc", "roles": ["sort"] if numeric else ["group"],
                    "column": idx})
    return out


def describe(a, sh=None):
    d = {
        "protocol": 1,
        "name": "csv",
        "verbs": ["list", "nudge", "remove", "restore"],
        "size": {"meaning": a.size_column or a.priority_column or "rank",
                 "scale": "relative"},
        "load": {"meaning": a.load_column or "none", "scale": "relative"},
        "nudge": {"meaning": ("lower %s by amount" % a.priority_column)
                  if a.priority_column else "move the row down amount rows"},
        "remove": {"meaning": "delete the row", "undoable": True},
    }
    if sh is not None:
        dims = dimensions(a, sh)
        d["dimensions"] = [{k: v for k, v in x.items() if k != "column"}
                           for x in dims]
        pcol = sh.col(a.priority_column)
        for x in dims:
            if x["column"] == pcol and x["kind"] == "numeric":
                d["default_sort"] = x["id"]
    return d


def parse_request(words, dims):
    by_id = {d["id"]: d for d in dims}
    sort, group, limit = [], None, None
    for w in words:
        k, _, v = w.partition("=")
        if k == "sort":
            for part in v.split(","):
                did, _, ddir = part.partition(":")
                d = by_id.get(did)
                if d and d["kind"] == "numeric":
                    sort.append((d["column"], ddir if ddir in ("asc", "desc")
                                 else d["dir"], did))
        elif k == "group" and by_id.get(v, {}).get("kind") == "category":
            group = by_id[v]["column"]
        elif k == "limit" and v.isdigit() and int(v) > 0:
            limit = int(v)
    return sort, group, limit


def list_items(a, sh, words=()):
    kcol = sh.col(a.key_column)
    ncol = sh.col(a.name_column, required=False)
    pcol = sh.col(a.priority_column)
    scol = sh.col(a.size_column)
    lcol = sh.col(a.load_column)
    dims = dimensions(a, sh)
    sort, group, limit = parse_request(words, dims)
    if not sort and pcol is not None:
        sort = [(pcol, "desc", None)]
    # A key names the first row with it in file order -- the row nudge and
    # remove act on -- so later duplicates are never listed.
    rows, seen = [], set()
    for i, r in enumerate(sh.rows):
        k = sh.cell(r, kcol)
        if k and k not in seen:
            seen.add(k)
            rows.append((i, r))

    def key(ir):
        k = []
        for col, ddir, _ in sort:
            v = num(sh.cell(ir[1], col))
            k.append((v is None, 0.0 if v is None
                      else (-v if ddir == "desc" else v)))
        return k + [ir[0]]           # file order breaks ties
    rows.sort(key=key)
    if group is not None:
        order, members = [], {}
        for ir in rows:
            g = sh.cell(ir[1], group) or "-"
            if g not in members:
                order.append(g)
                members[g] = []
            members[g].append(ir)
        rows = [ir for g in order for ir in members[g]]
    if limit:
        rows = rows[:limit]
    # grade: place on the primary sort column among the listed rows
    grade_of = {}
    if sort:
        col, ddir, _ = sort[0]
        vals = sorted({num(sh.cell(r, col)) for _, r in rows
                       if num(sh.cell(r, col)) is not None},
                      reverse=(ddir == "desc"))
        pos = {v: i for i, v in enumerate(vals)}
        for i, r in rows:
            v = num(sh.cell(r, col))
            if v is not None:
                grade_of[i] = 1.0 if len(vals) < 2 else \
                    round(1.0 - pos[v] / (len(vals) - 1), 4)
    numeric = [d for d in dims if d["kind"] == "numeric"]
    out = []
    for i, r in rows:
        it = {"key": sh.cell(r, kcol),
              "name": sh.cell(r, ncol) if ncol is not None
              else sh.cell(r, kcol)}
        size = num(sh.cell(r, scol if scol is not None else pcol))
        if size is not None and size >= 0:
            it["size"] = size
        load = num(sh.cell(r, lcol))
        if load is not None and load >= 0:
            it["load"] = load
        it["values"] = {d["id"]: num(sh.cell(r, d["column"]))
                        for d in numeric}
        if i in grade_of:
            it["grade"] = grade_of[i]
        if group is not None:
            it["group"] = sh.cell(r, group) or "-"
        out.append(it)
    return out


def result(ok, message="", undo=None):
    print(json.dumps({"ok": ok, "message": message, "undo": undo}))
    return 0 if ok else 1


def nudge(a, sh, key, amount):
    kcol = sh.col(a.key_column)
    i = sh.find(kcol, key)
    if i < 0:
        return result(False, "no row with that key")
    pcol = sh.col(a.priority_column)
    if pcol is not None:
        row = sh.rows[i]
        while len(row) <= pcol:
            row.append("")
        old = row[pcol]
        row[pcol] = fmt_num((num(old) or 0.0) - amount)
        sh.save()
        return result(True, "%s %s -> %s" % (a.priority_column, old,
                                             row[pcol]),
                      {"op": "priority", "key": key, "old": old})
    new = min(len(sh.rows) - 1, i + amount)
    sh.rows.insert(new, sh.rows.pop(i))
    sh.save()
    return result(True, "row %d -> %d" % (i, new),
                  {"op": "move", "key": key, "index": i})


def remove(a, sh, key):
    kcol = sh.col(a.key_column)
    i = sh.find(kcol, key)
    if i < 0:
        return result(False, "no row with that key")
    row = sh.rows.pop(i)
    sh.save()
    return result(True, "row deleted",
                  {"op": "insert", "key": key, "index": i, "row": row})


def restore(a, sh, undo):
    kcol = sh.col(a.key_column)
    if not isinstance(undo, dict) or not isinstance(undo.get("key"), str):
        return result(False, "bad undo record")
    key, op = undo["key"], undo.get("op")
    i = sh.find(kcol, key)
    if op == "insert":
        row = undo.get("row")
        if i >= 0:
            return result(False, "a row with that key exists again")
        if not isinstance(row, list) or \
                not all(isinstance(c, str) for c in row):
            return result(False, "bad undo record")
        idx = undo.get("index")
        idx = idx if isinstance(idx, int) and idx >= 0 else len(sh.rows)
        sh.rows.insert(min(idx, len(sh.rows)), row)
    elif op == "priority":
        pcol = sh.col(a.priority_column)
        if i < 0 or pcol is None or not isinstance(undo.get("old"), str):
            return result(False, "row or priority column is gone")
        row = sh.rows[i]
        while len(row) <= pcol:
            row.append("")
        row[pcol] = undo["old"]
    elif op == "move":
        idx = undo.get("index")
        if i < 0 or not isinstance(idx, int) or idx < 0:
            return result(False, "row is gone")
        sh.rows.insert(min(idx, len(sh.rows) - 1), sh.rows.pop(i))
    else:
        return result(False, "unknown undo op")
    sh.save()
    return result(True, "restored")


def main(argv):
    a = parse_args(argv)
    if a.verb == "describe":
        print(json.dumps(describe(a, Sheet(a.file))))
        return 0
    if a.verb == "restore":
        try:
            undo = json.loads(sys.stdin.read())
        except ValueError:
            return result(False, "undo record is not JSON")
        with FileLock(a.file):
            return restore(a, Sheet(a.file), undo)
    if a.verb == "list":
        for it in list_items(a, Sheet(a.file), a.rest):
            print(json.dumps(it))
        return 0
    if a.verb in ("nudge", "remove") and a.rest:
        key = a.rest[0]
        with FileLock(a.file):
            sh = Sheet(a.file)
            if a.verb == "remove":
                return remove(a, sh, key)
            amount = 1
            for kv in a.rest[1:]:
                if kv.startswith("amount=") and kv[7:].isdigit():
                    amount = int(kv[7:])
            return nudge(a, sh, key, amount)
    return result(False, "unsupported verb")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
