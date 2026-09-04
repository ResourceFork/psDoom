#!/usr/bin/env python3
"""Generate the deterministic psDoom octagonal sky-arena PWAD."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Iterable, Sequence

CENTER_X = 2080
CENTER_Y = -3432
ML_BLOCKING = 0x0001
ML_TWOSIDED = 0x0004
NO_SIDE = 0xFFFF

Point = tuple[Fraction, Fraction]


@dataclass(frozen=True)
class Line:
    point: Point
    delta: Point


@dataclass
class Leaf:
    polygon: list[Point]
    sector: int
    subsector: int = -1


@dataclass
class Split:
    line: Line
    right: "Tree"
    left: "Tree"


Tree = Leaf | Split


@dataclass(frozen=True)
class Sector:
    floor: int
    ceiling: int
    floor_flat: str
    ceiling_flat: str
    light: int
    special: int = 0
    tag: int = 0


@dataclass(frozen=True)
class Thing:
    x: int
    y: int
    angle: int
    kind: int
    flags: int


def point(x: int | Fraction, y: int | Fraction) -> Point:
    return Fraction(x), Fraction(y)


def cross(delta: Point, value: Point) -> Fraction:
    return delta[0] * value[1] - delta[1] * value[0]


def subtract(a: Point, b: Point) -> Point:
    return a[0] - b[0], a[1] - b[1]


def line_side(line: Line, value: Point) -> Fraction:
    return cross(line.delta, subtract(value, line.point))


def polygon_area(polygon: Sequence[Point]) -> Fraction:
    return sum(
        a[0] * b[1] - b[0] * a[1]
        for a, b in zip(polygon, polygon[1:] + polygon[:1])
    ) / 2


def clean_polygon(polygon: Iterable[Point]) -> list[Point]:
    result: list[Point] = []
    for value in polygon:
        if not result or value != result[-1]:
            result.append(value)
    if len(result) > 1 and result[0] == result[-1]:
        result.pop()
    return result


def clip_polygon(polygon: Sequence[Point], line: Line, keep_right: bool) -> list[Point]:
    result: list[Point] = []

    def inside(side: Fraction) -> bool:
        return side <= 0 if keep_right else side >= 0

    for start, end in zip(polygon, polygon[1:] + polygon[:1]):
        start_side = line_side(line, start)
        end_side = line_side(line, end)
        start_inside = inside(start_side)
        end_inside = inside(end_side)
        if start_inside:
            result.append(start)
        if start_inside != end_inside:
            ratio = start_side / (start_side - end_side)
            result.append(
                (
                    start[0] + ratio * (end[0] - start[0]),
                    start[1] + ratio * (end[1] - start[1]),
                )
            )
    return clean_polygon(result)


def split_polygon(polygon: Sequence[Point], line: Line) -> tuple[list[Point], list[Point]]:
    right = clip_polygon(polygon, line, True)
    left = clip_polygon(polygon, line, False)
    if len(right) < 3 or polygon_area(right) == 0:
        right = []
    if len(left) < 3 or polygon_area(left) == 0:
        left = []
    return right, left


def ensure_clockwise(polygon: Sequence[Point]) -> list[Point]:
    result = list(polygon)
    if polygon_area(result) > 0:
        result.reverse()
    if polygon_area(result) >= 0:
        raise ValueError("degenerate polygon")
    return result


def convex_intersection(subject: Sequence[Point], clipper: Sequence[Point]) -> list[Point]:
    result = ensure_clockwise(subject)
    for start, end in zip(clipper, clipper[1:] + clipper[:1]):
        result = clip_polygon(result, Line(start, subtract(end, start)), True)
        if len(result) < 3:
            return []
    return clean_polygon(result)


def on_segment(value: Point, start: Point, end: Point) -> bool:
    if cross(subtract(end, start), subtract(value, start)) != 0:
        return False
    return (
        min(start[0], end[0]) <= value[0] <= max(start[0], end[0])
        and min(start[1], end[1]) <= value[1] <= max(start[1], end[1])
    )


def centroid(polygon: Sequence[Point]) -> Point:
    return (
        sum(value[0] for value in polygon) / len(polygon),
        sum(value[1] for value in polygon) / len(polygon),
    )


def same_polygon(a: Sequence[Point], b: Sequence[Point]) -> bool:
    return set(a) == set(b) and abs(polygon_area(a)) == abs(polygon_area(b))


def octagon(radius: int, diagonal: int) -> list[Point]:
    corner = diagonal - radius
    return [
        point(-corner, radius),
        point(corner, radius),
        point(radius, corner),
        point(radius, -corner),
        point(corner, -radius),
        point(-corner, -radius),
        point(-radius, -corner),
        point(-radius, corner),
    ]


def rectangle(left: int, bottom: int, right: int, top: int) -> list[Point]:
    return [point(left, bottom), point(left, top), point(right, top), point(right, bottom)]


PILLAR_CENTERS = [
    (-304, 608), (304, 608), (608, 304), (608, -304),
    (304, -608), (-304, -608), (-608, -304), (-608, 304),
]
LANDINGS = [rectangle(x - 64, y - 64, x + 64, y + 64) for x, y in PILLAR_CENTERS]


def cover_pillar(x: int, y: int) -> list[Point]:
    """Return a shallow pillar whose long axis follows the local ring tangent.

    The north/south-side pillars are wider east-to-west; the east/west-side
    pillars are wider north-to-south. Each remains 32 units deep radially,
    fits its 128-unit landing, and presents a 67% wider silhouette to a player
    looking outward from the arena center.
    """
    if abs(y) > abs(x):
        return rectangle(x - 32, y - 16, x + 32, y + 16)
    return rectangle(x - 16, y - 32, x + 16, y + 32)


PILLARS = [cover_pillar(x, y) for x, y in PILLAR_CENTERS]
EXIT_SHELL = rectangle(-48, 1024, 48, 1120)
EXIT_CAVITY = [
    point(-24, 1024), point(-24, 1072), point(-16, 1088),
    point(16, 1088), point(24, 1072), point(24, 1024),
]
SWITCH_EDGE = frozenset((point(-16, 1088), point(16, 1088)))
ENTRANCE_EDGE = frozenset((point(-24, 1024), point(24, 1024)))
ML_DONTPEGTOP = 0x0008


def boundary_line(normal: tuple[int, int], threshold: int) -> Line:
    nx, ny = normal
    anchor = point(nx * threshold, 0) if nx else point(0, ny * threshold)
    return Line(anchor, point(ny, -nx))


def wedge_normal(polygon: Sequence[Point]) -> tuple[int, int]:
    value = centroid(polygon)
    candidates = [
        (0, 1),
        (1, 1),
        (1, 0),
        (1, -1),
        (0, -1),
        (-1, -1),
        (-1, 0),
        (-1, 1),
    ]

    def score(normal: tuple[int, int]) -> Fraction:
        scale = 6144 if normal[0] and normal[1] else 4096
        return (normal[0] * value[0] + normal[1] * value[1]) / scale

    return max(candidates, key=score)


def build_bands(polygon: Sequence[Point], normal: tuple[int, int]) -> Tree:
    cardinal = not (normal[0] and normal[1])
    thresholds = [384, 448, 512, 576, 640, 1024, 1064]
    if not cardinal:
        thresholds = [value * 3 // 2 for value in thresholds]
    sector_for_inner = [0, 1, 2, 3, 4, 5, 6]

    def split_band(remaining: list[Point], index: int) -> Tree:
        if index == len(thresholds):
            return Leaf(ensure_clockwise(remaining), 7)
        line = boundary_line(normal, thresholds[index])
        inner, outer = split_polygon(remaining, line)
        if not inner or not outer:
            raise ValueError("octagonal band did not split wedge")
        inner_tree = Leaf(ensure_clockwise(inner), sector_for_inner[index])
        return Split(line, inner_tree, split_band(ensure_clockwise(outer), index + 1))

    return split_band(ensure_clockwise(polygon), 0)


def carve_leaf(leaf: Leaf, feature: Sequence[Point], sector: int) -> Tree:
    intersection = convex_intersection(leaf.polygon, feature)
    if len(intersection) < 3 or polygon_area(intersection) == 0:
        return leaf

    def carve(remaining: list[Point], edge_index: int) -> Tree:
        if edge_index == len(feature):
            return Leaf(ensure_clockwise(remaining), sector)
        start = feature[edge_index]
        end = feature[(edge_index + 1) % len(feature)]
        line = Line(start, subtract(end, start))
        inside, outside = split_polygon(remaining, line)
        if not inside:
            raise ValueError("feature intersection vanished while carving")
        inside_tree = carve(ensure_clockwise(inside), edge_index + 1)
        if not outside:
            return inside_tree
        return Split(
            line,
            inside_tree,
            Leaf(ensure_clockwise(outside), leaf.sector),
        )

    return carve(ensure_clockwise(leaf.polygon), 0)


def overlay_feature(tree: Tree, feature: Sequence[Point], sector: int) -> Tree:
    feature = ensure_clockwise(feature)
    if isinstance(tree, Leaf):
        return carve_leaf(tree, feature, sector)
    return Split(
        tree.line,
        overlay_feature(tree.right, feature, sector),
        overlay_feature(tree.left, feature, sector),
    )


def build_tree() -> Tree:
    radial_lines = [
        Line(point(0, 0), point(1, 2)),
        Line(point(0, 0), point(2, 1)),
        Line(point(0, 0), point(2, -1)),
        Line(point(0, 0), point(1, -2)),
    ]

    def split_radially(polygon: list[Point], index: int) -> Tree:
        if index == len(radial_lines):
            return build_bands(polygon, wedge_normal(polygon))
        line = radial_lines[index]
        right, left = split_polygon(polygon, line)
        if right and left:
            return Split(
                line,
                split_radially(ensure_clockwise(right), index + 1),
                split_radially(ensure_clockwise(left), index + 1),
            )
        return split_radially(ensure_clockwise(right or left), index + 1)

    tree = split_radially(ensure_clockwise(octagon(4096, 6144)), 0)
    for landing in LANDINGS:
        tree = overlay_feature(tree, landing, 5)
    for pillar_index, pillar in enumerate(PILLARS):
        tree = overlay_feature(tree, pillar, 8 + pillar_index)
    tree = overlay_feature(tree, EXIT_SHELL, 17)
    tree = overlay_feature(tree, EXIT_CAVITY, 16)
    return tree


def collect_leaves(tree: Tree) -> list[Leaf]:
    leaves: list[Leaf] = []

    def visit(value: Tree) -> None:
        if isinstance(value, Leaf):
            value.subsector = len(leaves)
            leaves.append(value)
            return
        visit(value.right)
        visit(value.left)

    visit(tree)
    return leaves


def normalize_leaf_edges(leaves: Sequence[Leaf]) -> None:
    all_vertices = sorted({value for leaf in leaves for value in leaf.polygon})
    for leaf in leaves:
        normalized: list[Point] = []
        for start, end in zip(leaf.polygon, leaf.polygon[1:] + leaf.polygon[:1]):
            delta = subtract(end, start)
            length_squared = delta[0] * delta[0] + delta[1] * delta[1]
            edge_vertices = [
                value for value in all_vertices if on_segment(value, start, end)
            ]
            edge_vertices.sort(
                key=lambda value: (
                    (value[0] - start[0]) * delta[0]
                    + (value[1] - start[1]) * delta[1]
                )
                / length_squared
            )
            normalized.extend(edge_vertices[:-1])
        leaf.polygon = clean_polygon(normalized)
        if polygon_area(leaf.polygon) >= 0:
            raise ValueError("normalized subsector is not clockwise")


def integer(value: Fraction, label: str) -> int:
    if value.denominator != 1:
        raise ValueError(f"{label} is fractional: {value}")
    result = value.numerator
    if not -32768 <= result <= 32767:
        raise ValueError(f"{label} is outside signed 16-bit range: {result}")
    return result


def absolute(value: Point) -> tuple[int, int]:
    return (
        integer(value[0], "x coordinate") + CENTER_X,
        integer(value[1], "y coordinate") + CENTER_Y,
    )


def encode_name(name: str) -> bytes:
    encoded = name.encode("ascii")
    if not encoded or len(encoded) > 8:
        raise ValueError(f"invalid WAD name: {name!r}")
    return encoded.ljust(8, b"\0")


def sectors() -> list[Sector]:
    return [
        Sector(0, 256, "FLOOR4_8", "F_SKY1", 176),
        Sector(16, 256, "FLAT5_4", "F_SKY1", 180),
        Sector(32, 256, "FLAT5_4", "F_SKY1", 184),
        Sector(48, 256, "FLAT5_4", "F_SKY1", 188),
        Sector(64, 256, "FLAT5_4", "F_SKY1", 192),
        Sector(64, 256, "FLOOR5_1", "F_SKY1", 192),
        Sector(104, 256, "FLAT14", "F_SKY1", 192),
        Sector(64, 64, "F_SKY1", "F_SKY1", 192),
    ] + [Sector(64, 64, "FLAT14", "CEIL3_5", 160) for _ in range(8)] + [
        Sector(64, 136, "FLOOR5_1", "CEIL3_5", 192),
        Sector(64, 64, "FLAT14", "CEIL3_5", 160),
    ]


def things() -> list[Thing]:
    pickups = [
        (-344, 0, 2011), (344, 0, 2011), (0, -344, 2011),
        (-240, 320, 2012), (240, 320, 2012), (0, 608, 2001),
        (608, 0, 2018), (456, 456, 2015), (456, -456, 2015),
        (-456, -456, 2015), (-456, 456, 2015), (-800, 0, 2005),
        (0, -800, 2002), (160, 736, 2049), (160, -800, 2048),
        (-736, 160, 2007), (736, 160, 2007), (736, -160, 2012),
        (-736, -160, 2014),
    ]
    values = [Thing(CENTER_X, CENTER_Y + 800, 270, 1, 0x0007)]
    values.extend(
        Thing(CENTER_X + x, CENTER_Y + y, 0, kind, 0x0007)
        for x, y, kind in pickups
    )
    return values


def side_textures(own: int, other: int | None, is_switch_front: bool) -> tuple[str, str, str]:
    upper = "-"
    lower = "-"
    middle = "-"
    if other is None or own == other:
        return upper, lower, middle
    if {own, other} <= set(range(6)):
        own_floor = sectors()[own].floor
        other_floor = sectors()[other].floor
        if own_floor != other_floor:
            lower = "STEP1"
    if 6 in (own, other):
        lower = "STONE2"
    if 8 <= own <= 15 or 8 <= other <= 15:
        upper = "SUPPORT2"
    if {own, other} == {5, 16} and own == 5:
        upper = "STONE2"
    if 17 in (own, other):
        upper = "SUPPORT2"
    if is_switch_front:
        upper = "SW1EXIT"
    return upper, lower, middle


def segment_intersects_rectangle(
    start: tuple[int, int], end: tuple[int, int], left: int, bottom: int, right: int, top: int
) -> bool:
    def inside(value: tuple[int, int]) -> bool:
        return left <= value[0] <= right and bottom <= value[1] <= top

    if inside(start) or inside(end):
        return True

    def orientation(a: tuple[int, int], b: tuple[int, int], c: tuple[int, int]) -> int:
        value = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        return (value > 0) - (value < 0)

    def intersects(a: tuple[int, int], b: tuple[int, int], c: tuple[int, int], d: tuple[int, int]) -> bool:
        o1 = orientation(a, b, c)
        o2 = orientation(a, b, d)
        o3 = orientation(c, d, a)
        o4 = orientation(c, d, b)
        if o1 == 0 and min(a[0], b[0]) <= c[0] <= max(a[0], b[0]) and min(a[1], b[1]) <= c[1] <= max(a[1], b[1]):
            return True
        if o2 == 0 and min(a[0], b[0]) <= d[0] <= max(a[0], b[0]) and min(a[1], b[1]) <= d[1] <= max(a[1], b[1]):
            return True
        if o3 == 0 and min(c[0], d[0]) <= a[0] <= max(c[0], d[0]) and min(c[1], d[1]) <= a[1] <= max(c[1], d[1]):
            return True
        if o4 == 0 and min(c[0], d[0]) <= b[0] <= max(c[0], d[0]) and min(c[1], d[1]) <= b[1] <= max(c[1], d[1]):
            return True
        return o1 != o2 and o3 != o4

    corners = [(left, bottom), (right, bottom), (right, top), (left, top)]
    return any(
        intersects(start, end, a, b)
        for a, b in zip(corners, corners[1:] + corners[:1])
    )


def make_blockmap(vertices: Sequence[tuple[int, int]], linedefs: Sequence[tuple[int, int]]) -> bytes:
    min_x = min(value[0] for value in vertices)
    max_x = max(value[0] for value in vertices)
    min_y = min(value[1] for value in vertices)
    max_y = max(value[1] for value in vertices)
    origin_x = math.floor(min_x / 128) * 128
    origin_y = math.floor(min_y / 128) * 128
    width = (max_x - origin_x) // 128 + 1
    height = (max_y - origin_y) // 128 + 1
    if not (0 < width <= 0xFFFF and 0 < height <= 0xFFFF):
        raise ValueError("blockmap dimensions are outside unsigned 16-bit range")

    lists: list[list[int]] = []
    for row in range(height):
        bottom = origin_y + row * 128
        top = bottom + 127
        for column in range(width):
            left = origin_x + column * 128
            right = left + 127
            entries = [
                index
                for index, (v1, v2) in enumerate(linedefs)
                if segment_intersects_rectangle(vertices[v1], vertices[v2], left, bottom, right, top)
            ]
            lists.append(entries)

    offsets: list[int] = []
    words: list[int] = []
    next_offset = 4 + len(lists)
    for entries in lists:
        offsets.append(next_offset + len(words))
        words.extend([0, *entries, 0xFFFF])
    total_words = 4 + len(offsets) + len(words)
    if total_words >= 65536:
        raise ValueError(f"blockmap is too large: {total_words} words")
    return (
        struct.pack("<hhHH", origin_x, origin_y, width, height)
        + struct.pack(f"<{len(offsets)}H", *offsets)
        + struct.pack(f"<{len(words)}H", *words)
    )


def build_lumps() -> tuple[list[tuple[str, bytes]], dict[str, int]]:
    tree = build_tree()
    leaves = collect_leaves(tree)
    normalize_leaf_edges(leaves)
    if len(leaves) >= 0x8000:
        raise ValueError("too many subsectors for classic BSP nodes")
    if any(value.denominator != 1 for leaf in leaves for vertex in leaf.polygon for value in vertex):
        raise ValueError("BSP clipping produced a fractional vertex")

    local_vertices = sorted({value for leaf in leaves for value in leaf.polygon})
    absolute_vertices = [absolute(value) for value in local_vertices]
    vertex_index = {value: index for index, value in enumerate(local_vertices)}

    edge_occurrences: dict[frozenset[Point], list[tuple[int, Point, Point, int]]] = {}
    for leaf in leaves:
        for start, end in zip(leaf.polygon, leaf.polygon[1:] + leaf.polygon[:1]):
            key = frozenset((start, end))
            edge_occurrences.setdefault(key, []).append((leaf.subsector, start, end, leaf.sector))

    linedef_records: list[tuple[int, int, int, int, int, int, int]] = []
    linedef_vertices: list[tuple[int, int]] = []
    sidedef_records: list[tuple[int, int, str, str, str, int]] = []
    occurrence_line: dict[tuple[int, Point, Point], tuple[int, int]] = {}
    exit_count = 0

    for key, occurrences in edge_occurrences.items():
        if len(occurrences) not in (1, 2):
            raise ValueError(f"edge belongs to {len(occurrences)} subsectors")
        is_switch = key == SWITCH_EDGE
        is_entrance = key == ENTRANCE_EDGE
        if is_switch:
            cavity = [value for value in occurrences if value[3] == 16]
            if len(cavity) != 1:
                raise ValueError("switch edge does not have one cavity side")
            front = cavity[0]
            if (front[1], front[2]) != (point(-16, 1088), point(16, 1088)):
                raise ValueError("switch cavity edge has unexpected orientation")
            ordered = [front, *(value for value in occurrences if value is not front)]
            exit_count += 1
        else:
            ordered = list(occurrences)
        front = ordered[0]
        back = ordered[1] if len(ordered) == 2 else None
        _, start, end, front_sector = front
        back_sector = back[3] if back is not None else None
        if back is not None and (back[1], back[2]) != (end, start):
            raise ValueError("shared edge directions are inconsistent")
        if is_entrance and {front_sector, back_sector} != {5, 16}:
            raise ValueError("cavity entrance does not join the walkway")

        flags = 0
        if back is not None:
            flags |= ML_TWOSIDED
        if (
            back is None
            or 6 in (front_sector, back_sector)
            or ((front_sector >= 8 or (back_sector is not None and back_sector >= 8)) and not is_entrance)
        ):
            flags |= ML_BLOCKING
        if is_switch:
            flags |= ML_DONTPEGTOP
        special = 11 if is_switch else 0
        side0_index = len(sidedef_records)
        upper, lower, middle = side_textures(front_sector, back_sector, is_switch)
        sidedef_records.append((0, 0, upper, lower, middle, front_sector))
        side1_index = NO_SIDE
        if back is not None:
            side1_index = len(sidedef_records)
            upper, lower, middle = side_textures(back_sector, front_sector, False)
            sidedef_records.append((0, 0, upper, lower, middle, back_sector))
        line_index = len(linedef_records)
        v1 = vertex_index[start]
        v2 = vertex_index[end]
        linedef_records.append((v1, v2, flags, special, 0, side0_index, side1_index))
        linedef_vertices.append((v1, v2))
        occurrence_line[(front[0], start, end)] = (line_index, 0)
        if back is not None:
            occurrence_line[(back[0], end, start)] = (line_index, 1)

    if exit_count != 1:
        raise ValueError(f"generated {exit_count} exit lines, expected 1")

    seg_records: list[tuple[int, int, int, int, int, int]] = []
    subsector_records: list[tuple[int, int]] = []
    for leaf in leaves:
        first = len(seg_records)
        for start, end in zip(leaf.polygon, leaf.polygon[1:] + leaf.polygon[:1]):
            line_index, side = occurrence_line[(leaf.subsector, start, end)]
            dx = integer(end[0] - start[0], "seg dx")
            dy = integer(end[1] - start[1], "seg dy")
            angle = int(round(math.atan2(dy, dx) * 65536 / (2 * math.pi))) & 0xFFFF
            seg_records.append(
                (vertex_index[start], vertex_index[end], angle, line_index, side, 0)
            )
        subsector_records.append((len(seg_records) - first, first))

    leaf_by_identity = {id(leaf): leaf for leaf in leaves}
    node_records: list[tuple[int, ...]] = []

    def tree_vertices(value: Tree) -> list[Point]:
        if isinstance(value, Leaf):
            return value.polygon
        return tree_vertices(value.right) + tree_vertices(value.left)

    def bbox(value: Tree) -> tuple[int, int, int, int]:
        values = [absolute(vertex) for vertex in tree_vertices(value)]
        return (
            max(vertex[1] for vertex in values),
            min(vertex[1] for vertex in values),
            min(vertex[0] for vertex in values),
            max(vertex[0] for vertex in values),
        )

    def emit_child(value: Tree) -> int:
        if isinstance(value, Leaf):
            leaf = leaf_by_identity[id(value)]
            return 0x8000 | leaf.subsector
        right_child = emit_child(value.right)
        left_child = emit_child(value.left)
        x, y = absolute(value.line.point)
        dx = integer(value.line.delta[0], "node dx")
        dy = integer(value.line.delta[1], "node dy")
        if dx == 0 and dy == 0:
            raise ValueError("zero-length BSP partition")
        record = (x, y, dx, dy, *bbox(value.right), *bbox(value.left), right_child, left_child)
        node_records.append(record)
        if len(node_records) > 0x7FFF:
            raise ValueError("too many classic BSP nodes")
        return len(node_records) - 1

    root = emit_child(tree)
    if root != len(node_records) - 1 or not node_records:
        raise ValueError("BSP root is not the final node")

    sector_records = sectors()
    thing_records = things()
    reject_size = (len(sector_records) * len(sector_records) + 7) // 8
    blockmap = make_blockmap(absolute_vertices, linedef_vertices)

    lump_data = [
        ("PSDINTRO", b""),
        ("THINGS", b"".join(struct.pack("<hhhhh", value.x, value.y, value.angle, value.kind, value.flags) for value in thing_records)),
        ("LINEDEFS", b"".join(struct.pack("<HHHHHHH", *value) for value in linedef_records)),
        ("SIDEDEFS", b"".join(struct.pack("<hh8s8s8sH", value[0], value[1], encode_name(value[2]), encode_name(value[3]), encode_name(value[4]), value[5]) for value in sidedef_records)),
        ("VERTEXES", b"".join(struct.pack("<hh", *value) for value in absolute_vertices)),
        ("SEGS", b"".join(struct.pack("<HHHHHH", *value) for value in seg_records)),
        ("SSECTORS", b"".join(struct.pack("<HH", *value) for value in subsector_records)),
        ("NODES", b"".join(struct.pack("<hhhhhhhhhhhhHH", *value) for value in node_records)),
        ("SECTORS", b"".join(struct.pack("<hh8s8shhh", value.floor, value.ceiling, encode_name(value.floor_flat), encode_name(value.ceiling_flat), value.light, value.special, value.tag) for value in sector_records)),
        ("REJECT", bytes(reject_size)),
        ("BLOCKMAP", blockmap),
    ]
    counts = {
        "things": len(thing_records),
        "linedefs": len(linedef_records),
        "sidedefs": len(sidedef_records),
        "vertices": len(absolute_vertices),
        "segs": len(seg_records),
        "subsectors": len(subsector_records),
        "nodes": len(node_records),
        "sectors": len(sector_records),
    }
    return lump_data, counts


def write_wad(output: Path) -> dict[str, int]:
    lumps, counts = build_lumps()
    payload = bytearray()
    directory: list[tuple[int, int, str]] = []
    for name, data in lumps:
        offset = 12 + len(payload)
        payload.extend(data)
        directory.append((offset, len(data), name))
    directory_offset = 12 + len(payload)
    header = struct.pack("<4sII", b"PWAD", len(directory), directory_offset)
    entries = b"".join(
        struct.pack("<II8s", offset, size, encode_name(name))
        for offset, size, name in directory
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(header + payload + entries)
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="output PWAD path")
    args = parser.parse_args()
    counts = write_wad(args.output)
    summary = ", ".join(f"{name}={value}" for name, value in counts.items())
    print(f"generated {args.output}: PSDINTRO ({summary})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
