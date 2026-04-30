#!/usr/bin/env python3
"""Package ready ShaderClaw shaders for the Easel shader library.

This script is intentionally backend-light: it emits the Easel shader-library
JSON contract or POSTs it to a gateway/Cloud Function that writes Firestore.
ShaderClaw can call this when a shader graduates from prototype to published.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import urllib.error
import urllib.request
from typing import Optional


def slug(value: str) -> str:
    value = value.strip().lower()
    value = re.sub(r"[^a-z0-9_-]+", "-", value)
    value = re.sub(r"-+", "-", value).strip("-")
    return value or "shader"


def read_manifest(source_dir: pathlib.Path) -> dict[str, dict]:
    manifest_path = source_dir / "manifest.json"
    if not manifest_path.exists():
        return {}

    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    by_file: dict[str, dict] = {}
    if isinstance(manifest, list):
        for item in manifest:
            if not isinstance(item, dict):
                continue
            file_name = item.get("file")
            if file_name:
                by_file[file_name] = item
    return by_file


def shader_path(source_dir: pathlib.Path, manifest_item: dict, file_name: str) -> pathlib.Path:
    folder = manifest_item.get("folder")
    if folder:
        return source_dir / folder / file_name
    return source_dir / file_name


def build_entry(source_dir: pathlib.Path, file_name: str, manifest_item: dict) -> dict:
    path = shader_path(source_dir, manifest_item, file_name)
    if not path.exists():
        raise FileNotFoundError(path)

    title = manifest_item.get("title") or path.stem
    shader_id = manifest_item.get("id") or slug(path.stem)
    entry = {
        "id": shader_id,
        "title": title,
        "description": manifest_item.get("description", ""),
        "type": manifest_item.get("type", "generator"),
        "categories": manifest_item.get("categories", []),
        "status": "published",
        "file": path.name,
        "fragment": path.read_text(encoding="utf-8"),
    }

    vs_path = path.with_suffix(".vs")
    if vs_path.exists():
        entry["vertex"] = vs_path.read_text(encoding="utf-8")
    return entry


def discover_files(source_dir: pathlib.Path, manifest: dict[str, dict]) -> list[str]:
    if manifest:
        return [
            file_name
            for file_name, item in manifest.items()
            if item.get("type") != "scene" and pathlib.Path(file_name).suffix in {".fs", ".frag", ".glsl"}
        ]
    return sorted(p.name for p in source_dir.iterdir() if p.suffix in {".fs", ".frag", ".glsl"})


def post_payload(endpoint: str, payload: dict, token: Optional[str]) -> None:
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    if token:
        request.add_header("Authorization", f"Bearer {token}")

    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            body = response.read().decode("utf-8", errors="replace")
            print(f"POST {endpoint} -> {response.status}", file=sys.stderr)
            if body:
                print(body, file=sys.stderr)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise SystemExit(f"POST failed: HTTP {exc.code}\n{body}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description="Publish ShaderClaw shaders to Easel library JSON.")
    parser.add_argument("--source-dir", required=True, type=pathlib.Path)
    parser.add_argument("--shader", action="append", default=[], help="Shader file to publish; may be repeated.")
    parser.add_argument("--all", action="store_true", help="Publish every non-scene shader in manifest.json.")
    parser.add_argument("--endpoint", help="Optional POST endpoint for a Firestore/gateway publisher.")
    parser.add_argument("--token", help="Optional bearer token for --endpoint.")
    parser.add_argument("--out", type=pathlib.Path, help="Write JSON payload to a file instead of stdout.")
    args = parser.parse_args()

    source_dir = args.source_dir.expanduser().resolve()
    manifest = read_manifest(source_dir)
    files = discover_files(source_dir, manifest) if args.all else args.shader
    if not files:
        parser.error("provide --shader FILE or --all")

    payload = {"shaders": []}
    for file_name in files:
        item = manifest.get(file_name, {})
        payload["shaders"].append(build_entry(source_dir, file_name, item))

    if args.endpoint:
        post_payload(args.endpoint, payload, args.token)
    elif args.out:
        args.out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    else:
        print(json.dumps(payload, indent=2))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
