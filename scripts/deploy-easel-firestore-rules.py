#!/usr/bin/env python3
"""Deploy scoped public-read rules for the Easel shader Firestore collection."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
from typing import Optional

import requests
from google.auth.transport.requests import Request
from google.oauth2 import service_account


RULES_TEMPLATE = """rules_version = '2';
service cloud.firestore {{
  match /databases/{{database}}/documents {{
    match /{collection}/{{shaderId}} {{
      allow list: if true;
      allow get: if resource.data.enabled == true
        && resource.data.status in ['published', 'ready', 'active'];
      allow write: if false;
    }}

    match /{{document=**}} {{
      allow read, write: if false;
    }}
  }}
}}
"""


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


def auth_headers(credentials_path: Path) -> dict[str, str]:
    creds = service_account.Credentials.from_service_account_file(
        str(credentials_path.expanduser().resolve()),
        scopes=["https://www.googleapis.com/auth/firebase"],
    )
    creds.refresh(Request())
    return {
        "Authorization": f"Bearer {creds.token}",
        "Content-Type": "application/json",
    }


def request_json(method: str, url: str, headers: dict[str, str], payload: dict | None = None) -> dict:
    response = requests.request(method, url, headers=headers, json=payload, timeout=20)
    if response.status_code >= 400:
        raise SystemExit(f"{method} {url} failed: HTTP {response.status_code}\n{response.text}")
    if not response.text:
        return {}
    return response.json()


def main() -> int:
    parser = argparse.ArgumentParser(description="Deploy Easel shader Firestore read rules.")
    parser.add_argument("--project", default="etherea-aa67d")
    parser.add_argument("--collection", default="easel_shaders")
    parser.add_argument("--credentials", type=Path, default=default_credentials())
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--force",
        action="store_true",
        help="Replace an existing Firestore rules release. Without this, the script only creates rules when no release exists.",
    )
    args = parser.parse_args()

    if not args.credentials:
        raise SystemExit("No Firebase credentials found. Set GOOGLE_APPLICATION_CREDENTIALS.")

    rules = RULES_TEMPLATE.format(collection=args.collection)
    print(rules)
    if args.dry_run:
        return 0

    headers = auth_headers(args.credentials)
    base = f"https://firebaserules.googleapis.com/v1/projects/{args.project}"
    releases = request_json("GET", f"{base}/releases", headers).get("releases", [])
    firestore_releases = [r for r in releases if "/releases/cloud.firestore" in r.get("name", "")]
    if firestore_releases and not args.force:
        raise SystemExit(
            "Firestore rules release already exists; rerun with --force after reviewing current rules."
        )

    fingerprint = base64.b64encode(hashlib.sha256(rules.encode("utf-8")).digest()).decode("ascii")
    ruleset = request_json(
        "POST",
        f"{base}/rulesets",
        headers,
        {
            "source": {
                "files": [
                    {
                        "name": "firestore.rules",
                        "content": rules,
                        "fingerprint": fingerprint,
                    }
                ]
            }
        },
    )
    ruleset_name = ruleset["name"]
    print(f"Created ruleset {ruleset_name}")

    release_name = f"projects/{args.project}/releases/cloud.firestore"
    if firestore_releases:
        request_json(
            "PATCH",
            f"https://firebaserules.googleapis.com/v1/{release_name}",
            headers,
            {
                "release": {"name": release_name, "rulesetName": ruleset_name},
                "updateMask": "ruleset_name",
            },
        )
        print(f"Updated release {release_name}")
    else:
        request_json(
            "POST",
            f"{base}/releases",
            headers,
            {"name": release_name, "rulesetName": ruleset_name},
        )
        print(f"Created release {release_name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
