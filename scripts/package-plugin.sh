#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# package-plugin.sh — build HDR-Playlist-Source OBS plugin on Linux x64 /
# macOS (Apple Silicon).
#
# Output:
#   Linux : release/HDR-Playlist-Source-obs-linux-x64-<version>.tar.gz
#   macOS : release/HDR-Playlist-Source-obs-macos-arm64-<version>.tar.gz
#
# Prerequisites: cmake, git, gcc/clang. macOS additionally needs Xcode
# Command Line Tools.  Set OBS_VERSION to pick the obs-studio tag whose
# libobs headers are used (default 31.0.0).
# ---------------------------------------------------------------------------
set -euo pipefail

OBS_VERSION="${OBS_VERSION:-31.0.0}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

os="$(uname -s)"
case "$os" in
    Linux)  OS=linux ;;
    Darwin) OS=macos ;;
    *) echo "Unsupported OS: $os" >&2; exit 1 ;;
esac

case "$(uname -m)" in
    arm64|aarch64) ARCH=arm64 ;;
    x86_64|amd64)  ARCH=x64 ;;
    *) echo "Unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

step() { printf '\n==> %s\n' "$1"; }

# Version straight from CMakeLists.txt project(... VERSION x.y.z).
version="$(sed -n 's/^project(HDR-Playlist-Source VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -n1)"
if [ -z "$version" ]; then
    echo "could not read version from CMakeLists.txt" >&2
    exit 1
fi
step "Packaging HDR-Playlist-Source v$version ($OS-$ARCH)"

# --- OBS SDK headers --------------------------------------------------------
workdir="$root/build/plugin-sdk"
obs_src="$workdir/obs-studio"
if [ ! -f "$obs_src/libobs/obs-module.h" ]; then
    step "Fetching libobs headers (obs-studio $OBS_VERSION, shallow clone)"
    mkdir -p "$workdir"
    git clone --depth 1 --branch "$OBS_VERSION" \
        https://github.com/obsproject/obs-studio "$obs_src"
fi

CMAKE_EXTRA=("-DLIBOBS_INCLUDE_DIR=$obs_src/libobs")

if [ "$OS" = "linux" ]; then
    # --- Linux: stub libobs.so with soname libobs.so.0 ---------------------
    stub_dir="$workdir/stub"
    mkdir -p "$stub_dir"
    if [ ! -f "$stub_dir/libobs.so" ]; then
        step "Creating libobs stub shared library"
        : > "$stub_dir/stub.c"
        cc -shared -Wl,-soname,libobs.so.0 -o "$stub_dir/libobs.so" "$stub_dir/stub.c"
    fi
    CMAKE_EXTRA+=("-DOBS_STUB_LIB=$stub_dir/libobs.so")

    step "Building plugin (CMake)"
    cmake -S "$root" -B "$workdir/plugin-build-linux" \
        -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA[@]}"
    cmake --build "$workdir/plugin-build-linux"
    so="$(find "$workdir/plugin-build-linux" -name 'HDR-Playlist-Source.so' | head -n1)"
    [ -n "$so" ] || { echo "plugin .so not found" >&2; exit 1; }

    step "Assembling package"
    stage="$workdir/stage-linux"
    rm -rf "$stage"
    mkdir -p "$stage/HDR-Playlist-Source/bin/64bit" \
             "$stage/HDR-Playlist-Source/data/locale"
    cp "$so" "$stage/HDR-Playlist-Source/bin/64bit/"
    cp "$root"/data/locale/*.ini "$stage/HDR-Playlist-Source/data/locale/"
    cp "$root/README.md" "$stage/HDR-Playlist-Source/"

    mkdir -p "$root/release"
    out="$root/release/HDR-Playlist-Source-obs-linux-x64-$version.tar.gz"
    rm -f "$out"
    tar -czf "$out" -C "$stage" HDR-Playlist-Source
else
    # --- macOS: -undefined dynamic_lookup (no stub needed) ------------------
    step "Building plugin (CMake)"
    cmake -S "$root" -B "$workdir/plugin-build-macos" \
        -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA[@]}"
    cmake --build "$workdir/plugin-build-macos"
    mod="$(find "$workdir/plugin-build-macos" -name 'HDR-Playlist-Source' -type f | head -n1)"
    [ -n "$mod" ] || { echo "plugin module not found" >&2; exit 1; }

    step "Assembling .plugin bundle"
    stage="$workdir/stage-macos"
    rm -rf "$stage"
    bundle="$stage/HDR-Playlist-Source.plugin"
    mkdir -p "$bundle/Contents/MacOS" \
             "$bundle/Contents/Resources/data/locale"
    cp "$mod" "$bundle/Contents/MacOS/HDR-Playlist-Source"
    chmod +x "$bundle/Contents/MacOS/HDR-Playlist-Source"
    cp "$root"/data/locale/*.ini "$bundle/Contents/Resources/data/locale/"
    cp "$root/README.md" "$bundle/Contents/Resources/"
    cat > "$bundle/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>
    <string>com.hdrplaylist.HDR-Playlist-Source</string>
    <key>CFBundleName</key>
    <string>HDR-Playlist-Source</string>
    <key>CFBundleVersion</key>
    <string>$version</string>
    <key>CFBundleShortVersionString</key>
    <string>$version</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
</dict>
</plist>
EOF

    mkdir -p "$root/release"
    out="$root/release/HDR-Playlist-Source-obs-macos-$ARCH-$version.tar.gz"
    rm -f "$out"
    tar -czf "$out" -C "$stage" HDR-Playlist-Source.plugin
fi

if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$out" > "$out.sha256"
elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$out" > "$out.sha256"
fi

step "Done: $out"
echo "    SHA256: $(awk '{print $1}' "$out.sha256")"
echo "    Install: extract so the HDR-Playlist-Source(.plugin) folder lands in"
echo "    ~/.config/obs-studio/plugins/  (~/Library/Application Support/obs-studio/plugins/ on macOS)"
