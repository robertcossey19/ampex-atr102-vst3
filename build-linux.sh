#!/bin/bash
# Ampex - Linux build (VST3). Needs webkit2gtk for the web UI.
set -euo pipefail
cd "$(dirname "$0")"
sudo apt-get install -y cmake build-essential ninja-build pkg-config \
  libasound2-dev libfreetype-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libgtk-3-dev libwebkit2gtk-4.1-dev libcurl4-openssl-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Ampex_VST3 --parallel
echo "VST3: build/Ampex_artefacts/Release/VST3/Ampex.vst3  (copy to ~/.vst3/)"
