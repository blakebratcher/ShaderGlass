#!/usr/bin/env bash
#
# Build a ShaderScope AppImage. Produces ShaderScope-<commit>-x86_64.AppImage
# at the repo root. Requires:
#   - cmake, ninja or make, a C++20 toolchain
#   - linuxdeploy-x86_64.AppImage (auto-downloaded into ./build-appimage/
#     if absent; offline runs need it placed there manually).
#   - System libs for the build deps (SDL3, Vulkan, PipeWire, X11, glslang).
#
# Output AppImage bundles the system libs that the binary depends on, so
# it runs on most modern x86_64 distros without further install.
#
# Usage: ./packaging/build-appimage.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"

echo ">>> Clean build dir"
rm -rf "$BUILD"

echo ">>> Configure (Release, prefix=/usr)"
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DSHADERGLASS_TESTS=OFF

echo ">>> Build"
cmake --build "$BUILD" -j"$(nproc)"

echo ">>> Install into AppDir"
DESTDIR="$APPDIR" cmake --install "$BUILD"

# linuxdeploy needs the .desktop file at the top of the AppDir too;
# install put it under usr/share/applications/ already, but the
# top-level copy avoids a non-fatal warning.
cp "$APPDIR/usr/share/applications/shaderscope.desktop" "$APPDIR/" 2>/dev/null || true
cp "$APPDIR/usr/share/icons/hicolor/scalable/apps/shaderscope.svg" "$APPDIR/" 2>/dev/null || true

LINUXDEPLOY="$BUILD/linuxdeploy-x86_64.AppImage"
if [ ! -x "$LINUXDEPLOY" ]; then
    echo ">>> Fetching linuxdeploy"
    if command -v curl >/dev/null; then
        curl -fsSL -o "$LINUXDEPLOY" \
            "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    elif command -v wget >/dev/null; then
        wget -q -O "$LINUXDEPLOY" \
            "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    else
        echo "error: need curl or wget to fetch linuxdeploy" >&2
        exit 1
    fi
    chmod +x "$LINUXDEPLOY"
fi

# AppImages can't extract themselves from inside a sandboxed FUSE-less env;
# the --appimage-extract-and-run flag handles that case cleanly.
VERSION="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

echo ">>> Building AppImage (this can take a minute)"
cd "$BUILD"
ARCH=x86_64 VERSION="$VERSION" \
    "$LINUXDEPLOY" --appimage-extract-and-run \
        --appdir "$APPDIR" \
        --output appimage \
        --desktop-file "$APPDIR/shaderscope.desktop" \
        --icon-file "$APPDIR/shaderscope.svg"

# linuxdeploy emits ShaderScope-<VERSION>-<ARCH>.AppImage in CWD ($BUILD).
APPIMAGE=$(ls -1t "$BUILD"/ShaderScope-*.AppImage 2>/dev/null | head -1)
if [ -z "$APPIMAGE" ]; then
    # Fallback name if linuxdeploy used a different scheme
    APPIMAGE=$(ls -1t "$BUILD"/*.AppImage 2>/dev/null | grep -v linuxdeploy | head -1)
fi
if [ -n "$APPIMAGE" ]; then
    mv -v "$APPIMAGE" "$ROOT/"
    echo
    echo ">>> Done: $(basename "$APPIMAGE")"
    echo "    Path: $ROOT/$(basename "$APPIMAGE")"
else
    echo "error: no AppImage produced — inspect $BUILD for clues" >&2
    exit 1
fi
