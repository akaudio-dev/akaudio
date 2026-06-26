#!/usr/bin/env bash
# Download the official VCV Rack SDK so the plugin can be built WITHOUT a sibling
# Rack *source* checkout. The SDK ships everything the plugin link needs:
# include/, plugin.mk + friends, and libRack.dylib (the import library whose
# OpenSSL exports our TLS/SHA1 code resolves against at runtime).
#
# Usage:
#   tools/get_sdk.sh            # fetch into ../Rack-SDK (sibling of the repo)
#   tools/get_sdk.sh /some/dir  # fetch into a directory of your choosing
#
# Then build with:
#   make RACK_DIR=$(cd ../Rack-SDK && pwd)
# (the Makefile also auto-detects ../Rack-SDK — see Makefile).
set -euo pipefail

VERSION="${RACK_SDK_VERSION:-2.6.4}"

# Pick the SDK flavor for this host (Rack 2 ships per-OS/arch SDKs).
case "$(uname -s)" in
  Darwin) case "$(uname -m)" in
            arm64) PLAT="mac-arm64" ;;
            *)     PLAT="mac-x64" ;;
          esac ;;
  Linux)  PLAT="lin-x64" ;;
  MINGW*|MSYS*|CYGWIN*) PLAT="win-x64" ;;
  *) echo "Unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac

DEST="${1:-$(cd "$(dirname "$0")/../.." && pwd)/Rack-SDK}"
URL="https://vcvrack.com/downloads/Rack-SDK-${VERSION}-${PLAT}.zip"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Downloading $URL"
curl -fL -o "$TMP/sdk.zip" "$URL"
unzip -q "$TMP/sdk.zip" -d "$TMP"

rm -rf "$DEST"
mv "$TMP/Rack-SDK" "$DEST"
echo "Rack SDK ${VERSION} (${PLAT}) installed at: $DEST"
echo
echo "Build with:  make RACK_DIR=\"$DEST\""
