#!/usr/bin/env bash
# Builds the onnxruntime-tts source package for the PPA: a private copy of a
# newer onnxruntime than the distribution's, in /usr/lib/<triplet>/linux-tts
#   packaging/onnxruntime-tts/make-source.sh [codename]
# The orig tarball is the release commit (git archive) with the archives of
# its CMake dependencies (the list in deps, the URLs and SHA1s in the
# release's cmake/deps.txt) in mirror/, where onnxruntime's CMake looks for
# them first, so the build needs no network.
# github.com/microsoft/onnxruntime is gone since 2026-11-17; the commit of
# the tag (from the Software Heritage snapshot of 2026-11-14) is fetched by
# its hash from a fork, which git checks against the hash: MIRRORS.
# NO_UPLOAD=1: unsigned, no upload. SIGN_KEY, PPA as in upload-ubuntu-ppa.sh.
set -euo pipefail
VER=1.32.0
COMMIT=fab30a46491a40f5e4c968b80e8a59acbbb91b53
MIRRORS=${MIRRORS:-"https://github.com/microsoft/onnxruntime https://github.com/intel/onnxruntime"}
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(dirname "$(dirname "$HERE")")/_build/onnxruntime-tts
CACHE=${CACHE:-$WORK/cache}
mkdir -p "$CACHE"
REL=onnxruntime-$VER.tar
if [ ! -f "$CACHE/$REL" ]; then
  G=$CACHE/git
  [ -d "$G" ] || git init -q --bare "$G"
  if ! git -C "$G" cat-file -e "$COMMIT^{commit}" 2>/dev/null; then
    for m in $MIRRORS; do
      git -C "$G" fetch -q --depth 1 "$m" "$COMMIT" && break || true
    done
  fi
  [ "$(git -C "$G" rev-parse "$COMMIT^{commit}")" = "$COMMIT" ]
  git -C "$G" archive --prefix="onnxruntime-$VER/" -o "$CACHE/$REL.tmp" "$COMMIT"
  mv "$CACHE/$REL.tmp" "$CACHE/$REL"
fi
[ "$(tar xOf "$CACHE/$REL" "onnxruntime-$VER/VERSION_NUMBER")" = "$VER" ]

ORIG=onnxruntime-tts_$VER+deps.orig.tar.xz
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
