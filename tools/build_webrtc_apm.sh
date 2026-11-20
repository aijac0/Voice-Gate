#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work_dir=${1:-"$root/build/webrtc-apm"}
mkdir -p "$work_dir"
work_dir=$(CDPATH= cd -- "$work_dir" && pwd -P)

fetch() (
    file=$1
    checksum=$2
    url=$3
    if [ -f "$file" ]; then
        printf '%s  %s\n' "$checksum" "$file" | shasum -a 256 -c -
        exit
    fi
    partial=$(mktemp "$file.XXXXXX")
    trap 'rm -f "$partial"' 0
    curl -fLsS --retry 2 -o "$partial" "$url"
    printf '%s  %s\n' "$checksum" "$partial" | shasum -a 256 -c -
    mv "$partial" "$file"
)

archive="$work_dir/apm.tar.xz"
source="$work_dir/webrtc-audio-processing-2.1"
fetch "$archive" ae9302824b2038d394f10213cab05312c564a038434269f11dbf68f511f9f9fe \
    https://gstreamer.freedesktop.org/src/mirror/webrtc-audio-processing/webrtc-audio-processing-2.1.tar.xz
if [ ! -d "$source" ]; then
    tar -xf "$archive" -C "$work_dir"
fi

# These are the exact Abseil inputs in the release's Meson wrap.
cache="$source/subprojects/packagecache"
mkdir -p "$cache"
fetch "$cache/abseil-cpp-20240722.0.tar.gz" \
    f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3 \
    https://github.com/abseil/abseil-cpp/releases/download/20240722.0/abseil-cpp-20240722.0.tar.gz
fetch "$cache/abseil-cpp_20240722.0-3_patch.zip" \
    12dd8df1488a314c53e3751abd2750cf233b830651d168b6a9f15e7d0cf71f7b \
    https://wrapdb.mesonbuild.com/v2/abseil-cpp_20240722.0-3/get_patch

prefix="$work_dir/install"
build_dir="$work_dir/apm-build"
if [ ! -f "$build_dir/build.ninja" ]; then
    meson setup "$build_dir" "$source" --prefix "$prefix" --libdir lib \
        --default-library=static --wrap-mode=forcefallback -Dbuildtype=release
fi
meson compile -C "$build_dir"
meson install -C "$build_dir" --no-rebuild
printf 'WebRTC pkg-config directory: %s/lib/pkgconfig\n' "$prefix"
