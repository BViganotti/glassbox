#!/bin/sh
# Builds a local macOS application bundle. Ad-hoc signing works without an
# Apple Developer identity; set GBX_CODESIGN_IDENTITY to sign for distribution.
# Notarization still requires an Apple Developer account and is intentionally
# not automated here.
set -eu

BUILD=${1:-build}
DIST=${2:-dist}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BINARY="$ROOT/$BUILD/glassbox"
PLIST="$ROOT/packaging/Info.plist.in"
APP="$ROOT/$DIST/Glassbox.app"

if [ ! -x "$BINARY" ]; then
  echo "missing $BINARY; build first (make build)" >&2
  exit 1
fi

VERSION=$("$BINARY" --version | awk '{print $2}')
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BINARY" "$APP/Contents/MacOS/Glassbox"
sed "s/@VERSION@/$VERSION/g" "$PLIST" > "$APP/Contents/Info.plist"

IDENTITY=${GBX_CODESIGN_IDENTITY:--}
if command -v codesign >/dev/null 2>&1; then
  if codesign --force --deep --sign "$IDENTITY" "$APP" >/dev/null 2>&1; then
    echo "signed with identity: $IDENTITY"
  else
    echo "warning: codesign failed; the bundle will still run locally" >&2
  fi
fi
echo "created $APP (version $VERSION)"
