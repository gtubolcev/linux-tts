#!/usr/bin/env bash
# Builds the audiocpp-tts source package for the PPA: a private libaudiocpp
# (audio.cpp, ggml) in /usr/lib/<triplet>/linux-tts/audiocpp for sd_audiocpp
#   packaging/audiocpp-tts/make-source.sh [codename]
# The orig tarball is the release commit (git archive) without the files
# the library build does not use: model weights, test and demo audio of
# unclear licences, the web UI, documentation (debian/copyright,
# Files-Excluded). Everything else, ggml and the other vendored libraries
# included, is in the tree, so the build needs no network.
# NO_UPLOAD=1: unsigned, no upload. SIGN_KEY, PPA as in upload-ubuntu-ppa.sh.
set -euo pipefail
VER=0.9.6
COMMIT=04a37f2e80ff2dbcdd84cc67848a970bd02a73e0
REPO=${REPO:-https://github.com/0xShug0/audio.cpp}
# the files left out: Files-Excluded of debian/copyright
EXCLUDED=$(awk '/^Files-Excluded:/ {f = 1; sub(/^Files-Excluded:/, ""); print; next}
  f && /^ / {print; next} {f = 0}' "$(dirname "$0")/debian/copyright")
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(dirname "$(dirname "$HERE")")/_build/audiocpp-tts
CACHE=${CACHE:-$WORK/cache}
mkdir -p "$CACHE"
S=audiocpp-tts-$VER+ds
ORIG=audiocpp-tts_$VER+ds.orig.tar.xz
if [ ! -f "$CACHE/$ORIG" ]; then
  G=$CACHE/git
  [ -d "$G" ] || git init -q --bare "$G"
  git -C "$G" cat-file -e "$COMMIT^{commit}" 2>/dev/null \
    || git -C "$G" fetch -q --depth 1 "$REPO" "$COMMIT"
  [ "$(git -C "$G" rev-parse "$COMMIT^{commit}")" = "$COMMIT" ]
  T=$CACHE/tree; rm -rf "$T"; mkdir -p "$T"
  git -C "$G" archive --prefix="$S/" "$COMMIT" | tar -C "$T" -xf -
  for p in $EXCLUDED; do rm -rf "${T:?}/$S/$p"; done
  # the commit time, so the tarball does not depend on the checkout time
  WHEN=$(git -C "$G" log -1 --format=%ct "$COMMIT")
  tar -C "$T" --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$WHEN" \
    -cJf "$CACHE/$ORIG.tmp" "$S"
  mv "$CACHE/$ORIG.tmp" "$CACHE/$ORIG"
  rm -rf "$T"
fi
rm -rf "$WORK/$DIST"; mkdir -p "$WORK/$DIST"; cd "$WORK/$DIST"
cp "$CACHE/$ORIG" .
tar xf "$ORIG"
cp -r "$HERE/debian" "$S/"
cd "$S"
N=1
sed -i "1s/(\([^)]*\)) [a-z]*;/(\1~ppa$N~$DIST) $DIST;/" debian/changelog
if [ -n "${NO_UPLOAD:-}" ]; then dpkg-buildpackage -S -d -nc -us -uc -sa; exit 0; fi
dpkg-buildpackage -S -d -nc -sa ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
