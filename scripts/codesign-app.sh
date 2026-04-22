#!/bin/bash
# Codesign Easel.app after build.
# - Uses the "Easel Dev" self-signed cert (if present) so TCC permissions
#   (Screen Recording / Mic / Camera) persist across rebuilds.
# - Falls back to ad-hoc signing ('-') if the cert is missing.
#
# Run scripts/setup-codesign.sh once to create the "Easel Dev" cert.

set -e

APP_PATH="$1"
if [ -z "$APP_PATH" ]; then
    echo "usage: $0 <path/to/Easel.app>" >&2
    exit 1
fi

CERT_NAME="Easel Dev"
if security find-certificate -c "$CERT_NAME" >/dev/null 2>&1; then
    codesign --force --deep --sign "$CERT_NAME" "$APP_PATH"
else
    codesign --force --deep --sign - "$APP_PATH"
fi
