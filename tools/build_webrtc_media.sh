#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work_dir=${1:-"$root/build-native-webrtc"}
mkdir -p "$work_dir/deps"
work_dir=$(CDPATH= cd -- "$work_dir" && pwd -P)

datachannel="$work_dir/deps/libdatachannel"
datachannel_commit=6b1e2e620f1e37f0eafeee702eaea0043cb305fd
if [ ! -d "$datachannel/.git" ]; then
    git clone --depth 1 --branch v0.24.6 \
        https://github.com/paullouisageneau/libdatachannel.git "$datachannel"
fi
if [ "$(git -C "$datachannel" rev-parse HEAD)" != "$datachannel_commit" ]; then
    printf '%s\n' 'libdatachannel checkout does not match the pinned release' >&2
    exit 1
fi
git -C "$datachannel" submodule update --init --recursive --depth 1

opus="$work_dir/deps/opus-1.6.1"
archive="$opus.tar.gz"
checksum=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
if [ ! -f "$archive" ]; then
    partial=$(mktemp "$archive.XXXXXX")
    trap 'rm -f "$partial"' EXIT
    curl -fLsS --retry 2 -o "$partial" https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
    printf '%s  %s\n' "$checksum" "$partial" | shasum -a 256 -c -
    mv "$partial" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | shasum -a 256 -c -
if [ ! -d "$opus" ]; then
    tar -xf "$archive" -C "$work_dir/deps"
fi

openssl_prefix=${OPENSSL_ROOT_DIR:-}
set --
if [ -n "$openssl_prefix" ]; then
    set -- "-DOPENSSL_ROOT_DIR=$openssl_prefix"
fi

cmake -S "$datachannel" -B "$work_dir/datachannel-build" "$@" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$work_dir/install/datachannel" \
    -DBUILD_SHARED_LIBS=OFF -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON
cmake --build "$work_dir/datachannel-build" --parallel 4
cmake --install "$work_dir/datachannel-build"

cmake -S "$opus" -B "$work_dir/opus-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$work_dir/install/opus" \
    -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
cmake --build "$work_dir/opus-build" --parallel 4
cmake --install "$work_dir/opus-build"
printf 'CMake prefix path: %s/install/datachannel;%s/install/opus\n' "$work_dir" "$work_dir"
