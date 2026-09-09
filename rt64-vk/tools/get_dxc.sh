#!/usr/bin/env bash
# Fetch the upstream DirectX Shader Compiler Linux build.
#
# DXC is not packaged for Fedora (Arch has directx-shader-compiler; Fedora does
# not, and mesa-dxil-devel is the opposite direction). Microsoft ships a Linux
# tarball containing bin/dxc, lib/libdxcompiler.so and include/dxc.
#
# Installs into rt64-vk/third_party/dxc, where CMake looks by default.

set -euo pipefail

# Locate the rt64-vk root by walking up from the script, so this works whether
# it sits in rt64-vk/tools/ (where it ships) or in rt64-vk/ itself.
find_root() {
    local d
    d="$(cd "$(dirname "$0")" && pwd)"
    while [ "$d" != "/" ]; do
        if [ -f "$d/CMakeLists.txt" ] && grep -q 'project(rt64' "$d/CMakeLists.txt" 2>/dev/null; then
            printf '%s\n' "$d"
            return 0
        fi
        d="$(dirname "$d")"
    done
    return 1
}

if ! HERE="$(find_root)"; then
    echo "Could not find the rt64-vk root (no CMakeLists.txt with project(rt64)"
    echo "at or above $(dirname "$0"))."
    echo "Run this from inside the rt64-vk tree."
    exit 1
fi
DEST="$HERE/third_party/dxc"
echo ":: rt64-vk root: $HERE"
REPO="https://github.com/microsoft/DirectXShaderCompiler"

if [ -x "$DEST/bin/dxc" ]; then
    echo "dxc already installed at $DEST"
    LD_LIBRARY_PATH="$DEST/lib" "$DEST/bin/dxc" --version
    exit 0
fi

echo ":: locating the newest Linux release asset"
asset=$(curl -fsSL "https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases?per_page=10" |
        grep -oE '"browser_download_url": *"[^"]*linux_dxc[^"]*\.tar\.gz"' |
        head -1 | sed 's/.*"\(https[^"]*\)"/\1/')

if [ -z "$asset" ]; then
    echo "Could not find a Linux asset automatically (GitHub API rate limit?)."
    echo "Download one manually from $REPO/releases — the asset is named"
    echo "linux_dxc_<date>.x86_64.tar.gz — then extract it into:"
    echo "    $DEST"
    exit 1
fi

echo ":: downloading $(basename "$asset")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/dxc.tar.gz" "$asset"

mkdir -p "$DEST"
tar -xzf "$tmp/dxc.tar.gz" -C "$DEST"
chmod +x "$DEST/bin/dxc"

echo ":: installed to $DEST"
LD_LIBRARY_PATH="$DEST/lib" "$DEST/bin/dxc" --version
if ! LD_LIBRARY_PATH="$DEST/lib" "$DEST/bin/dxc" --help 2>&1 | grep -q -- '-spirv'; then
    echo "WARNING: this dxc has no SPIR-V backend — unusable for this port."
    exit 1
fi
echo ":: SPIR-V backend present"
