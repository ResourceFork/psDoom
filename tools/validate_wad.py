#!/usr/bin/env python3
"""Strictly validate the generated psDoom octagonal sky-arena PWAD."""

from __future__ import annotations

import argparse
import math
import struct
import sys
from collections import Counter, defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

MAP_LUMPS = [
    "THINGS",
    "LINEDEFS",
    "SIDEDEFS",
    "VERTEXES",
    "SEGS",
    "SSECTORS",
    "NODES",
    "SECTORS",
    "REJECT",
    "BLOCKMAP",
]
ML_BLOCKING = 0x0001
ML_TWOSIDED = 0x0004
ML_DONTPEGTOP = 0x0008
NO_SIDE = 0xFFFF


class ValidationError(ValueError):
    pass


@dataclass(frozen=True)
class Lump:
    name: str
    offset: int
    size: int
    data: bytes


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def decode_name(raw: bytes, context: str) -> str:
    require(len(raw) == 8, f"{context}: name is not eight bytes")
    nul = raw.find(b"\0")
    value = raw if nul < 0 else raw[:nul]
    if nul >= 0:
        require(not any(raw[nul:]), f"{context}: nonzero bytes follow name terminator")
    try:
        name = value.decode("ascii")
    except UnicodeDecodeError as error:
        raise ValidationError(f"{context}: name is not ASCII") from error
    require(bool(name), f"{context}: empty name")
    return name


def parse_wad(path: Path, expected_magic: bytes | None = None) -> tuple[bytes, list[Lump]]:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValidationError(f"cannot read {path}: {error}") from error
    require(len(data) >= 12, f"{path}: file is shorter than a WAD header")
    magic, count, directory_offset = struct.unpack_from("<4sII", data)
    if expected_magic is not None:
        require(magic == expected_magic, f"{path}: expected {expected_magic.decode()} header")
    else:
        require(magic in (b"IWAD", b"PWAD"), f"{path}: invalid WAD header {magic!r}")
    require(count > 0, f"{path}: empty WAD directory")
    directory_size = count * 16
    require(
        directory_offset >= 12 and directory_offset + directory_size <= len(data),
        f"{path}: directory is outside file bounds",
    )
    lumps: list[Lump] = []
    for index in range(count):
        entry = directory_offset + index * 16
        offset, size, raw_name = struct.unpack_from("<II8s", data, entry)
        name = decode_name(raw_name, f"directory entry {index}")
        require(offset <= len(data), f"{name}: lump offset is outside file bounds")
        require(size <= len(data) - offset, f"{name}: lump payload is outside file bounds")
        require(
            size == 0 or offset + size <= directory_offset,
            f"{name}: lump overlaps the WAD directory",
        )
        lumps.append(Lump(name, offset, size, data[offset : offset + size]))
    return magic, lumps


def records(lump: Lump, format_string: str, label: str) -> list[tuple[int | bytes, ...]]:
    size = struct.calcsize(format_string)
    require(lump.size % size == 0, f"{label}: size {lump.size} is not divisible by {size}")
    return [
        struct.unpack_from(format_string, lump.data, offset)
        for offset in range(0, lump.size, size)
    ]


def point_side(node: Sequence[int], value: tuple[int, int]) -> int:
    x, y, dx, dy = node[:4]
    return dx * (value[1] - y) - dy * (value[0] - x)


def signed_area(values: Sequence[tuple[int, int]]) -> int:
    return sum(
        a[0] * b[1] - b[0] * a[1]
        for a, b in zip(values, values[1:] + values[:1])
    )


def squared_distance_to_segment(
    value: tuple[int, int], start: tuple[int, int], end: tuple[int, int]
) -> float:
    dx = end[0] - start[0]
    dy = end[1] - start[1]
    length_squared = dx * dx + dy * dy
    projection = (
        (value[0] - start[0]) * dx + (value[1] - start[1]) * dy
    ) / length_squared
    projection = max(0.0, min(1.0, projection))
    closest_x = start[0] + projection * dx
    closest_y = start[1] + projection * dy
    return (value[0] - closest_x) ** 2 + (value[1] - closest_y) ** 2


def segment_intersects_rectangle(
    start: tuple[int, int], end: tuple[int, int], left: int, bottom: int, right: int, top: int
) -> bool:
    def inside(value: tuple[int, int]) -> bool:
        return left <= value[0] <= right and bottom <= value[1] <= top

    if inside(start) or inside(end):
        return True

    def orientation(a: tuple[int, int], b: tuple[int, int], c: tuple[int, int]) -> int:
        result = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        return (result > 0) - (result < 0)

    def on_segment(a: tuple[int, int], b: tuple[int, int], value: tuple[int, int]) -> bool:
        return (
            min(a[0], b[0]) <= value[0] <= max(a[0], b[0])
            and min(a[1], b[1]) <= value[1] <= max(a[1], b[1])
        )

    def intersects(a: tuple[int, int], b: tuple[int, int], c: tuple[int, int], d: tuple[int, int]) -> bool:
        o1 = orientation(a, b, c)
        o2 = orientation(a, b, d)
        o3 = orientation(c, d, a)
        o4 = orientation(c, d, b)
        if o1 == 0 and on_segment(a, b, c):
            return True
        if o2 == 0 and on_segment(a, b, d):
            return True
        if o3 == 0 and on_segment(c, d, a):
            return True
        if o4 == 0 and on_segment(c, d, b):
            return True
        return o1 != o2 and o3 != o4

    corners = [(left, bottom), (right, bottom), (right, top), (left, top)]
    return any(
        intersects(start, end, a, b)
        for a, b in zip(corners, corners[1:] + corners[:1])
    )


def parse_texture_definitions(lumps: Sequence[Lump]) -> dict[str, tuple[int, int]]:
    textures: dict[str, tuple[int, int]] = {}
    for lump in lumps:
        if lump.name not in ("TEXTURE1", "TEXTURE2"):
            continue
        require(lump.size >= 4, f"{lump.name}: truncated texture directory")
        count = struct.unpack_from("<I", lump.data)[0]
        require(4 + count * 4 <= lump.size, f"{lump.name}: truncated offset table")
        for index in range(count):
            offset = struct.unpack_from("<I", lump.data, 4 + index * 4)[0]
            require(offset + 22 <= lump.size, f"{lump.name}: texture {index} header is out of bounds")
            name = decode_name(lump.data[offset : offset + 8], f"{lump.name} texture {index}")
            width, height = struct.unpack_from("<hh", lump.data, offset + 12)
            patch_count = struct.unpack_from("<h", lump.data, offset + 20)[0]
            require(width > 0 and height > 0, f"{lump.name} texture {name}: non-positive dimensions")
            require(patch_count >= 0, f"{lump.name} texture {name}: negative patch count")
            require(
                offset + 22 + patch_count * 10 <= lump.size,
                f"{lump.name} texture {name}: patch records are out of bounds",
            )
            dimensions = (width, height)
            require(
                name not in textures or textures[name] == dimensions,
                f"IWAD: conflicting duplicate texture definition for {name}",
            )
            textures[name] = dimensions
    require(bool(textures), "IWAD: no TEXTURE1/TEXTURE2 definitions found")
    return textures


def parse_flat_names(lumps: Sequence[Lump]) -> set[str]:
    starts = [index for index, lump in enumerate(lumps) if lump.name == "F_START"]
    ends = [index for index, lump in enumerate(lumps) if lump.name == "F_END"]
    require(len(starts) == 1 and len(ends) == 1, "IWAD: expected one F_START/F_END namespace")
    require(starts[0] < ends[0], "IWAD: F_END precedes F_START")
    return {lump.name for lump in lumps[starts[0] + 1 : ends[0]] if lump.size > 0}


def validate_iwad(
    path: Path, required_textures: set[str], required_flats: set[str]
) -> dict[str, tuple[int, int]]:
    _, lumps = parse_wad(path, b"IWAD")
    lump_names = {lump.name for lump in lumps}
    textures = parse_texture_definitions(lumps)
    flat_names = parse_flat_names(lumps)
    missing_textures = sorted(required_textures - set(textures))
    missing_flats = sorted(required_flats - flat_names)
    require(not missing_textures, f"IWAD: missing textures: {', '.join(missing_textures)}")
    require(not missing_flats, f"IWAD: missing flats: {', '.join(missing_flats)}")
    require("SKY1" in textures, "IWAD: missing episode-one SKY1 texture")
    require("F_SKY1" in flat_names, "IWAD: missing F_SKY1 sky flat")
    require("E1M1" in lump_names, "IWAD: missing stock E1M1 intro destination")
    require("E1M2" in lump_names, "IWAD: missing stock E1M2 campaign map")
    for sprite in ("STIMA0", "MEDIA0", "BON1A0", "BON2A0", "ARM1A0",
                   "SHOTA0", "MGUNA0", "CSAWA0", "CLIPA0", "AMMOA0", "SBOXA0"):
        require(sprite in lump_names, f"IWAD: missing pickup sprite {sprite}")
    require(textures.get("SW1EXIT") == (32, 72),
            f"IWAD: SW1EXIT must be 32x72, found {textures.get('SW1EXIT')}")
    require(textures.get("SW2EXIT") == (32, 72),
            f"IWAD: SW2EXIT must be 32x72, found {textures.get('SW2EXIT')}")
    return textures


def validate_sector_boundaries(
    linedefs: Sequence[Sequence[int]], sidedefs: Sequence[Sequence[int | bytes]], sector_count: int
) -> None:
    boundary_graphs: list[dict[int, int]] = [defaultdict(int) for _ in range(sector_count)]
    for line in linedefs:
        v1, v2, _, _, _, side0, side1 = line
        sector0 = int(sidedefs[side0][5])
        sector1 = int(sidedefs[side1][5]) if side1 != NO_SIDE else None
        if sector1 is None or sector1 != sector0:
            boundary_graphs[sector0][v1] += 1
            boundary_graphs[sector0][v2] += 1
        if sector1 is not None and sector1 != sector0:
            boundary_graphs[sector1][v1] += 1
            boundary_graphs[sector1][v2] += 1
    for sector, graph in enumerate(boundary_graphs):
        require(bool(graph), f"sector {sector}: no boundary")
        bad = sorted(vertex for vertex, degree in graph.items() if degree != 2)
        require(not bad, f"sector {sector}: boundary is not closed at vertices {bad}")


def validate_blockmap(
    lump: Lump, vertices: Sequence[tuple[int, int]], linedefs: Sequence[Sequence[int]]
) -> None:
    require(lump.size >= 8 and lump.size % 2 == 0, "BLOCKMAP: invalid byte size")
    origin_x, origin_y, width, height = struct.unpack_from("<hhHH", lump.data)
    require(width > 0 and height > 0, "BLOCKMAP: zero dimensions")
    cell_count = width * height
    require(8 + cell_count * 2 <= lump.size, "BLOCKMAP: truncated offset table")
    word_count = lump.size // 2
    require(word_count < 65536, "BLOCKMAP: exceeds classic 65,535-word limit")
    offsets = struct.unpack_from(f"<{cell_count}H", lump.data, 8)
    line_segments = [(vertices[line[0]], vertices[line[1]]) for line in linedefs]
    for cell, offset in enumerate(offsets):
        require(offset >= 4 + cell_count and offset < word_count, f"BLOCKMAP cell {cell}: bad offset")
        position = offset
        require(struct.unpack_from("<H", lump.data, position * 2)[0] == 0, f"BLOCKMAP cell {cell}: missing leading zero")
        position += 1
        entries: list[int] = []
        while position < word_count:
            value = struct.unpack_from("<H", lump.data, position * 2)[0]
            position += 1
            if value == 0xFFFF:
                break
            require(value < len(linedefs), f"BLOCKMAP cell {cell}: invalid linedef {value}")
            entries.append(value)
        else:
            raise ValidationError(f"BLOCKMAP cell {cell}: missing 0xffff sentinel")
        require(entries == sorted(set(entries)), f"BLOCKMAP cell {cell}: lines are not unique and sorted")
        row, column = divmod(cell, width)
        left = origin_x + column * 128
        bottom = origin_y + row * 128
        right = left + 127
        top = bottom + 127
        expected = [
            index
            for index, (start, end) in enumerate(line_segments)
            if segment_intersects_rectangle(start, end, left, bottom, right, top)
        ]
        require(entries == expected, f"BLOCKMAP cell {cell}: line intersection list is incomplete or extraneous")


def validate_map(path: Path, iwad: Path | None) -> dict[str, int]:
    magic, all_lumps = parse_wad(path, b"PWAD")
    del magic
    forbidden = {"BEHAVIOR", "TEXTMAP", "XNOD", "ZNOD", "xNd4"}
    require(not (forbidden & {lump.name for lump in all_lumps}), "PWAD contains a non-classic map lump")
    markers = [index for index, lump in enumerate(all_lumps) if lump.name == "PSDINTRO"]
    require(len(markers) == 1, "PWAD must contain exactly one PSDINTRO marker")
    require(not any(lump.name == "E1M1" for lump in all_lumps), "PWAD must not replace stock E1M1")
    marker_index = markers[0]
    marker = all_lumps[marker_index]
    require(marker.size == 0, "PSDINTRO marker must be zero length")
    require(marker_index + 11 <= len(all_lumps), "PSDINTRO map lump set is truncated")
    map_lumps = all_lumps[marker_index + 1 : marker_index + 11]
    require([lump.name for lump in map_lumps] == MAP_LUMPS, "PSDINTRO does not have the exact classic ten-lump order")
    require(len(all_lumps) == 11, "PWAD contains unexpected lumps outside PSDINTRO")
    by_name = {lump.name: lump for lump in map_lumps}

    things = records(by_name["THINGS"], "<hhhhh", "THINGS")
    linedefs = records(by_name["LINEDEFS"], "<HHHHHHH", "LINEDEFS")
    sidedefs = records(by_name["SIDEDEFS"], "<hh8s8s8sH", "SIDEDEFS")
    vertices = [(int(x), int(y)) for x, y in records(by_name["VERTEXES"], "<hh", "VERTEXES")]
    segs = records(by_name["SEGS"], "<HHHHHH", "SEGS")
    subsectors = records(by_name["SSECTORS"], "<HH", "SSECTORS")
    nodes = records(by_name["NODES"], "<hhhhhhhhhhhhHH", "NODES")
    raw_sectors = records(by_name["SECTORS"], "<hh8s8shhh", "SECTORS")
    sectors = [
        (int(value[0]), int(value[1]), decode_name(value[2], f"sector {index} floor"), decode_name(value[3], f"sector {index} ceiling"), int(value[4]), int(value[5]), int(value[6]))
        for index, value in enumerate(raw_sectors)
    ]

    for name, values in (
        ("THINGS", things),
        ("LINEDEFS", linedefs),
        ("SIDEDEFS", sidedefs),
        ("VERTEXES", vertices),
        ("SEGS", segs),
        ("SSECTORS", subsectors),
        ("NODES", nodes),
        ("SECTORS", sectors),
    ):
        require(bool(values), f"{name}: no records")
    require(not by_name["NODES"].data.startswith((b"XNOD", b"ZNOD", b"xNd4")), "NODES: extended/compressed nodes are not allowed")

    decoded_sidedefs = []
    for index, value in enumerate(sidedefs):
        x_offset, y_offset, upper, lower, middle, sector = value
        require(sector < len(sectors), f"sidedef {index}: invalid sector {sector}")
        decoded_sidedefs.append(
            (
                int(x_offset),
                int(y_offset),
                decode_name(upper, f"sidedef {index} upper"),
                decode_name(lower, f"sidedef {index} lower"),
                decode_name(middle, f"sidedef {index} middle"),
                int(sector),
            )
        )
    sidedefs = decoded_sidedefs

    for index, line in enumerate(linedefs):
        v1, v2, flags, _, _, side0, side1 = line
        require(v1 < len(vertices) and v2 < len(vertices), f"linedef {index}: invalid vertex")
        require(v1 != v2 and vertices[v1] != vertices[v2], f"linedef {index}: zero length")
        require(side0 < len(sidedefs), f"linedef {index}: invalid side 0")
        require(side1 == NO_SIDE or side1 < len(sidedefs), f"linedef {index}: invalid side 1")
        require(bool(flags & ML_TWOSIDED) == (side1 != NO_SIDE), f"linedef {index}: two-sided flag/reference mismatch")

    seg_sector: list[int] = []
    for index, seg in enumerate(segs):
        v1, v2, _, line_index, side, _ = seg
        require(v1 < len(vertices) and v2 < len(vertices), f"seg {index}: invalid vertex")
        require(v1 != v2, f"seg {index}: zero length")
        require(line_index < len(linedefs), f"seg {index}: invalid linedef")
        require(side in (0, 1), f"seg {index}: invalid side {side}")
        line = linedefs[line_index]
        side_index = line[5 + side]
        require(side_index != NO_SIDE and side_index < len(sidedefs), f"seg {index}: missing referenced side")
        expected = (line[0], line[1]) if side == 0 else (line[1], line[0])
        require((v1, v2) == expected, f"seg {index}: endpoints do not match its normalized linedef side")
        seg_sector.append(int(sidedefs[side_index][5]))

    used_seg_indices: set[int] = set()
    subsector_vertices: list[set[int]] = []
    subsector_sectors: list[int] = []
    subsector_polygons: list[list[tuple[int, int]]] = []
    for index, (count, first) in enumerate(subsectors):
        require(count >= 3, f"subsector {index}: fewer than three segs")
        require(first + count <= len(segs), f"subsector {index}: seg range is out of bounds")
        indices = list(range(first, first + count))
        require(not (used_seg_indices & set(indices)), f"subsector {index}: overlapping seg range")
        used_seg_indices.update(indices)
        loop = [segs[seg_index] for seg_index in indices]
        for current, following in zip(loop, loop[1:] + loop[:1]):
            require(current[1] == following[0], f"subsector {index}: seg loop is not closed")
        polygon = [vertices[seg[0]] for seg in loop]
        require(signed_area(polygon) < 0, f"subsector {index}: seg loop is not clockwise")
        sector_set = {seg_sector[seg_index] for seg_index in indices}
        require(len(sector_set) == 1, f"subsector {index}: inconsistent front sectors")
        subsector_sectors.append(next(iter(sector_set)))
        subsector_vertices.append({seg[0] for seg in loop} | {seg[1] for seg in loop})
        subsector_polygons.append(polygon)
    require(used_seg_indices == set(range(len(segs))), "SSECTORS: not every seg belongs to exactly one subsector")

    for index, node in enumerate(nodes):
        x, y, dx, dy = node[:4]
        require(dx != 0 or dy != 0, f"node {index}: zero-length partition")
        del x, y
        for child in node[12:14]:
            if child & 0x8000:
                require((child & 0x7FFF) < len(subsectors), f"node {index}: invalid subsector child")
            else:
                require(child < index, f"node {index}: child node is not earlier in postorder")
    require(bool(nodes), "NODES: classic BSP root is missing")

    descendant_cache: dict[int, set[int]] = {}

    def child_subsectors(child: int) -> set[int]:
        if child & 0x8000:
            return {child & 0x7FFF}
        if child in descendant_cache:
            return descendant_cache[child]
        node = nodes[child]
        result = child_subsectors(int(node[12])) | child_subsectors(int(node[13]))
        descendant_cache[child] = result
        return result

    reached_nodes: set[int] = set()

    def visit_node(index: int) -> None:
        require(index not in reached_nodes, f"NODES: node {index} is referenced more than once")
        reached_nodes.add(index)
        for child in nodes[index][12:14]:
            if not (child & 0x8000):
                visit_node(int(child))

    visit_node(len(nodes) - 1)
    require(reached_nodes == set(range(len(nodes))), "NODES: final root does not reach every node")
    require(child_subsectors(len(nodes) - 1) == set(range(len(subsectors))), "NODES: final root does not reach every subsector")

    for index, node in enumerate(nodes):
        for child_number, child in enumerate(node[12:14]):
            descendants = child_subsectors(int(child))
            vertex_indices = set().union(*(subsector_vertices[value] for value in descendants))
            child_vertices = [vertices[value] for value in vertex_indices]
            top, bottom, left, right = node[4 + child_number * 4 : 8 + child_number * 4]
            require(top >= bottom and right >= left, f"node {index} child {child_number}: invalid bounding box")
            require(
                all(left <= x <= right and bottom <= y <= top for x, y in child_vertices),
                f"node {index} child {child_number}: bounding box misses descendant vertices",
            )
            sides = [point_side(node, value) for value in child_vertices]
            if child_number == 0:
                require(all(value <= 0 for value in sides), f"node {index}: child 0 is not on right/front side")
            else:
                require(all(value >= 0 for value in sides), f"node {index}: child 1 is not on left/back side")

    validate_sector_boundaries(linedefs, sidedefs, len(sectors))
    validate_blockmap(by_name["BLOCKMAP"], vertices, linedefs)
    expected_reject = (len(sectors) * len(sectors) + 7) // 8
    require(by_name["REJECT"].size == expected_reject, f"REJECT: expected {expected_reject} bytes")
    require(not any(by_name["REJECT"].data), "REJECT: expected a zero-filled visibility matrix")

    require(len(sectors) == 18, f"SECTORS: expected 18, found {len(sectors)}")
    expected_sectors = [
        (0, 256, "FLOOR4_8", "F_SKY1", 176, 0, 0),
        (16, 256, "FLAT5_4", "F_SKY1", 180, 0, 0),
        (32, 256, "FLAT5_4", "F_SKY1", 184, 0, 0),
        (48, 256, "FLAT5_4", "F_SKY1", 188, 0, 0),
        (64, 256, "FLAT5_4", "F_SKY1", 192, 0, 0),
        (64, 256, "FLOOR5_1", "F_SKY1", 192, 0, 0),
        (104, 256, "FLAT14", "F_SKY1", 192, 0, 0),
        (64, 64, "F_SKY1", "F_SKY1", 192, 0, 0),
    ] + [(64, 64, "FLAT14", "CEIL3_5", 160, 0, 0) for _ in range(8)] + [
        (64, 136, "FLOOR5_1", "CEIL3_5", 192, 0, 0),
        (64, 64, "FLAT14", "CEIL3_5", 160, 0, 0),
    ]
    require(sectors == expected_sectors,
            "SECTORS: heights, flats, lighting, or ordering differ from arena specification")
    require(all(sectors[index][3] == "F_SKY1" for index in range(7)),
            "playable arena sector ceiling is not F_SKY1")
    require(sectors[7][2:4] == ("F_SKY1", "F_SKY1"),
            "horizon does not render sky above and below")
    require(sectors[16][3] != "F_SKY1", "exit cavity ceiling must be enclosed")

    def subsector_indices_for_point(value: tuple[int, int]) -> set[int]:
        candidates: set[int] = set()
        for index, polygon in enumerate(subsector_polygons):
            signs = [
                (end[0] - start[0]) * (value[1] - start[1])
                - (end[1] - start[1]) * (value[0] - start[0])
                for start, end in zip(polygon, polygon[1:] + polygon[:1])
            ]
            if all(sign <= 0 for sign in signs):
                candidates.add(index)
        return candidates

    def sectors_for_point(value: tuple[int, int]) -> set[int]:
        return {subsector_sectors[index] for index in subsector_indices_for_point(value)}

    def unique_subsector(value: tuple[int, int], label: str) -> int:
        candidates = subsector_indices_for_point(value)
        require(bool(candidates), f"{label}: outside every subsector")
        candidate_sectors = {subsector_sectors[index] for index in candidates}
        require(len(candidate_sectors) == 1,
                f"{label}: lies on subsectors from multiple sectors {sorted(candidate_sectors)}")
        # Probes on a neutral BSP split can be contained by two convex leaves
        # in the same sector. Either leaf represents the same playable space.
        return min(candidates)

    line_owners: dict[int, dict[int, int]] = defaultdict(dict)
    for subsector_index, (count, first) in enumerate(subsectors):
        for seg_index in range(first, first + count):
            line_index = int(segs[seg_index][3])
            side = int(segs[seg_index][4])
            require(side not in line_owners[line_index],
                    f"linedef {line_index}: side {side} belongs to multiple subsectors")
            line_owners[line_index][side] = subsector_index

    adjacency_lines: dict[frozenset[int], list[int]] = defaultdict(list)
    subsector_graph: dict[int, set[int]] = defaultdict(set)
    for index, line in enumerate(linedefs):
        side0, side1 = int(line[5]), int(line[6])
        sector0 = int(sidedefs[side0][5])
        if side1 == NO_SIDE:
            require(sector0 == 7, f"linedef {index}: non-horizon one-sided boundary")
            require(bool(line[2] & ML_BLOCKING),
                    f"linedef {index}: outer horizon boundary is not solid")
            continue
        sector1 = int(sidedefs[side1][5])
        if sector0 != sector1:
            adjacency_lines[frozenset((sector0, sector1))].append(index)
        if 6 in (sector0, sector1):
            require(bool(line[2] & ML_BLOCKING), f"linedef {index}: rail boundary is not blocking")
        if 8 <= sector0 <= 15 or 8 <= sector1 <= 15:
            require(bool(line[2] & ML_BLOCKING), f"linedef {index}: pillar boundary is not blocking")
        if 17 in (sector0, sector1) and {sector0, sector1} != {5, 16}:
            require(bool(line[2] & ML_BLOCKING), f"linedef {index}: exit shell boundary is not blocking")
        owners = line_owners.get(index, {})
        require(set(owners) == {0, 1}, f"linedef {index}: two-sided line lacks two seg owners")
        if not (line[2] & ML_BLOCKING):
            for source_side, destination_side in ((0, 1), (1, 0)):
                source_sector = sector0 if source_side == 0 else sector1
                destination_sector = sector1 if source_side == 0 else sector0
                rise = sectors[destination_sector][0] - sectors[source_sector][0]
                opening = min(sectors[source_sector][1], sectors[destination_sector][1]) \
                    - max(sectors[source_sector][0], sectors[destination_sector][0])
                if rise <= 24 and opening >= 56:
                    subsector_graph[owners[source_side]].add(owners[destination_side])

    for lower in range(4):
        pair = frozenset((lower, lower + 1))
        require(pair in adjacency_lines, f"stairs: sectors {lower} and {lower + 1} do not meet")
        require(sectors[lower + 1][0] - sectors[lower][0] == 16,
                f"stairs: transition {lower} is not 16 units")
        require(all(not (linedefs[index][2] & ML_BLOCKING) for index in adjacency_lines[pair]),
                f"stairs: transition {lower} is blocked")
    require(frozenset((4, 5)) in adjacency_lines,
            "stairs do not connect to the elevated walkway")

    def reachable_from(start: int, graph: dict[int, set[int]]) -> set[int]:
        reached = {start}
        queue = deque([start])
        while queue:
            source = queue.popleft()
            for destination in graph.get(source, set()):
                if destination not in reached:
                    reached.add(destination)
                    queue.append(destination)
        return reached

    center_x, center_y = 2080, -3432
    player = [value for value in things if int(value[3]) == 1]
    require(len(player) == 1, "THINGS: expected exactly one player start")
    player = player[0]
    require(player == (2080, -2632, 270, 1, 0x0007),
            "THINGS: player 1 start differs from the north walkway position facing the arena")
    player_subsector = unique_subsector((int(player[0]), int(player[1])), "player start")
    require(subsector_sectors[player_subsector] == 5,
            "player start is not on the elevated walkway")
    player_reachable = reachable_from(player_subsector, subsector_graph)

    stair_chains = [
        [(0, 300), (0, 416), (0, 480), (0, 544), (0, 608), (0, 736)],
        [(250, 250), (312, 312), (360, 360), (408, 408), (456, 456), (528, 528)],
        [(300, 0), (416, 0), (480, 0), (544, 0), (608, 0), (736, 0)],
        [(250, -250), (312, -312), (360, -360), (408, -408), (456, -456), (528, -528)],
        [(0, -300), (0, -416), (0, -480), (0, -544), (0, -608), (0, -736)],
        [(-250, -250), (-312, -312), (-360, -360), (-408, -408), (-456, -456), (-528, -528)],
        [(-300, 0), (-416, 0), (-480, 0), (-544, 0), (-608, 0), (-736, 0)],
        [(-250, 250), (-312, 312), (-360, 360), (-408, 408), (-456, 456), (-528, 528)],
    ]
    ring_side_subsectors: list[int] = []
    for chain_index, relative_chain in enumerate(stair_chains):
        chain = [(center_x + x, center_y + y) for x, y in relative_chain]
        chain_subsectors = [
            unique_subsector(value, f"stair chain {chain_index} probe {probe_index}")
            for probe_index, value in enumerate(chain)
        ]
        require([subsector_sectors[value] for value in chain_subsectors] == [0, 1, 2, 3, 4, 5],
                f"stair chain {chain_index}: floor sequence is not 0/16/32/48/64/64")
        for step, (source, destination) in enumerate(zip(chain_subsectors, chain_subsectors[1:])):
            require(destination in reachable_from(source, subsector_graph),
                    f"stair chain {chain_index}: transition {step} is not traversable")
        require(chain_subsectors[-1] in player_reachable,
                f"stair chain {chain_index}: walkway is not reachable from the player")
        ring_side_subsectors.append(chain_subsectors[-1])

    pillar_centers = [
        (-304, 608), (304, 608), (608, 304), (608, -304),
        (304, -608), (-304, -608), (-608, -304), (-608, 304),
    ]
    pillar_counts = Counter(subsector_sectors)
    landing_subsectors: list[int] = []
    for pillar_offset, (relative_x, relative_y) in enumerate(pillar_centers):
        pillar = 8 + pillar_offset
        if abs(relative_y) > abs(relative_x):
            half_width, half_height = 32, 16
            tangential_span = 2 * (2 * half_width + half_height)
        else:
            half_width, half_height = 16, 32
            tangential_span = 2 * (half_width + 2 * half_height)
        left = center_x + relative_x - half_width
        right = center_x + relative_x + half_width
        bottom = center_y + relative_y - half_height
        top = center_y + relative_y + half_height
        require(pillar_counts[pillar] >= 1, f"pillar sector {pillar}: no subsector")
        exposed: list[tuple[tuple[int, int], tuple[int, int]]] = []
        for line in linedefs:
            if line[6] == NO_SIDE:
                continue
            sector0 = int(sidedefs[line[5]][5])
            sector1 = int(sidedefs[line[6]][5])
            if pillar not in (sector0, sector1) or sector0 == sector1:
                continue
            other = sector1 if sector0 == pillar else sector0
            require(other == 5, f"pillar sector {pillar}: touches non-landing sector {other}")
            require(bool(line[2] & ML_BLOCKING), f"pillar sector {pillar}: exposed face is not solid")
            exposed.append((vertices[line[0]], vertices[line[1]]))
        require(exposed, f"pillar sector {pillar}: no exposed faces")
        intervals: dict[str, list[tuple[int, int]]] = defaultdict(list)
        for start, end in exposed:
            if start[1] == end[1] == bottom:
                intervals["bottom"].append(tuple(sorted((start[0], end[0]))))
            elif start[1] == end[1] == top:
                intervals["top"].append(tuple(sorted((start[0], end[0]))))
            elif start[0] == end[0] == left:
                intervals["left"].append(tuple(sorted((start[1], end[1]))))
            elif start[0] == end[0] == right:
                intervals["right"].append(tuple(sorted((start[1], end[1]))))
            else:
                raise ValidationError(
                    f"pillar sector {pillar}: non-rectangular exposed segment {start}->{end}"
                )
        for face in ("bottom", "top"):
            cursor = left
            for interval_start, interval_end in sorted(intervals[face]):
                require(interval_start == cursor and interval_end > interval_start,
                        f"pillar sector {pillar}: {face} face is gapped or overlapping")
                cursor = interval_end
            require(cursor == right, f"pillar sector {pillar}: incomplete {face} face")
        for face in ("left", "right"):
            cursor = bottom
            for interval_start, interval_end in sorted(intervals[face]):
                require(interval_start == cursor and interval_end > interval_start,
                        f"pillar sector {pillar}: {face} face is gapped or overlapping")
                cursor = interval_end
            require(cursor == top, f"pillar sector {pillar}: incomplete {face} face")
        require(right - left == 2 * half_width and top - bottom == 2 * half_height,
                f"pillar sector {pillar}: incorrect cover dimensions")
        require(tangential_span == 160,
                f"pillar sector {pillar}: center-facing cover span is not widened")
        require(64 - half_width >= 32 and 64 - half_height >= 32,
                f"pillar sector {pillar}: landing clearance is too narrow")
        for dx, dy in ((40, 0), (-40, 0), (0, 40), (0, -40)):
            probe = (center_x + relative_x + dx, center_y + relative_y + dy)
            landing = unique_subsector(probe, f"pillar sector {pillar} landing ring")
            require(subsector_sectors[landing] == 5,
                    f"pillar sector {pillar}: playable landing perimeter is not floor 64")
            landing_subsectors.append(landing)

    for first_index, first in enumerate(pillar_centers):
        for second in pillar_centers[first_index + 1:]:
            require((first[0] - second[0]) ** 2 + (first[1] - second[1]) ** 2 >= 384 ** 2,
                    "pillar footprints are not separated")

    ring_nodes = {
        index for index, sector in enumerate(subsector_sectors)
        if sectors[sector][0] == 64 and sector in (5, 16)
    }
    ring_graph: dict[int, set[int]] = defaultdict(set)
    for source in ring_nodes:
        ring_graph[source] = subsector_graph.get(source, set()) & ring_nodes
    ring_component = reachable_from(ring_side_subsectors[0], ring_graph)
    require(all(value in ring_component for value in ring_side_subsectors + landing_subsectors),
            "floor-64 walkway, side sections, and pillar landings do not form one circulation ring")

    pickup_specs = [
        (2011, -344, 0, "lower"), (2011, 344, 0, "lower"),
        (2011, 0, -344, "lower"), (2012, -240, 320, "lower"),
        (2012, 240, 320, "lower"), (2001, 0, 608, "stair"),
        (2018, 608, 0, "stair"), (2015, 456, 456, "stair"),
        (2015, 456, -456, "stair"), (2015, -456, -456, "stair"),
        (2015, -456, 456, "stair"), (2005, -800, 0, "walkway"),
        (2002, 0, -800, "walkway"), (2049, 160, 736, "walkway"),
        (2048, 160, -800, "walkway"), (2007, -736, 160, "walkway"),
        (2007, 736, 160, "walkway"), (2012, 736, -160, "walkway"),
        (2014, -736, -160, "walkway"),
    ]
    expected_pickups = Counter(
        (center_x + x, center_y + y, 0, kind, 0x0007)
        for kind, x, y, _ in pickup_specs
    )
    actual_pickups = Counter(tuple(int(field) for field in value) for value in things if int(value[3]) != 1)
    require(actual_pickups == expected_pickups,
            f"THINGS: pickup inventory or placement differs: {dict(actual_pickups)}")
    require(len(actual_pickups) == 19, "THINGS: pickup positions are not unique")
    allowed_types = {2001, 2002, 2005, 2007, 2011, 2012, 2014, 2015, 2018, 2048, 2049}
    require(set(value[3] for value in actual_pickups) <= allowed_types,
            "THINGS: pickup type is not stock Doom Shareware-compatible")
    expected_zone_sector = {"lower": 0, "stair": 4, "walkway": 5}
    for kind, relative_x, relative_y, zone in pickup_specs:
        position = (center_x + relative_x, center_y + relative_y)
        require(sectors_for_point(position) == {expected_zone_sector[zone]},
                f"pickup type {kind} at {position}: not in expected {zone} zone")
        require(not any(
            abs(relative_x - pillar_x) <= 64 and abs(relative_y - pillar_y) <= 64
            for pillar_x, pillar_y in pillar_centers
        ), f"pickup type {kind} at {position}: intrudes on a pillar landing")
        require(not (-48 <= relative_x <= 48 and 1024 <= relative_y <= 1120),
                f"pickup type {kind} at {position}: intrudes on the exit cavity")

    grid_points = [
        (1800 + column * 56, -3600 + row * 56)
        for row in range(7)
        for column in range(11)
    ]
    require(len(grid_points) == 77 and len(set(grid_points)) == 77,
            "process grid does not contain 77 unique cells")
    require(sectors[0][1] - sectors[0][0] >= 128,
            "process grid sector lacks adequate vertical clearance")
    radius = 24
    surrounding_offsets = [
        (0, 0), (radius, 0), (-radius, 0), (0, radius), (0, -radius),
        (17, 17), (17, -17), (-17, 17), (-17, -17),
    ]
    blocking_segments = [
        (vertices[line[0]], vertices[line[1]])
        for line in linedefs if line[2] & ML_BLOCKING
    ]
    static_clearance = radius + 20
    for cell_index, grid_point in enumerate(grid_points):
        require(all(
            sectors_for_point((grid_point[0] + dx, grid_point[1] + dy)) == {0}
            for dx, dy in surrounding_offsets
        ), f"process grid cell {cell_index}: cell or 24-unit surroundings leave the lower arena")
        require(all(
            squared_distance_to_segment(grid_point, start, end) > radius * radius
            for start, end in blocking_segments
        ), f"process grid cell {cell_index}: blocking linedef intrudes on spawn clearance")
        require(all(
            (grid_point[0] - int(thing[0])) ** 2 + (grid_point[1] - int(thing[1])) ** 2
            > static_clearance * static_clearance for thing in things
        ), f"process grid cell {cell_index}: static thing intrudes on spawn clearance")
    for index, first in enumerate(grid_points):
        require(all(
            (first[0] - second[0]) ** 2 + (first[1] - second[1]) ** 2 >= (2 * radius) ** 2
            for second in grid_points[index + 1:]
        ), f"process grid cell {index}: neighboring cells overlap")
    for thing in things[1:]:
        position = (int(thing[0]), int(thing[1]))
        require(all(
            squared_distance_to_segment(position, start, end) > 20 * 20
            for start, end in blocking_segments
        ), f"pickup type {thing[3]} at {position}: too close to blocking geometry")

    exits = [(index, line) for index, line in enumerate(linedefs) if line[3] == 11]
    require(len(exits) == 1, f"LINEDEFS: expected one special-11 exit, found {len(exits)}")
    exit_index, exit_line = exits[0]
    exit_start, exit_end = vertices[exit_line[0]], vertices[exit_line[1]]
    require((exit_start, exit_end) == ((2064, -2344), (2096, -2344)),
            f"linedef {exit_index}: switch endpoints/orientation are incorrect")
    require(exit_line[2] == (ML_BLOCKING | ML_TWOSIDED | ML_DONTPEGTOP),
            f"linedef {exit_index}: switch flags are incorrect")
    require(exit_line[4] == 0, f"linedef {exit_index}: switch tag is not zero")
    require(exit_line[6] != NO_SIDE, f"linedef {exit_index}: switch has no back side")
    exit_front_side = sidedefs[exit_line[5]]
    exit_back_side = sidedefs[exit_line[6]]
    require(exit_front_side[5] == 16 and exit_back_side[5] == 17,
            f"linedef {exit_index}: switch does not face cavity sector 16 with shell sector 17 behind")
    require(exit_front_side[:3] == (0, 0, "SW1EXIT"),
            f"linedef {exit_index}: switch texture or offsets are incorrect")
    sw1_references = sum(
        1 for side in sidedefs for field in (2, 3, 4) if side[field] == "SW1EXIT"
    )
    require(sw1_references == 1,
            f"SIDEDEFS: expected exactly one SW1EXIT reference, found {sw1_references}")
    switch_width = int(round(math.dist(exit_start, exit_end)))
    switch_height = sectors[16][1] - sectors[17][1]
    require((switch_width, switch_height) == (32, 72),
            f"exit switch surface is {switch_width}x{switch_height}, expected 32x72")

    entrance_endpoints = frozenset(((2056, -2408), (2104, -2408)))
    entrances = []
    for index, line in enumerate(linedefs):
        if frozenset((vertices[line[0]], vertices[line[1]])) != entrance_endpoints:
            continue
        sector0 = int(sidedefs[line[5]][5])
        sector1 = int(sidedefs[line[6]][5]) if line[6] != NO_SIDE else -1
        if {sector0, sector1} == {5, 16}:
            entrances.append((index, line))
    require(len(entrances) == 1, f"exit cavity: expected one walkway entrance, found {len(entrances)}")
    entrance_index, entrance_line = entrances[0]
    require(entrance_line[2] == ML_TWOSIDED,
            f"linedef {entrance_index}: cavity entrance is blocked or has unexpected flags")
    entrance_width = int(round(math.dist(vertices[entrance_line[0]], vertices[entrance_line[1]])))
    entrance_opening = min(sectors[5][1], sectors[16][1]) - max(sectors[5][0], sectors[16][0])
    require((entrance_width, entrance_opening) == (48, 72),
            f"exit cavity entrance is {entrance_width}x{entrance_opening}, expected 48x72")

    cavity_boundaries = []
    for index, line in enumerate(linedefs):
        if line[6] == NO_SIDE:
            continue
        sector0 = int(sidedefs[line[5]][5])
        sector1 = int(sidedefs[line[6]][5])
        if 16 in (sector0, sector1) and sector0 != sector1:
            cavity_boundaries.append((index, line, sector1 if sector0 == 16 else sector0))
    require(cavity_boundaries, "exit cavity has no enclosure boundaries")
    for index, line, other in cavity_boundaries:
        if index == entrance_index:
            require(other == 5 and not (line[2] & ML_BLOCKING),
                    "exit cavity entrance is not its sole open walkway portal")
        else:
            require(other == 17 and bool(line[2] & ML_BLOCKING),
                    f"linedef {index}: cavity side/back is not enclosed by the solid shell")
    require(all(other != 7 for _, _, other in cavity_boundaries),
            "exit cavity has a portal to the sky horizon")
    cavity_probe = unique_subsector((2080, -2376), "exit cavity use position")
    require(subsector_sectors[cavity_probe] == 16,
            "exit cavity use position is outside cavity sector 16")
    require(cavity_probe in player_reachable, "exit cavity is not reachable from the player start")
    require(squared_distance_to_segment((2080, -2376), exit_start, exit_end) <= 64 * 64,
            "exit switch is outside Doom's 64-unit front-use range")
    require(not any(subsector_sectors[index] == 7 for index in player_reachable),
            "playable portal graph reaches the sky horizon")

    map_textures = {
        str(side[field]) for side in sidedefs for field in (2, 3, 4) if side[field] != "-"
    }
    map_flats = {sector[2] for sector in sectors} | {sector[3] for sector in sectors}
    required_textures = map_textures | {"SW1EXIT", "SW2EXIT", "SKY1"}
    iwad_textures: dict[str, tuple[int, int]] = {}
    if iwad is not None:
        iwad_textures = validate_iwad(iwad, required_textures, map_flats)
        require(iwad_textures["SW1EXIT"] == (switch_width, switch_height),
                "exit surface dimensions do not match independently parsed SW1EXIT")
        require(iwad_textures["SW2EXIT"] == (switch_width, switch_height),
                "exit surface dimensions do not match independently parsed SW2EXIT")

    return {
        "lumps": len(all_lumps),
        "things": len(things),
        "pickups": 19,
        "linedefs": len(linedefs),
        "sidedefs": len(sidedefs),
        "vertices": len(vertices),
        "segs": len(segs),
        "subsectors": len(subsectors),
        "nodes": len(nodes),
        "sectors": len(sectors),
        "pillars": 8,
        "stair_chains": 8,
        "spawn_cells": len(grid_points),
        "exits": 1,
        "SW1EXIT": f"{switch_width}x{switch_height}",
        "SW2EXIT": f"{switch_width}x{switch_height}",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wad", required=True, type=Path, help="PWAD to validate")
    parser.add_argument("--iwad", type=Path, help="optional Doom IWAD for resource validation")
    args = parser.parse_args()
    try:
        counts = validate_map(args.wad, args.iwad)
    except ValidationError as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1
    summary = ", ".join(f"{name}={value}" for name, value in counts.items())
    resources = f"; resources verified against {args.iwad}" if args.iwad else ""
    print(
        f"valid Doom-format PSDINTRO: classic BSP, blockmap/reject, closed sectors, "
        f"reachable exit ({summary}){resources}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
