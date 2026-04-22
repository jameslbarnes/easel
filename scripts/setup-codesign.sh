#!/bin/bash
# One-time setup: create a self-signed code-signing certificate named
# "Easel Dev" in the user's login keychain. The Easel CMake build uses it
# via POST_BUILD codesign so macOS TCC (Screen Recording / Microphone /
# Camera / Accessibility) permissions persist across rebuilds.
#
# Without this, every rebuild produces a new CDHash and macOS re-prompts
# for permissions every time the app launches.

set -e

CERT_NAME="Easel Dev"

if security find-certificate -c "$CERT_NAME" >/dev/null 2>&1; then
    echo "[setup-codesign] '$CERT_NAME' already exists in keychain — nothing to do."
    exit 0
fi

echo "[setup-codesign] Creating self-signed code-signing certificate '$CERT_NAME'..."

TMP=$(mktemp -d)
trap "rm -rf '$TMP'" EXIT

cat > "$TMP/openssl.cnf" <<'EOF'
[req]
distinguished_name = dn
x509_extensions    = v3_codesign
prompt             = no

[dn]
CN = Easel Dev
O  = Easel

[v3_codesign]
basicConstraints     = critical, CA:false
keyUsage             = critical, digitalSignature
extendedKeyUsage     = critical, codeSigning
subjectKeyIdentifier = hash
EOF

# 10-year self-signed code-signing cert
openssl req -x509 -new -nodes -newkey rsa:2048 \
    -keyout "$TMP/easel.key" \
    -out    "$TMP/easel.crt" \
    -sha256 -days 3650 \
    -config "$TMP/openssl.cnf" 2>/dev/null

openssl pkcs12 -export \
    -out "$TMP/easel.p12" \
    -inkey "$TMP/easel.key" \
    -in "$TMP/easel.crt" \
    -passout pass:easel 2>/dev/null

# Import into login keychain, grant codesign access without prompts.
security import "$TMP/easel.p12" \
    -k "$HOME/Library/Keychains/login.keychain-db" \
    -P easel \
    -T /usr/bin/codesign

echo "[setup-codesign] Done."
echo "[setup-codesign] The next build will be signed with '$CERT_NAME'."
echo "[setup-codesign] macOS will prompt for permissions once more (because the"
echo "                 signing identity changed); after that, grants persist."
