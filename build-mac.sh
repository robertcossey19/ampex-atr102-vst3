#!/bin/bash
# ============================================================
# Ampex (ANALOGEXACT) - macOS build + sign + install
#
# Default: universal (arm64 + x86_64) VST3 / AU / Standalone,
#          ad-hoc signed, copied into ~/Library/Audio/Plug-Ins/.
#
# Optional Developer ID signing + notarization:
#   export AMPEX_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
#   export AMPEX_DEVELOPMENT_TEAM="TEAMID"
#   export AMPEX_NOTARY_PROFILE="ampex-notary"   # from: xcrun notarytool store-credentials
#   ./build-mac.sh
# ============================================================
set -euo pipefail

cd "$(dirname "$0")"

IDENTITY="${AMPEX_CODESIGN_IDENTITY:--}"      # "-" = ad-hoc
TEAM="${AMPEX_DEVELOPMENT_TEAM:-}"
NOTARY_PROFILE="${AMPEX_NOTARY_PROFILE:-}"
ARCHS="${AMPEX_ARCHS:-x86_64;arm64}"           # set AMPEX_ARCHS=arm64 for a faster native-only build

echo "Ampex - macOS build"
echo "==================="

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Xcode command line tools not found. Run:  xcode-select --install"
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    if command -v brew >/dev/null 2>&1; then
        echo "CMake not found - installing with Homebrew..."
        brew install cmake
    else
        echo "CMake not found. Install Homebrew (https://brew.sh) then:  brew install cmake"
        exit 1
    fi
fi

mkdir -p build
cd build

echo
echo "[1/4] Configure (Xcode generator, archs: $ARCHS, sign identity: $IDENTITY)"
cmake .. -G "Xcode" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="11.0" \
    -DAMPEX_CODESIGN_IDENTITY="$IDENTITY" \
    -DAMPEX_DEVELOPMENT_TEAM="$TEAM"

echo
echo "[2/4] Build Release"
cmake --build . --config Release --parallel

VST3="./Ampex_artefacts/Release/VST3/Ampex.vst3"
AU="./Ampex_artefacts/Release/AU/Ampex.component"
APP="./Ampex_artefacts/Release/Standalone/Ampex.app"

echo
echo "[3/4] Sign"
sign_bundle () {
    local bundle="$1"
    [ -d "$bundle" ] || return 0
    if [ "$IDENTITY" = "-" ]; then
        codesign --force --deep --sign - --timestamp=none "$bundle"
    else
        codesign --force --deep --sign "$IDENTITY" --timestamp --options runtime "$bundle"
    fi
    codesign --verify --deep --strict "$bundle"
    echo "  signed: $bundle"
}
sign_bundle "$VST3"
sign_bundle "$AU"
sign_bundle "$APP"

if [ -n "$NOTARY_PROFILE" ] && [ "$IDENTITY" != "-" ]; then
    echo
    echo "Notarizing VST3 (this talks to Apple, takes a few minutes)..."
    ditto -c -k --keepParent "$VST3" Ampex.vst3.zip
    xcrun notarytool submit Ampex.vst3.zip --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$VST3"
fi

echo
echo "[4/4] Install to ~/Library/Audio/Plug-Ins"
VST3_DEST="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DEST="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_DEST" "$AU_DEST"

if [ -d "$VST3" ]; then
    rm -rf "$VST3_DEST/Ampex.vst3"
    ditto "$VST3" "$VST3_DEST/Ampex.vst3"
    xattr -dr com.apple.quarantine "$VST3_DEST/Ampex.vst3" 2>/dev/null || true
    echo "  VST3 -> $VST3_DEST/Ampex.vst3   (this is what Cubase loads)"
fi
if [ -d "$AU" ]; then
    rm -rf "$AU_DEST/Ampex.component"
    ditto "$AU" "$AU_DEST/Ampex.component"
    xattr -dr com.apple.quarantine "$AU_DEST/Ampex.component" 2>/dev/null || true
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    echo "  AU   -> $AU_DEST/Ampex.component   (Logic/GarageBand; Cubase ignores AU)"
fi

echo
echo "Done. In Cubase: Studio > VST Plug-in Manager > Rescan All, then look under"
echo "Distortion (or search \"Ampex\") in the insert selector."
