# Easel Shader Library

Easel treats ShaderClaw as a prototyping/publishing tool. Runtime clients load
from an Easel-owned shader library cache and optionally sync that cache from a
remote endpoint.

## Client Configuration

By default, Easel reads from the `easel_shaders` Firestore collection in the
`etherea-aa67d` Firebase project:

```text
https://firestore.googleapis.com/v1/projects/etherea-aa67d/databases/(default)/documents/easel_shaders
```

Override with one of these environment variables before launching Easel:

```sh
export EASEL_SHADER_LIBRARY_URL="https://example.com/easel/shaders"
```

`EASEL_SHADER_FIRESTORE_URL` is also accepted. The URL may point at:

- an Easel JSON endpoint with a top-level `shaders` array
- a Firestore REST collection/list response with a top-level `documents` array

## Import Current ShaderClaw Shaders

```sh
/Users/etherealmachine/etherea-ai/.venv/bin/python \
  scripts/import-easel-shaders-firestore.py \
  --source-dir ~/shader-claw3/shaders \
  --collection easel_shaders \
  --prune
```

This creates/updates one Firestore document per shader and, with `--prune`,
removes Firestore documents that no longer exist in the source set. Firestore
creates the collection on first write.

To publish from ShaderClaw's remote `origin/master` without changing a local
ShaderClaw checkout:

```sh
tmp=$(mktemp -d /tmp/easel-shaderclaw-origin-master.XXXXXX)
git -C ~/shader-claw3 archive origin/master shaders | tar -x -C "$tmp"
/Users/etherealmachine/etherea-ai/.venv/bin/python \
  scripts/import-easel-shaders-firestore.py \
  --source-dir "$tmp/shaders" \
  --collection easel_shaders \
  --prune
```

## Firestore Read Access

Native Easel clients read the collection through Firestore REST, so the
collection must be readable by unauthenticated clients or by a public gateway.
The intended direct-read rule is:

```text
match /easel_shaders/{shaderId} {
  allow list: if true;
  allow get: if resource.data.enabled == true
    && resource.data.status in ['published', 'ready', 'active'];
  allow write: if false;
}
```

The helper below creates that ruleset and attaches it as the active Firestore
release when the credentials have Firebase Rules release permission:

```sh
/Users/etherealmachine/etherea-ai/.venv/bin/python \
  scripts/deploy-easel-firestore-rules.py \
  --project etherea-aa67d \
  --collection easel_shaders
```

Easel syncs in the background every 15 seconds and writes files into:

```text
~/.easel/shader-library/
```

Loaded shader layers watch those cached files, so a synced update hot-reloads
the running layer.

## Easel JSON Format

```json
{
  "shaders": [
    {
      "id": "text-wave",
      "title": "Text Wave",
      "description": "Audio-reactive text wave shader",
      "type": "generator",
      "categories": ["Text"],
      "status": "published",
      "updatedAt": "2026-04-29T00:00:00Z",
      "file": "text_wave.fs",
      "fragment": "/* ISF JSON */\nvoid main() { ... }",
      "vertex": "void main() { ... }"
    }
  ]
}
```

Instead of inline source, a publisher can provide `fragmentUrl` or `sourceUrl`.
Paired vertex code can be inline as `vertex`/`vs` or linked as `vertexUrl`.

## Firestore Fields

For direct Firestore REST collection reads, Easel recognizes the same logical
fields as typed Firestore values:

```text
id, title, description, type, categories, status, enabled,
updatedAt, file, fragment/source/code/fs, vertex/vs,
fragmentUrl/sourceUrl, vertexUrl
```

Only documents with no `status`, or `status` equal to `published`, `ready`, or
`active`, are shown. `enabled: false` hides a shader.

## ShaderClaw Publish Flow

1. Prototype in ShaderClaw.
2. When ready, publish the `.fs` and optional paired `.vs` plus metadata to the
   Easel shader endpoint or Firestore collection.
3. Easel clients receive the update on their next background sync and render
   from the local Easel cache.

The helper script can package one shader, all manifest shaders, or POST to a
publisher gateway:

```sh
scripts/publish-easel-shader.py \
  --source-dir ~/shader-claw3/shaders \
  --shader text_wave.fs \
  --endpoint https://example.com/easel/shaders/publish \
  --token "$EASEL_SHADER_PUBLISH_TOKEN"
```

The in-app `Shaders > ShaderClaw Import` control is only for local development
and cache seeding. It does not publish to the shared cloud library.
