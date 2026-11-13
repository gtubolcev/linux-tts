#!/usr/bin/env bash
# Builds the sherpa-onnx source package for the PPA:
#   packaging/sherpa-onnx/make-source.sh [codename]
# The orig tarball is the release with the tarballs of its CMake
# dependencies at the top (checked against deps.txt, whose checksums are
# those in the release's cmake/*.cmake), so the build needs no network.
# NO_UPLOAD=1: unsigned, no upload. SIGN_KEY, PPA as in upload-ubuntu-ppa.sh.
set -euo pipefail
VER=2.0.1
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(dirname "$(dirname "$HERE")")/_build/sherpa-onnx
CACHE=${CACHE:-$WORK/cache}
mkdir -p "$CACHE"
fetch() { # file url sha256
  [ -f "$CACHE/$1" ] && echo "$3  $CACHE/$1" | sha256sum -c --quiet && return
  curl -fsSL --retry 5 -o "$CACHE/$1" "$2"
  echo "$3  $CACHE/$1" | sha256sum -c --quiet
}
fetch sherpa-onnx-$VER.tar.gz https://github.com/k2-fsa/sherpa-onnx/archive/refs/tags/v$VER.tar.gz "$(cat "$HERE/release.sha256")"
while read -r f url sum; do fetch "$f" "$url" "$sum"; done < "$HERE/deps.txt"

# +deps2: the first orig tarball had all its times at 1970, which
# Launchpad rejects in the .debs; the name of a rejected orig is taken
ORIG=sherpa-onnx_$VER+deps2.orig.tar.xz
rm -rf "$WORK/$DIST"; mkdir -p "$WORK/$DIST"; cd "$WORK/$DIST"
tar xf "$CACHE/sherpa-onnx-$VER.tar.gz"
S=sherpa-onnx-$VER
while read -r f _; do cp "$CACHE/$f" "$S/"; done < "$HERE/deps.txt"
# the times of the release; the copied dependency tarballs are clamped to
# the newest of them, so the tarball does not depend on the download time
NEWEST=$(find "$S" -type f ! -name '*.tar.gz' ! -name '*.zip' -printf '%T@\n' | sort -n | tail -1)
NEWEST=${NEWEST%.*}
[ "$NEWEST" -gt 946684800 ] || { echo "error: no usable times in the release" >&2; exit 1; }
find "$S" -newermt "@$NEWEST" -exec touch -h -d "@$NEWEST" {} +
tar --sort=name --owner=0 --group=0 --numeric-owner --owner=0 --group=0 --numeric-owner -cJf "$ORIG" "$S"
cp -r "$HERE/debian" "$S/"
cd "$S"
N=1
sed -i "1s/(\([^)]*\)) [a-z]*;/(\1~ppa$N~$DIST) $DIST;/" debian/changelog
if [ -n "${NO_UPLOAD:-}" ]; then dpkg-buildpackage -S -d -us -uc -sa; exit 0; fi
dpkg-buildpackage -S -d -sa ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
