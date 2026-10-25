#!/bin/bash
# Install an already-built (or downloaded) Ampex.vst3 / Ampex.component into the user plug-in folders.
# Usage: ./install-mac.sh                 (uses ./build/Ampex_artefacts/Release/...)
#        ./install-mac.sh path/to/Ampex.vst3
set -euo pipefail

VST3_SRC="${1:-./build/Ampex_artefacts/Release/VST3/Ampex.vst3}"
AU_SRC="./build/Ampex_artefacts/Release/AU/Ampex.component"
VST3_DEST="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DEST="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_DEST" "$AU_DEST"

install_bundle () {
    local src="$1" dest="$2"
    [ -d "$src" ] || { echo "skip (not found): $src"; return 0; }
    rm -rf "$dest/$(basename "$src")"
    ditto "$src" "$dest/$(basename "$src")"
    # Downloaded bundles carry the quarantine flag; strip it or Gatekeeper blocks the load.
    xattr -dr com.apple.quarantine "$dest/$(basename "$src")" 2>/dev/null || true
    # Make sure there is a valid signature (ad-hoc is enough on your own machine).
    if ! codesign --verify --deep --strict "$dest/$(basename "$src")" 2>/dev/null; then
        codesign --force --deep --sign - --timestamp=none "$dest/$(basename "$src")"
    fi
    echo "installed: $dest/$(basename "$src")"
}

install_bundle "$VST3_SRC" "$VST3_DEST"
install_bundle "$AU_SRC" "$AU_DEST"
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo
echo "Now in Cubase: Studio > VST Plug-in Manager > Rescan All (or restart Cubase)."
