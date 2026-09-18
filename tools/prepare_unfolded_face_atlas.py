#!/usr/bin/env python3
"""Create a flat 6x3 atlas from Easel's 18 cube-face material groups.

Each output quad keeps the matching face material name, while its UVs select
that tile's portion of one shared source image. The result is one continuous
artwork spread across all 18 face tiles instead of repeating or alternating
sources per face.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def parse_material_names(path: Path) -> list[str]:
    with path.open("rb") as handle:
        magic, version, _ = struct.unpack("<4sII", handle.read(12))
        if magic != b"glTF" or version != 2:
            raise ValueError(f"{path} is not a glTF 2.0 binary")
        json_length, json_type = struct.unpack("<I4s", handle.read(8))
        if json_type != b"JSON":
            raise ValueError("GLB JSON chunk is missing")
        document = json.loads(handle.read(json_length).decode("utf-8").rstrip("\x00 "))
    names = [material.get("name", "") for material in document.get("materials", [])]
    names = [name for name in names if name and name != "Structure"]
    if len(names) != 18:
        raise ValueError(f"Expected 18 cube-face materials, found {len(names)}")
    return names


def aligned_append(blob: bytearray, payload: bytes) -> tuple[int, int]:
    while len(blob) % 4:
        blob.append(0)
    offset = len(blob)
    blob.extend(payload)
    return offset, len(payload)


def write_glb(path: Path, document: dict, blob: bytearray) -> None:
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
    parser.add_argument("input", type=Path, help="Face-zoned GLB used for material names")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    names = parse_material_names(args.input)
    columns, rows = 6, 3
    cell = 0.5
    gap = 0.018
    tile = cell - gap
    atlas_width = columns * cell
    atlas_height = rows * cell

    blob = bytearray()
    buffer_views: list[dict] = []
    accessors: list[dict] = []
    primitives: list[dict] = []
    materials: list[dict] = []

    for index, name in enumerate(names):
        row, column = divmod(index, columns)
        center_x = (column + 0.5) * cell - atlas_width * 0.5
        center_y = atlas_height * 0.5 - (row + 0.5) * cell
        half = tile * 0.5
        positions = (
            center_x - half, center_y - half, 0.0,
            center_x + half, center_y - half, 0.0,
            center_x + half, center_y + half, 0.0,
            center_x - half, center_y + half, 0.0,
        )

        # OpenGL's texture origin is bottom-left. The top geometry row samples
        # the source's top third so the reconstructed image remains upright.
        u0, u1 = column / columns, (column + 1) / columns
        v0, v1 = (rows - row - 1) / rows, (rows - row) / rows
        uvs = (u0, v0, u1, v0, u1, v1, u0, v1)
        indices = (0, 1, 2, 0, 2, 3)

        pos_offset, pos_length = aligned_append(blob, struct.pack("<12f", *positions))
        pos_view = len(buffer_views)
        buffer_views.append(
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": pos_length, "target": 34962}
        )
        pos_accessor = len(accessors)
        accessors.append(
            {
                "bufferView": pos_view,
                "componentType": 5126,
                "count": 4,
                "type": "VEC3",
                "min": [center_x - half, center_y - half, 0.0],
                "max": [center_x + half, center_y + half, 0.0],
            }
        )

        uv_offset, uv_length = aligned_append(blob, struct.pack("<8f", *uvs))
        uv_view = len(buffer_views)
        buffer_views.append(
            {"buffer": 0, "byteOffset": uv_offset, "byteLength": uv_length, "target": 34962}
        )
        uv_accessor = len(accessors)
        accessors.append(
            {"bufferView": uv_view, "componentType": 5126, "count": 4, "type": "VEC2"}
        )

        idx_offset, idx_length = aligned_append(blob, struct.pack("<6H", *indices))
        idx_view = len(buffer_views)
        buffer_views.append(
            {"buffer": 0, "byteOffset": idx_offset, "byteLength": idx_length, "target": 34963}
        )
        idx_accessor = len(accessors)
        accessors.append(
            {"bufferView": idx_view, "componentType": 5123, "count": 6, "type": "SCALAR"}
        )

        material_index = len(materials)
        materials.append(
            {
                "name": name,
                "doubleSided": True,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 1.0,
                },
                "extras": {"easelAtlasRow": row, "easelAtlasColumn": column},
            }
        )
        primitives.append(
            {
                "attributes": {"POSITION": pos_accessor, "TEXCOORD_0": uv_accessor},
                "indices": idx_accessor,
                "material": material_index,
                "mode": 4,
            }
        )

    document = {
        "asset": {"version": "2.0", "generator": "Easel prepare_unfolded_face_atlas.py"},
        "scene": 0,
        "scenes": [{"name": "Unfolded Cube Face Atlas", "nodes": [0]}],
        "nodes": [{"name": "Unfolded Face Atlas", "mesh": 0}],
        "meshes": [{"name": "Unfolded Face Atlas", "primitives": primitives}],
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(blob)}],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_glb(args.output, document, blob)
    print(f"Wrote {args.output} with {len(primitives)} face tiles")


if __name__ == "__main__":
    main()
