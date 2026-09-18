#!/usr/bin/env python3
"""Place Easel's unfolded lantern atlas beside the original 3D lantern.

The two input GLBs are combined without changing their geometry. Node
transforms arrange the 6x3 atlas on the left and a rotated, scaled lantern on
the right. Matching cube-face materials are shared, so both presentations use
the same per-face source routing inside Easel.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import struct
from pathlib import Path


def read_glb(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"{path} is too short to be a GLB")
    magic, version, declared_length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF" or version != 2 or declared_length != len(data):
        raise ValueError(f"{path} is not a valid glTF 2.0 binary")

    offset = 12
    document: dict | None = None
    binary = b""
    while offset + 8 <= len(data):
        chunk_length, chunk_type = struct.unpack_from("<I4s", data, offset)
        offset += 8
        payload = data[offset : offset + chunk_length]
        offset += chunk_length
        if chunk_type == b"JSON":
            document = json.loads(payload.decode("utf-8").rstrip("\x00 "))
        elif chunk_type == b"BIN\x00":
            binary = payload
    if document is None:
        raise ValueError(f"{path} has no JSON chunk")
    if len(document.get("buffers", [])) != 1:
        raise ValueError(f"{path} must contain exactly one embedded buffer")
    return document, binary


def write_glb(path: Path, document: dict, binary: bytes) -> None:
    document["buffers"] = [{"byteLength": len(binary)}]
    json_bytes = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    binary += b"\x00" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(json_bytes) + 8 + len(binary)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(struct.pack("<4sII", b"glTF", 2, total))
        handle.write(struct.pack("<I4s", len(json_bytes), b"JSON"))
        handle.write(json_bytes)
        handle.write(struct.pack("<I4s", len(binary), b"BIN\x00"))
        handle.write(binary)


def append_geometry(
    source: dict,
    blob_offset: int,
    buffer_views: list[dict],
    accessors: list[dict],
    materials_by_name: dict[str, int],
    combined_materials: list[dict],
) -> list[dict]:
    view_base = len(buffer_views)
    for source_view in source.get("bufferViews", []):
        view = copy.deepcopy(source_view)
        view["buffer"] = 0
        view["byteOffset"] = view.get("byteOffset", 0) + blob_offset
        buffer_views.append(view)

    accessor_base = len(accessors)
    for source_accessor in source.get("accessors", []):
        accessor = copy.deepcopy(source_accessor)
        if "bufferView" in accessor:
            accessor["bufferView"] += view_base
        accessors.append(accessor)

    material_map: dict[int, int] = {}
    for index, source_material in enumerate(source.get("materials", [])):
        name = source_material.get("name") or f"Material {index}"
        if name not in materials_by_name:
            materials_by_name[name] = len(combined_materials)
            combined_materials.append(copy.deepcopy(source_material))
        material_map[index] = materials_by_name[name]

    meshes: list[dict] = []
    for source_mesh in source.get("meshes", []):
        mesh = {key: copy.deepcopy(value) for key, value in source_mesh.items() if key != "primitives"}
        mesh["primitives"] = []
        for source_primitive in source_mesh.get("primitives", []):
            primitive = copy.deepcopy(source_primitive)
            primitive["attributes"] = {
                name: accessor + accessor_base
                for name, accessor in source_primitive.get("attributes", {}).items()
            }
            if source_primitive.get("indices", -1) >= 0:
                primitive["indices"] = source_primitive["indices"] + accessor_base
            if source_primitive.get("material", -1) >= 0:
                primitive["material"] = material_map[source_primitive["material"]]
            mesh["primitives"].append(primitive)
        meshes.append(mesh)
    return meshes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("atlas", type=Path, help="Unfolded 6x3 atlas GLB")
    parser.add_argument("lantern", type=Path, help="Face-zoned 3D lantern GLB")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    atlas, atlas_blob = read_glb(args.atlas)
    lantern, lantern_blob = read_glb(args.lantern)

    combined_blob = bytearray(atlas_blob)
    while len(combined_blob) % 4:
        combined_blob.append(0)
    lantern_blob_offset = len(combined_blob)
    combined_blob.extend(lantern_blob)

    buffer_views: list[dict] = []
    accessors: list[dict] = []
    materials: list[dict] = []
    materials_by_name: dict[str, int] = {}

    atlas_meshes = append_geometry(
        atlas, 0, buffer_views, accessors, materials_by_name, materials
    )
    lantern_meshes = append_geometry(
        lantern,
        lantern_blob_offset,
        buffer_views,
        accessors,
        materials_by_name,
        materials,
    )
    if len(atlas_meshes) != 1 or len(lantern_meshes) != 1:
        raise ValueError("Expected one mesh in each input GLB")

    # The atlas remains flat to camera. The lantern is turned around Y so its
    # front and side read clearly while it sits beside the atlas.
    lantern_angle = 0.65
    combined = {
        "asset": {"version": "2.0", "generator": "Easel prepare_lantern_atlas_with_3d.py"},
        "scene": 0,
        "scenes": [{"name": "Unfolded Atlas and 3D Lantern", "nodes": [0, 1]}],
        "nodes": [
            {
                "name": "Unfolded Face Atlas",
                "mesh": 0,
                "translation": [-0.48, 0.0, 0.0],
                "scale": [0.72, 0.72, 0.72],
            },
            {
                "name": "3D Butterfly Lantern",
                "mesh": 1,
                "translation": [0.94, 0.0, 0.0],
                "rotation": [0.0, math.sin(lantern_angle * 0.5), 0.0, math.cos(lantern_angle * 0.5)],
                "scale": [0.8, 0.8, 0.8],
            },
        ],
        "meshes": atlas_meshes + lantern_meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
    }
    write_glb(args.output, combined, bytes(combined_blob))
    print(
        f"Wrote {args.output} with {len(materials)} materials, "
        f"{len(atlas_meshes[0]['primitives'])} atlas primitives, and "
        f"{len(lantern_meshes[0]['primitives'])} 3D primitives"
    )


if __name__ == "__main__":
    main()
