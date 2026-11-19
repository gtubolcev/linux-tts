#!/usr/bin/env bash
# Builds the onnxruntime-ep-vulkan source package for the PPA: the Vulkan
# plugin execution provider of onnx-vulkan-rs, for the private onnxruntime
# of libonnxruntime-tts.
#   packaging/onnxruntime-ep-vulkan/make-source.sh [codename]
# The orig tarball is the upstream commit (git archive) with the crates of
# Cargo.lock in vendor/ (cargo vendor; crates.io checksums in Cargo.lock),
# so the build needs no network. Our changes are in debian/patches.
# NO_UPLOAD=1: unsigned, no upload. SIGN_KEY, PPA as in upload-ubuntu-ppa.sh.
set -euo pipefail
FULL=8625ef9
VER=0.0.1~git$FULL
REPO=${REPO:-https://github.com/automataIA/onnx-vulkan-rs}
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(dirname "$(dirname "$HERE")")/_build/onnxruntime-ep-vulkan
CACHE=${CACHE:-$WORK/cache}
mkdir -p "$CACHE"
G=$CACHE/git
[ -d "$G" ] || git clone -q --bare "$REPO" "$G"
git -C "$G" cat-file -e "$FULL^{commit}" 2>/dev/null || git -C "$G" fetch -q "$REPO"
HASH=$(git -C "$G" rev-parse "$FULL^{commit}")
grep -qx "$HASH" "$HERE/commit"   # the full hash this packaging was tested with

ORIG=onnxruntime-ep-vulkan_$VER.orig.tar.xz
S=onnxruntime-ep-vulkan-$VER
rm -rf "$WORK/$DIST"; mkdir -p "$WORK/$DIST"; cd "$WORK/$DIST"
git -C "$G" archive --prefix="$S/" "$HASH" | tar x
# crates.io sources of the whole workspace; only downloaded, nothing of
# them runs. Without cargo here, VENDOR_HOST=user@host runs cargo vendor in
# docker there (image VENDOR_IMAGE, through drun). Either way each crate is
# checked against the checksum in Cargo.lock below.
V=$CACHE/vendor-$HASH
if [ ! -d "$V" ]; then
  if [ -n "${VENDOR_HOST:-}" ]; then
    R=claude-builds/ep-vendor
    ssh "$VENDOR_HOST" "rm -rf ~/$R && mkdir -p ~/$R/src"
    tar c -C "$S" . | ssh "$VENDOR_HOST" "tar x -C ~/$R/src && cd ~/$R && timeout 1200 ~/claude-builds/drun --job ep-vendor --owner tts -u \$(id -u):\$(id -g) -e HOME=/w -e CARGO_HOME=/w/ch -v \$PWD:/w -w /w/src ${VENDOR_IMAGE:-claude-onnx-vulkan} cargo vendor --locked /w/vendor >/dev/null && tar c -C vendor ." | { mkdir -p "$V.tmp"; tar x -C "$V.tmp"; }
  else
    CARGO_HOME=$CACHE/cargo-home cargo vendor --locked --manifest-path "$S/Cargo.toml" "$V.tmp" > /dev/null
  fi
  python3 -I "$HERE/check-vendor.py" "$S/Cargo.lock" "$V.tmp"
  mv "$V.tmp" "$V"
fi
python3 -I "$HERE/check-vendor.py" "$S/Cargo.lock" "$V"
cp -a "$V" "$S/vendor"
# the commit time; the vendored crates are clamped to it
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
