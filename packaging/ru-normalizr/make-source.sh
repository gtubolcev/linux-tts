#!/usr/bin/env bash
# Builds the speech-dispatcher-ru-normalizr source package for the PPA:
# ru-normalizr (upstream commit COMMIT, our fixes in debian/patches) and the
# pure-Python libraries Ubuntu lacks, in a private directory for tts-normalize;
# and speech-dispatcher-ru-normalizr-data, the data of the C normalizer made from
# them at build time by textnorm/cmorphy3/mkdict.py and textnorm/rn/gen-ipa.py.
#   packaging/ru-normalizr/make-source.sh [codename]
# The orig tarball is the upstream commit (git archive) with deps/: the wheels
# and the one sdist named in deps.sha256, fetched with pip download and checked
# by hash, so the build needs no network.
# RN_GIT: a clone of ru-normalizr that has COMMIT (else it is cloned).
# NO_UPLOAD=1: unsigned, no upload. SIGN_KEY, PPA as in upload-ubuntu-ppa.sh.
set -euo pipefail
COMMIT=5f3a016757dd08b9c245b9b892f2d8080e1474a5
UPSTREAM=https://github.com/NickZaitsev/ru-normalizr
VER=1.3.0+git20261012.5f3a016+deps
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(dirname "$(dirname "$HERE")")/_build/ru-normalizr
CACHE=$WORK/cache
mkdir -p "$CACHE/deps"
G=${RN_GIT:-$CACHE/git}
if ! git -C "$G" cat-file -e "$COMMIT^{commit}" 2>/dev/null; then
  [ -d "$G" ] || git init -q --bare "$G"
  git -C "$G" fetch -q --depth 1 "$UPSTREAM" "$COMMIT"
fi
[ "$(git -C "$G" rev-parse "$COMMIT^{commit}")" = "$COMMIT" ]

# the dependencies: wheels, except eng_to_ipa (sdist only)
cd "$CACHE/deps"
if ! sha256sum -c --quiet "$HERE/deps.sha256" 2>/dev/null; then
  python3 -I -m pip download -q --no-deps -d . --only-binary=:all: \
    $(sed -n 's/^[0-9a-f]*  \(.*\)-py.*\.whl$/\1/p' "$HERE/deps.sha256" \
      | sed -E 's/^([^-]+)-(.*)$/\1==\2/')
  python3 -I -m pip download -q --no-deps -d . --no-binary=:all: --no-build-isolation \
    $(sed -n 's/^[0-9a-f]*  \(.*\)\.tar\.gz$/\1/p' "$HERE/deps.sha256" \
      | sed -E 's/^(.*)-([0-9][^-]*)$/\1==\2/')
  sha256sum -c --quiet "$HERE/deps.sha256"
fi

PKG=speech-dispatcher-ru-normalizr
S=$PKG-$VER
ORIG=${PKG}_$VER.orig.tar.xz
rm -rf "$WORK/$DIST"; mkdir -p "$WORK/$DIST/$S/deps"; cd "$WORK/$DIST"
git -C "$G" archive "$COMMIT" | tar x -C "$S"
cut -d' ' -f3 "$HERE/deps.sha256" | while read -r f; do cp "$CACHE/deps/$f" "$S/deps/"; done
T=$(git -C "$G" log -1 --format=%ct "$COMMIT")
find "$S" -exec touch -h -d "@$T" {} +
tar --sort=name --owner=0 --group=0 --numeric-owner -cJf "$ORIG" "$S"
cp -r "$HERE/debian" "$S/"
mkdir -p "$S/debian/textnorm"
cp "$HERE/../../textnorm/cmorphy3/mkdict.py" "$HERE/../../textnorm/rn/gen-ipa.py" \
  "$S/debian/textnorm/"
cd "$S"
N=1
sed -i "1s/(\([^)]*\)) [a-z]*;/(\1~ppa$N~$DIST) $DIST;/" debian/changelog
if [ -n "${NO_UPLOAD:-}" ]; then dpkg-buildpackage -S -d -nc -us -uc -sa; exit 0; fi
dpkg-buildpackage -S -d -nc -sa ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
