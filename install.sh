#!/bin/sh
set -eu
repo=https://github.com/mattmezza/desktop
prefix=${PREFIX:-"$HOME/.local"}
version=${DESKTOP_VERSION:-latest}
case "$(uname -s):$(uname -m)" in
    Linux:x86_64) arch=x86_64 ;;
    Linux:aarch64|Linux:arm64) arch=aarch64 ;;
    *) printf '%s\n' 'Desktop releases support Linux x86-64 and ARM64.' >&2; exit 1 ;;
esac
case "$version" in
    latest) url=$repo/releases/latest/download ;;
    v[0-9]*)
        printf '%s\n' "$version" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$' || exit 2
        url=$repo/releases/download/$version ;;
    *) printf '%s\n' 'Use DESKTOP_VERSION=vMAJOR.MINOR.PATCH to install a specific version.' >&2; exit 2 ;;
esac
tmp=$(mktemp -d)
target=
trap 'rm -rf "$tmp"; if [ -n "$target" ]; then rm -f "$target"; fi' EXIT
asset=desktop-linux-$arch.tar.gz
curl -fLsS --retry 3 "$url/$asset" -o "$tmp/$asset"
curl -fLsS --retry 3 "$url/$asset.sha256" -o "$tmp/$asset.sha256"
expected=$(awk 'NR == 1 {print $1}' "$tmp/$asset.sha256")
actual=$(sha256sum "$tmp/$asset" | awk '{print $1}')
test "$actual" = "$expected" || { echo 'Desktop release checksum mismatch.' >&2; exit 1; }
mkdir "$tmp/package"
tar -xzf "$tmp/$asset" -C "$tmp/package"
for name in desktop-ffmpeg desktop-ffprobe desktop; do
    mkdir -p "$prefix/bin"
    target=$(mktemp "$prefix/bin/.$name.XXXXXXXX")
    install -m 755 "$tmp/package/bin/$name" "$target"
    mv -f "$target" "$prefix/bin/$name"
    target=
done
for directory in desktop applications icons/hicolor/scalable/apps; do
    mkdir -p "$prefix/share/$directory"
    install -m 644 "$tmp/package/share/$directory/"* "$prefix/share/$directory/"
done
config=${XDG_CONFIG_HOME:-"$HOME/.config"}/desktop/config.ini
if [ ! -e "$config" ] && [ ! -L "$config" ]; then
    mkdir -p "$(dirname "$config")"
    (umask 077; set -C; cat "$prefix/share/desktop/config.ini" > "$config") ||
        { printf 'Using existing config %s\n' "$config" >&2; }
fi
printf 'Installed %s at %s/bin/desktop\n' "$("$prefix/bin/desktop" --version)" "$prefix"
