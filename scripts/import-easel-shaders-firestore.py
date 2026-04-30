#!/usr/bin/env python3
"""Import local ShaderClaw/Easel shaders into Firestore for Easel clients.

Creates/updates a dedicated Firestore collection, defaulting to `easel_shaders`.
The document shape is intentionally direct: Easel can read the collection list
through the Firestore REST API and cache each shader locally.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional


def slug(value: str) -> str:
    value = value.strip().lower()
    value = re.sub(r"\.fs$|\.frag$|\.glsl$", "", value)
    value = re.sub(r"[^a-z0-9_-]+", "-", value)
    value = re.sub(r"-+", "-", value).strip("-")
    return value or "shader"


def parse_isf_metadata(source: str) -> dict[str, Any]:
    start = source.find("/*")
    end = source.find("*/", start + 2)
    if start < 0 or end < 0:
        return {}
    block = source[start + 2 : end].strip()
    brace = block.find("{")
    if brace >= 0:
        block = block[brace:]
    try:
        return json.loads(block)
    except Exception:
        return {}


def load_manifest(source_dir: Path) -> dict[str, dict[str, Any]]:
    manifest_path = source_dir / "manifest.json"
    if not manifest_path.exists():
        return {}
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file: dict[str, dict[str, Any]] = {}
    if isinstance(manifest, list):
        for item in manifest:
            if isinstance(item, dict) and item.get("file"):
                by_file[item["file"]] = item
    return by_file


def has_audio(metadata: dict[str, Any]) -> bool:
    return any(
        inp.get("TYPE") in {"audio", "audioFFT"}
        for inp in metadata.get("INPUTS", [])
        if isinstance(inp, dict)
    )


def shader_files(source_dir: Path, manifest: dict[str, dict[str, Any]]) -> list[Path]:
    files_by_name: dict[str, Path] = {}
    if manifest:
        for file_name, item in manifest.items():
            if item.get("type") == "scene":
                continue
            path = source_dir / item.get("folder", "") / file_name
            if path.suffix.lower() in {".fs", ".frag", ".glsl"} and path.exists():
                files_by_name[path.name] = path

    for path in source_dir.iterdir():
        if path.is_file() and path.suffix.lower() in {".fs", ".frag", ".glsl"}:
            files_by_name.setdefault(path.name, path)

    return sorted(files_by_name.values(), key=lambda p: p.name.lower())


def build_doc(source_dir: Path, path: Path, manifest_item: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    fragment = path.read_text(encoding="utf-8")
    metadata = parse_isf_metadata(fragment)
    categories = manifest_item.get("categories") or metadata.get("CATEGORIES") or []
    title = manifest_item.get("title") or metadata.get("DESCRIPTION") or path.stem
    shader_id = slug(path.stem)

    vertex = ""
    vs_path = path.with_suffix(".vs")
    if vs_path.exists():
        vertex = vs_path.read_text(encoding="utf-8")

    doc = {
        "id": shader_id,
        "title": title,
        "description": manifest_item.get("description") or metadata.get("DESCRIPTION", ""),
        "file": path.name,
        "type": manifest_item.get("type", "generator"),
        "categories": categories,
        "status": "published",
        "enabled": True,
        "hidden": bool(manifest_item.get("hidden", False)),
        "updatedAt": datetime.now(timezone.utc).isoformat(),
        "fragment": fragment,
        "vertex": vertex,
        "metadata": metadata,
        "library_entry": {
            "description": manifest_item.get("description") or metadata.get("DESCRIPTION", ""),
            "credit": metadata.get("CREDIT", ""),
            "categories": categories,
            "inputs": metadata.get("INPUTS", []),
            "has_audio": has_audio(metadata),
            "shader_type": manifest_item.get("type", "generator"),
            "source": "shaderclaw",
        },
        "source": "shaderclaw",
        "sourceId": str(manifest_item.get("id", "")),
        "sourcePath": str(path.relative_to(source_dir)),
    }
    return shader_id, doc


def default_credentials() -> Optional[Path]:
    env = os.environ.get("GOOGLE_APPLICATION_CREDENTIALS")
    if env and Path(env).exists():
        return Path(env)
    for candidate in [
        Path("/Users/etherealmachine/etherea-ai/firebase-service-account.json"),
        Path("/Users/etherealmachine/archive/firebase-service-account.json"),
    ]:
        if candidate.exists():
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Import ready shaders into Firestore.")
    parser.add_argument("--source-dir", type=Path, default=Path("~/shader-claw3/shaders"))
    parser.add_argument("--collection", default="easel_shaders")
    parser.add_argument("--credentials", type=Path, default=default_credentials())
    parser.add_argument(
        "--prune",
        action="store_true",
        help="Delete Firestore documents in the collection that are not present in the source shader set.",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not args.credentials:
        raise SystemExit("No Firebase credentials found. Set GOOGLE_APPLICATION_CREDENTIALS.")

    source_dir = args.source_dir.expanduser().resolve()
    manifest = load_manifest(source_dir)
    files = shader_files(source_dir, manifest)
    docs = [build_doc(source_dir, path, manifest.get(path.name, {})) for path in files]
    doc_ids = {doc_id for doc_id, _ in docs}

    print(f"Prepared {len(docs)} shaders for collection `{args.collection}`")
    if args.dry_run:
        for doc_id, doc in docs[:10]:
            print(f"  {doc_id}: {doc['title']} ({doc['file']})")
        if len(docs) > 10:
            print(f"  ... {len(docs) - 10} more")
        return 0

    import firebase_admin
    from firebase_admin import credentials, firestore

    if not firebase_admin._apps:
        cred = credentials.Certificate(str(args.credentials.expanduser().resolve()))
        firebase_admin.initialize_app(cred)

    db = firestore.client()
    batch = db.batch()
    pending = 0
    written = 0
    for doc_id, doc in docs:
        ref = db.collection(args.collection).document(doc_id)
        batch.set(ref, doc, merge=False)
        pending += 1
        written += 1
        if pending >= 400:
            batch.commit()
            batch = db.batch()
            pending = 0
    if pending:
        batch.commit()

    pruned = 0
    if args.prune:
        batch = db.batch()
        pending = 0
        for existing in db.collection(args.collection).stream():
            if existing.id in doc_ids:
                continue
            batch.delete(existing.reference)
            pending += 1
            pruned += 1
            if pending >= 400:
                batch.commit()
                batch = db.batch()
                pending = 0
        if pending:
            batch.commit()

    print(f"Wrote {written} shaders to Firestore collection `{args.collection}`")
    if args.prune:
        print(f"Pruned {pruned} stale shaders from Firestore collection `{args.collection}`")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
