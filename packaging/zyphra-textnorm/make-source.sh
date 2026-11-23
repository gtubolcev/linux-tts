#!/usr/bin/env bash
# Builds the zyphra-textnorm source package for the PPA: the C ABI of
# zyphra-textnorm as a private shared library, which the speech-dispatcher
# modules load with dlopen for the languages other than Russian.
#   packaging/zyphra-textnorm/make-source.sh [codename]
# The orig tarball is the upstream release commit (git archive) with the
# crates of Cargo.lock in vendor/ (cargo vendor; checked against the
# crates.io checksums in Cargo.lock), so the build needs no network.
# Without cargo here, VENDOR_HOST=user@host runs cargo vendor in docker there.
# NO_UPLOAD=1: unsigned, no upload. SIGN_KEY, PPA as in upload-ubuntu-ppa.sh.
set -euo pipefail
VER=0.3.2
REPO=${REPO:-https://github.com/Zyphra/zyphra-textnorm}
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(dirname "$(dirname "$HERE")")/_build/zyphra-textnorm
CACHE=${CACHE:-$WORK/cache}
mkdir -p "$CACHE"
G=$CACHE/git
[ -d "$G" ] || git clone -q --bare "$REPO" "$G"
HASH=$(cat "$HERE/commit")   # the full hash of v$VER this packaging was tested with
git -C "$G" cat-file -e "$HASH^{commit}" 2>/dev/null || git -C "$G" fetch -q "$REPO" "refs/tags/v$VER:refs/tags/v$VER"
[ "$(git -C "$G" rev-parse "v$VER^{commit}")" = "$HASH" ]

ORIG=zyphra-textnorm_$VER.orig.tar.xz
S=zyphra-textnorm-$VER
rm -rf "$WORK/$DIST"; mkdir -p "$WORK/$DIST"; cd "$WORK/$DIST"
git -C "$G" archive --prefix="$S/" "$HASH" | tar x
V=$CACHE/vendor-$HASH
if [ ! -d "$V" ]; then
  if [ -n "${VENDOR_HOST:-}" ]; then
    R=claude-builds/zt-vendor
    ssh "$VENDOR_HOST" "rm -rf ~/$R && mkdir -p ~/$R/src"
    tar c -C "$S" . | ssh "$VENDOR_HOST" "tar x -C ~/$R/src && cd ~/$R && timeout 1200 ~/claude-builds/drun --job zt-vendor --owner TTS -u \$(id -u):\$(id -g) -e HOME=/w -e CARGO_HOME=/w/ch -e RUSTC=rustc-1.97 -v /etc/ssl/certs:/etc/ssl/certs:ro -v \$PWD:/w -w /w/src ${VENDOR_IMAGE:-claude-pkgbuild-rust:26.04} cargo-1.97 vendor --locked /w/vendor >/dev/null && tar c -C vendor ." | { mkdir -p "$V.tmp"; tar x -C "$V.tmp"; }
  else
    CARGO_HOME=$CACHE/cargo-home cargo vendor --locked --manifest-path "$S/Cargo.toml" "$V.tmp" > /dev/null
  fi
  python3 -I "$HERE/../onnxruntime-ep-vulkan/check-vendor.py" "$S/Cargo.lock" "$V.tmp"
  mv "$V.tmp" "$V"
fi
python3 -I "$HERE/../onnxruntime-ep-vulkan/check-vendor.py" "$S/Cargo.lock" "$V"
cp -a "$V" "$S/vendor"
T=$(git -C "$G" log -1 --format=%ct "$HASH")
find "$S" -newermt "@$T" -exec touch -h -d "@$T" {} +
find "$S/vendor" ! -newermt "@946684800" -exec touch -h -d "@$T" {} +
tar --sort=name --owner=0 --group=0 --numeric-owner -cJf "$ORIG" "$S"
cp -r "$HERE/debian" "$S/"
cd "$S"
N=1
sed -i "1s/(\([^)]*\)) [a-z]*;/(\1~ppa$N~$DIST) $DIST;/" debian/changelog
if [ -n "${NO_UPLOAD:-}" ]; then dpkg-buildpackage -S -d -us -uc -sa; exit 0; fi
dpkg-buildpackage -S -d -sa ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
