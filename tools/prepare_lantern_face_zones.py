#!/usr/bin/env python3
"""Split the Meshy butterfly lantern GLB into independently routable cube faces.

The source asset is a single primitive with positions only. Easel routes 3D
mesh content per glTF material group, so this tool classifies the three cube
bodies into six faces each, creates planar UVs for every face, and keeps the
rods/caps in an untextured ``Structure`` material. With ``--seal-faces``, an
inset textured panel is placed behind every cube side so openings in the cage
geometry cannot read as transparent.

The cube extents and bottom-cube rotation below are measurements of the source
asset named on the command line. Keeping the conversion explicit makes the
derived GLB reproducible without adding a heavyweight mesh-processing runtime.
"""

from __future__ import annotations

import argparse
import array
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class CubeSpec:
    name: str
    y_min: float
    y_max: float
    yaw_degrees: float


CUBES = (
    CubeSpec("Cube 1 (top)", 0.174, 0.497, 0.0),
    CubeSpec("Cube 2 (middle)", -0.433, -0.138, 0.0),
    CubeSpec("Cube 3 (bottom)", -0.953, -0.690, 32.0),
)

FACE_NAMES = (
    "Right (+X)",
    "Front (+Z)",
    "Left (-X)",
    "Back (-Z)",
    "Top (+Y)",
    "Bottom (-Y)",
)


def parse_glb(path: Path) -> tuple[dict, bytes]:
    with path.open("rb") as handle:
        magic, version, total_length = struct.unpack("<4sII", handle.read(12))
        if magic != b"glTF" or version != 2:
            raise ValueError(f"{path} is not a glTF 2.0 binary")
        json_length, json_type = struct.unpack("<I4s", handle.read(8))
        if json_type != b"JSON":
            raise ValueError("GLB JSON chunk is missing")
        document = json.loads(handle.read(json_length).decode("utf-8").rstrip("\x00 "))
        bin_length, bin_type = struct.unpack("<I4s", handle.read(8))
        if bin_type != b"BIN\x00":
            raise ValueError("GLB binary chunk is missing")
        blob = handle.read(bin_length)
        if total_length < 20 + json_length + 8 + bin_length:
            raise ValueError("GLB is truncated")
    return document, blob


def accessor_bytes(document: dict, blob: bytes, accessor_index: int) -> tuple[dict, memoryview]:
    accessor = document["accessors"][accessor_index]
    view = document["bufferViews"][accessor["bufferView"]]
    offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    length = view["byteLength"] - accessor.get("byteOffset", 0)
    return accessor, memoryview(blob)[offset : offset + length]


def append_aligned(target: bytearray, payload: bytes | bytearray | memoryview) -> tuple[int, int]:
    while len(target) % 4:
        target.append(0)
    offset = len(target)
    target.extend(payload)
    return offset, len(payload)


def local_xz(x: float, z: float, yaw_degrees: float) -> tuple[float, float]:
    angle = math.radians(yaw_degrees)
    cosine, sine = math.cos(angle), math.sin(angle)
    return x * cosine + z * sine, -x * sine + z * cosine


def world_xz(u: float, v: float, yaw_degrees: float) -> tuple[float, float]:
    """Inverse of ``local_xz`` for constructing panels on rotated cubes."""
    angle = math.radians(yaw_degrees)
    cosine, sine = math.cos(angle), math.sin(angle)
    return u * cosine - v * sine, u * sine + v * cosine


def write_glb(path: Path, document: dict, blob: bytes | bytearray) -> None:
    json_bytes = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    bin_bytes = bytes(blob)
    bin_bytes += b"\x00" * ((4 - len(bin_bytes) % 4) % 4)
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    with path.open("wb") as handle:
        handle.write(struct.pack("<4sII", b"glTF", 2, total))
        handle.write(struct.pack("<I4s", len(json_bytes), b"JSON"))
        handle.write(json_bytes)
        handle.write(struct.pack("<I4s", len(bin_bytes), b"BIN\x00"))
        handle.write(bin_bytes)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--seal-faces",
        action="store_true",
        help="add inset backfill panels so all six sides of every cube are opaque",
    )
    args = parser.parse_args()

    source, source_blob = parse_glb(args.input)
    if len(source.get("meshes", [])) != 1 or len(source["meshes"][0].get("primitives", [])) != 1:
        raise ValueError("Expected the original single-mesh, single-primitive Meshy export")
    primitive = source["meshes"][0]["primitives"][0]
    if set(primitive.get("attributes", {})) != {"POSITION"}:
        raise ValueError("Expected a positions-only source primitive")

    position_accessor, position_bytes = accessor_bytes(source, source_blob, primitive["attributes"]["POSITION"])
    index_accessor, index_bytes = accessor_bytes(source, source_blob, primitive["indices"])
    if position_accessor["componentType"] != 5126 or position_accessor["type"] != "VEC3":
        raise ValueError("Expected float32 VEC3 positions")
    if index_accessor["componentType"] != 5125 or index_accessor["type"] != "SCALAR":
        raise ValueError("Expected uint32 indices")

    position_count = position_accessor["count"]
    index_count = index_accessor["count"]
    positions = position_bytes[: position_count * 12]
    indices = index_bytes[: index_count * 4].cast("I")

    def position(vertex_index: int) -> tuple[float, float, float]:
        return struct.unpack_from("<fff", positions, vertex_index * 12)

    # Measure the working local-coordinate bounds for every cube body.
    cube_bounds: list[tuple[float, float, float, float, float, float]] = []
    for cube in CUBES:
        u_min = v_min = float("inf")
        u_max = v_max = float("-inf")
        found = False
        for vertex_index in range(position_count):
            x, y, z = position(vertex_index)
            if cube.y_min <= y <= cube.y_max:
                u, v = local_xz(x, z, cube.yaw_degrees)
                u_min, u_max = min(u_min, u), max(u_max, u)
                v_min, v_max = min(v_min, v), max(v_max, v)
                found = True
        if not found:
            raise ValueError(f"No vertices found for {cube.name}")
        cube_bounds.append((u_min, u_max, v_min, v_max, cube.y_min, cube.y_max))

    group_names = ["Structure"]
    group_roles = ["structure"]
    for cube_index, cube in enumerate(CUBES):
        for face_index, face_name in enumerate(FACE_NAMES):
            group_names.append(f"{cube.name} / {face_name}")
            group_roles.append("flux-in" if (cube_index + face_index) % 2 == 0 else "fluxrt-output")
    grouped_indices = [array.array("I") for _ in group_names]

    def classify_triangle(a: int, b: int, c: int) -> int:
        p0, p1, p2 = position(a), position(b), position(c)
        center_x = (p0[0] + p1[0] + p2[0]) / 3.0
        center_y = (p0[1] + p1[1] + p2[1]) / 3.0
        center_z = (p0[2] + p1[2] + p2[2]) / 3.0
        for cube_index, cube in enumerate(CUBES):
            if not (cube.y_min <= center_y <= cube.y_max):
                continue
            u, v = local_xz(center_x, center_z, cube.yaw_degrees)
            u_min, u_max, v_min, v_max, y_min, y_max = cube_bounds[cube_index]
            u_span = max(1e-9, u_max - u_min)
            v_span = max(1e-9, v_max - v_min)
            y_span = max(1e-9, y_max - y_min)
            distances = (
                abs(u_max - u) / u_span,
                abs(v_max - v) / v_span,
                abs(u - u_min) / u_span,
                abs(v - v_min) / v_span,
                abs(y_max - center_y) / y_span,
                abs(center_y - y_min) / y_span,
            )
            face_index = min(range(6), key=distances.__getitem__)
            return 1 + cube_index * 6 + face_index
        return 0

    for offset in range(0, index_count, 3):
        triangle = (indices[offset], indices[offset + 1], indices[offset + 2])
        group = classify_triangle(*triangle)
        grouped_indices[group].extend(triangle)

    output_blob = bytearray()
    buffer_views: list[dict] = []
    accessors: list[dict] = []
    output_primitives: list[dict] = []
    materials: list[dict] = []
    output_triangle_count = 0

    for group_index, (name, role, source_indices) in enumerate(
        zip(group_names, group_roles, grouped_indices)
    ):
        if not source_indices:
            continue
        local_map: dict[int, int] = {}
        local_positions = bytearray()
        local_uvs = bytearray()
        local_indices = array.array("I")
        out_min = [float("inf")] * 3
        out_max = [float("-inf")] * 3

        cube_index = (group_index - 1) // 6 if group_index else -1
        face_index = (group_index - 1) % 6 if group_index else -1

        def uv_for(x: float, y: float, z: float) -> tuple[float, float]:
            if cube_index < 0:
                source_min, source_max = position_accessor["min"], position_accessor["max"]
                return (
                    (x - source_min[0]) / max(1e-9, source_max[0] - source_min[0]),
                    (y - source_min[1]) / max(1e-9, source_max[1] - source_min[1]),
                )
            cube = CUBES[cube_index]
            u, v = local_xz(x, z, cube.yaw_degrees)
            u_min, u_max, v_min, v_max, y_min, y_max = cube_bounds[cube_index]
            nu = (u - u_min) / max(1e-9, u_max - u_min)
            nv = (v - v_min) / max(1e-9, v_max - v_min)
            ny = (y - y_min) / max(1e-9, y_max - y_min)
            if face_index == 0:  # +U, viewed from outside
                return 1.0 - nv, ny
            if face_index == 1:  # +V
                return nu, ny
            if face_index == 2:  # -U
                return nv, ny
            if face_index == 3:  # -V
                return 1.0 - nu, ny
            if face_index == 4:  # +Y
                return nu, nv
            return nu, 1.0 - nv  # -Y

        def sealed_panel() -> tuple[tuple[float, float, float], ...]:
            """A quad just inside this face, behind the source cage geometry."""
            cube = CUBES[cube_index]
            u_min, u_max, v_min, v_max, y_min, y_max = cube_bounds[cube_index]
            u_span, v_span, y_span = u_max - u_min, v_max - v_min, y_max - y_min
            u0, u1 = u_min + u_span * 0.025, u_max - u_span * 0.025
            v0, v1 = v_min + v_span * 0.025, v_max - v_span * 0.025
            y0, y1 = y_min + y_span * 0.025, y_max - y_span * 0.025
            u_in, v_in, y_in = u_span * 0.012, v_span * 0.012, y_span * 0.012

            if face_index == 0:  # +U
                local = ((u_max - u_in, v1, y0), (u_max - u_in, v0, y0),
                         (u_max - u_in, v0, y1), (u_max - u_in, v1, y1))
            elif face_index == 1:  # +V
                local = ((u0, v_max - v_in, y0), (u1, v_max - v_in, y0),
                         (u1, v_max - v_in, y1), (u0, v_max - v_in, y1))
            elif face_index == 2:  # -U
                local = ((u_min + u_in, v0, y0), (u_min + u_in, v1, y0),
                         (u_min + u_in, v1, y1), (u_min + u_in, v0, y1))
            elif face_index == 3:  # -V
                local = ((u1, v_min + v_in, y0), (u0, v_min + v_in, y0),
                         (u0, v_min + v_in, y1), (u1, v_min + v_in, y1))
            elif face_index == 4:  # +Y
                local = ((u0, v1, y_max - y_in), (u1, v1, y_max - y_in),
                         (u1, v0, y_max - y_in), (u0, v0, y_max - y_in))
            else:  # -Y
                local = ((u0, v0, y_min + y_in), (u1, v0, y_min + y_in),
                         (u1, v1, y_min + y_in), (u0, v1, y_min + y_in))

            result = []
            for u, v, y in local:
                x, z = world_xz(u, v, cube.yaw_degrees)
                result.append((x, y, z))
            return tuple(result)

        for source_index in source_indices:
            mapped = local_map.get(source_index)
            if mapped is None:
                mapped = len(local_map)
                local_map[source_index] = mapped
                x, y, z = position(source_index)
                local_positions.extend(struct.pack("<fff", x, y, z))
                local_uvs.extend(struct.pack("<ff", *uv_for(x, y, z)))
                out_min[0], out_min[1], out_min[2] = min(out_min[0], x), min(out_min[1], y), min(out_min[2], z)
                out_max[0], out_max[1], out_max[2] = max(out_max[0], x), max(out_max[1], y), max(out_max[2], z)
            local_indices.append(mapped)

        if args.seal_faces and cube_index >= 0:
            panel_base = len(local_map)
            panel_uvs = ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))
            for (x, y, z), (u, v) in zip(sealed_panel(), panel_uvs):
                local_positions.extend(struct.pack("<fff", x, y, z))
                local_uvs.extend(struct.pack("<ff", u, v))
                out_min[0], out_min[1], out_min[2] = min(out_min[0], x), min(out_min[1], y), min(out_min[2], z)
                out_max[0], out_max[1], out_max[2] = max(out_max[0], x), max(out_max[1], y), max(out_max[2], z)
            local_indices.extend((panel_base, panel_base + 1, panel_base + 2,
                                  panel_base, panel_base + 2, panel_base + 3))

        position_offset, position_length = append_aligned(output_blob, local_positions)
        position_view = len(buffer_views)
        buffer_views.append(
            {"buffer": 0, "byteOffset": position_offset, "byteLength": position_length, "target": 34962}
        )
        position_out_accessor = len(accessors)
        accessors.append(
            {
                "bufferView": position_view,
                "componentType": 5126,
                "count": len(local_map) + (4 if args.seal_faces and cube_index >= 0 else 0),
                "type": "VEC3",
                "min": out_min,
                "max": out_max,
            }
        )

        uv_offset, uv_length = append_aligned(output_blob, local_uvs)
        uv_view = len(buffer_views)
        buffer_views.append(
            {"buffer": 0, "byteOffset": uv_offset, "byteLength": uv_length, "target": 34962}
        )
        uv_accessor = len(accessors)
        accessors.append(
            {
                "bufferView": uv_view,
                "componentType": 5126,
                "count": len(local_map) + (4 if args.seal_faces and cube_index >= 0 else 0),
                "type": "VEC2",
            }
        )

        index_offset, index_length = append_aligned(output_blob, local_indices.tobytes())
        index_view = len(buffer_views)
        buffer_views.append(
            {"buffer": 0, "byteOffset": index_offset, "byteLength": index_length, "target": 34963}
        )
        index_out_accessor = len(accessors)
        accessors.append(
            {"bufferView": index_view, "componentType": 5125, "count": len(local_indices), "type": "SCALAR"}
        )

        material_index = len(materials)
        materials.append(
            {
                "name": name,
                "doubleSided": True,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.08, 0.09, 0.12, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.8,
                },
                "extras": {"easelSourceRole": role},
            }
        )
        output_primitives.append(
            {
                "attributes": {"POSITION": position_out_accessor, "TEXCOORD_0": uv_accessor},
                "indices": index_out_accessor,
                "material": material_index,
                "mode": 4,
            }
        )
        print(
            f"{name}: {len(local_indices) // 3:,} triangles, "
            f"{len(local_map) + (4 if args.seal_faces and cube_index >= 0 else 0):,} vertices, role={role}"
        )
        output_triangle_count += len(local_indices) // 3

    output = {
        "asset": {"version": "2.0", "generator": "Easel prepare_lantern_face_zones.py"},
        "scene": 0,
        "scenes": [{"name": "Butterfly Lantern Face Zones", "nodes": [0]}],
        "nodes": [{"name": "Butterfly Lantern", "mesh": 0}],
        "meshes": [{"name": "Butterfly Lantern Face Zones", "primitives": output_primitives}],
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(output_blob)}],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_glb(args.output, output, output_blob)
    expected_triangles = index_count // 3 + (len(CUBES) * len(FACE_NAMES) * 2 if args.seal_faces else 0)
    if output_triangle_count != expected_triangles:
        raise RuntimeError(
            f"Triangle count mismatch: expected {expected_triangles:,}, got {output_triangle_count:,}"
        )
    print(
        f"Wrote {args.output} ({args.output.stat().st_size:,} bytes, "
        f"{output_triangle_count:,} triangles)"
    )


if __name__ == "__main__":
    main()
