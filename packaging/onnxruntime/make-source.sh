#!/usr/bin/env bash
# Builds the onnxruntime source package for the PPA (for releases without
# onnxruntime, e.g. noble):
#   packaging/onnxruntime/make-source.sh [codename]
# The orig tarball is the release with the archives of its CMake
# dependencies (the list in deps, the URLs and SHA1s in the release's
# cmake/deps.txt) in mirror/, where onnxruntime's CMake looks for them
# first, so the build needs no network.
# NO_UPLOAD=1: unsigned, no upload. SIGN_KEY, PPA as in upload-ubuntu-ppa.sh.
set -euo pipefail
VER=1.23.2
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(dirname "$(dirname "$HERE")")/_build/onnxruntime
CACHE=${CACHE:-$WORK/cache}
mkdir -p "$CACHE"
REL=onnxruntime-$VER.tar.gz
SUM=$(cat "$HERE/release.sha256")
if ! { [ -f "$CACHE/$REL" ] && echo "$SUM  $CACHE/$REL" | sha256sum -c --quiet; }; then
  curl -fsSL --retry 5 -o "$CACHE/$REL" https://github.com/microsoft/onnxruntime/archive/refs/tags/v$VER.tar.gz
  echo "$SUM  $CACHE/$REL" | sha256sum -c --quiet
fi

ORIG=onnxruntime_$VER+deps.orig.tar.xz
rm -rf "$WORK/$DIST"; mkdir -p "$WORK/$DIST"; cd "$WORK/$DIST"
tar xf "$CACHE/$REL"
S=onnxruntime-$VER
for name in $(sed -e 's/#.*//' "$HERE/deps"); do
  IFS=';' read -r _ url sha1 < <(grep "^$name;" "$S/cmake/deps.txt")
  [ -n "${sha1:-}" ] || { echo "error: $name not in deps.txt" >&2; exit 1; }
  rel=${url#https://}
  f=$CACHE/deps/$rel
  if ! { [ -f "$f" ] && echo "$sha1  $f" | sha1sum -c --quiet; }; then
    mkdir -p "$(dirname "$f")"
    curl -fsSL --retry 5 -o "$f" "$url"
    echo "$sha1  $f" | sha1sum -c --quiet
  fi
  mkdir -p "$S/mirror/$(dirname "$rel")"
  cp "$f" "$S/mirror/$rel"
done
# the times of the release; the copied archives are clamped to the newest
# of them, so the tarball does not depend on the download time
NEWEST=$(find "$S" -path "$S/mirror" -prune -o -type f -printf '%T@\n' | sort -n | tail -1)
NEWEST=${NEWEST%.*}
[ "$NEWEST" -gt 946684800 ] || { echo "error: no usable times in the release" >&2; exit 1; }
find "$S" -newermt "@$NEWEST" -exec touch -h -d "@$NEWEST" {} +
tar --sort=name --owner=0 --group=0 --numeric-owner -cJf "$ORIG" "$S"
cp -r "$HERE/debian" "$S/"
cd "$S"
N=1
sed -i "1s/(\([^)]*\)) [a-z]*;/(\1~ppa$N~$DIST) $DIST;/" debian/changelog
if [ -n "${NO_UPLOAD:-}" ]; then dpkg-buildpackage -S -d -us -uc -sa; exit 0; fi
dpkg-buildpackage -S -d -sa ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
