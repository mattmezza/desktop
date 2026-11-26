#!/bin/sh
# Run within Alpine Linux 3.23. Mount the source tree read-only and /out writable.
set -eu
version=${1#v}
output=$2
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
jobs=$(getconf _NPROCESSORS_ONLN)
if [ "$jobs" -gt 8 ]; then jobs=8; fi

apk add --no-cache build-base pkgconf curl xz binutils nasm \
    libxrandr-dev libx11-static libxcb-static libxext-static \
    libxrender-dev libxfixes-dev libxscrnsaver-dev \
    freetype-dev freetype-static harfbuzz-dev harfbuzz-static \
    bzip2-static libpng-dev libpng-static brotli-static zlib-static glib-static \
    gettext-static pcre2-static graphite2-static libjpeg-turbo-dev libjpeg-turbo-static libwebp-dev libwebp-static

cd "$tmp"
build_xorg() {
    package=$1
    hash=$2
    archive=$package.tar.xz
    if [ -f "/sources/$archive" ]; then cp "/sources/$archive" .
    else curl -fsSL --retry 3 "https://www.x.org/releases/individual/lib/$archive" -o "$archive"
    fi
    printf '%s  %s\n' "$hash" "$archive" | sha256sum -c -
    tar -xf "$archive"
    (
        cd "$package"
        ./configure --prefix=/usr/local --enable-static --disable-shared > "$tmp/$package.log"
        make -j "$jobs" install >> "$tmp/$package.log"
    )
}

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
build_xorg libXau-1.0.12 74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb
build_xorg libXrandr-1.5.5 72b922c2e765434e9e9f0960148070bd4504b288263e2868a4ccce1b7cf2767a
build_xorg libXrender-0.9.12 b832128da48b39c8d608224481743403ad1691bf4e554e4be9c174df171d1b97
build_xorg libXfixes-6.0.1 b695f93cd2499421ab02d22744458e650ccc88c1d4c8130d60200213abc02d58
build_xorg libXScrnSaver-1.2.4 75cd2859f38e207a090cac980d76bc71e9da99d48d09703584e00585abc920fe

mkdir app
cp -R "$src/LICENSE" "$src/Makefile" "$src/config.ini" "$src/src" "$src/vendor" "$src/assets" app
make -C app -j "$jobs" STATIC=1 VERSION="$version" > "$tmp/desktop.log" 2>&1 || { cat "$tmp/desktop.log"; exit 1; }
artifact="desktop-linux-$(uname -m)"
mkdir -p "$output" "$tmp/package/bin" "$tmp/package/share/desktop"
strip -o "$tmp/package/bin/desktop" app/build/desktop
test "$("$tmp/package/bin/desktop" --version)" = "desktop $version"
make -C app install BINDIR=/bin DATADIR=/share/desktop PREFIX= DESTDIR="$tmp/install" \
    STATIC=1 VERSION="$version" >/dev/null
cp -R "$tmp/install/share/." "$tmp/package/share/"

archive=ffmpeg-9.1.tar.xz
if [ -f "/sources/$archive" ]; then cp "/sources/$archive" .
else curl -fsSL --retry 3 "https://ffmpeg.org/releases/$archive" -o "$archive"
fi
printf '%s  %s\n' 1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433 "$archive" | sha256sum -c -
tar -xf "$archive"
(
    cd ffmpeg-9.1
    ./configure --disable-autodetect --disable-shared --enable-static --pkg-config-flags=--static \
        --extra-ldflags=-static --disable-debug --disable-doc --disable-network --disable-devices \
        --disable-ffplay --disable-encoders --enable-encoder=rawvideo \
        --disable-muxers --enable-muxer=rawvideo --disable-protocols --enable-protocol=file,pipe \
        --disable-hwaccels > "$tmp/ffmpeg.log" 2>&1 || { cat "$tmp/ffmpeg.log"; exit 1; }
    make -j "$jobs" ffmpeg ffprobe >> "$tmp/ffmpeg.log" 2>&1 || { tail -150 "$tmp/ffmpeg.log"; exit 1; }
    strip -o "$tmp/package/bin/desktop-ffmpeg" ffmpeg
    strip -o "$tmp/package/bin/desktop-ffprobe" ffprobe
    cp COPYING.LGPLv2.1 "$tmp/package/share/desktop/FFmpeg-LGPLv2.1"
)
for executable in "$tmp"/package/bin/*; do
    if readelf -lW "$executable" | grep -q ' INTERP ' ||
        readelf -dW "$executable" | grep -q '(NEEDED)'; then
        echo "Unexpected dynamic linkage in $executable." >&2
        exit 1
    fi
done
cp "$tmp/package/bin/desktop" "$output/$artifact"
mkdir -p "$output/test-bin"
cp "$tmp/package/bin/desktop-ffmpeg" "$tmp/package/bin/desktop-ffprobe" "$output/test-bin"
cp "$archive" "$output/"
tar -czf "$output/$artifact.tar.gz" -C "$tmp/package" .
(cd "$output" && sha256sum "$artifact" > "$artifact.sha256" &&
    sha256sum "$artifact.tar.gz" > "$artifact.tar.gz.sha256")
