#!/bin/sh
# Builds speech-dispatcher-sherpa-onnx-vulkan-rs: the synthesizer
# "sherpa-onnx-vulkan-rs", sd_sherpa-onnx on the graphics card through the
# onnx-vulkan-rs plugin of onnxruntime. Only a link to sd_sherpa-onnx and its
# configuration; kept apart from speech-dispatcher-sherpa-onnx because the
# plugin (onnxruntime-ep-vulkan) is not in the PPA.
#   build-deb.sh [OUT]
set -eu
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../sherpa
OUT=${1:-$HERE/out}
P=speech-dispatcher-sherpa-onnx-vulkan-rs
VER=0.1.0-1~local
MOD=usr/lib/speech-dispatcher-modules
CONF=etc/speech-dispatcher/modules
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

mkdir -p "$W/DEBIAN" "$W/$MOD" "$W/$CONF" "$W/usr/share/doc/$P" "$OUT"
ln -s sd_sherpa-onnx "$W/$MOD/sd_sherpa-onnx-vulkan-rs"
{
  echo "# sd_sherpa-onnx-vulkan-rs: the synthesizer sherpa-onnx on the graphics card,"
  echo "# through the onnx-vulkan-rs plugin of onnxruntime (onnxruntime-ep-vulkan);"
  echo "# hidden without it. Only voices that run there are listed: Piper voices"
  echo "# and VITS models on characters (MMS). The settings are those of"
  echo "# sherpa-onnx.conf."
  echo
  sed -e '1,/^$/d' -e 's/^#\(SherpaProvider\) .*/\1 "vulkan"/' \
      -e 's/^\(SherpaFamilies\) .*/\1 "piper vits"/' "$SRC/sherpa-onnx.conf"
} > "$W/$CONF/sherpa-onnx-vulkan-rs.conf"
grep -q '^SherpaProvider "vulkan"' "$W/$CONF/sherpa-onnx-vulkan-rs.conf"
grep -q '^SherpaFamilies "piper vits"' "$W/$CONF/sherpa-onnx-vulkan-rs.conf"
echo "/$CONF/sherpa-onnx-vulkan-rs.conf" > "$W/DEBIAN/conffiles"
cat > "$W/usr/share/doc/$P/copyright" <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: linux-tts

Files: *
Copyright: 2026 Grigory Tuboltsev
License: MIT
X
cat > "$W/DEBIAN/control" <<X
Package: $P
Version: $VER
Architecture: all
Maintainer: Grigory Tuboltsev <noreply@localhost>
Depends: speech-dispatcher-sherpa-onnx (>= 0.1.0~), onnxruntime-ep-vulkan
Replaces: speech-dispatcher-sherpa-onnx (<< 0.1.0~)
Section: sound
Priority: optional
Description: Speech Dispatcher module for sherpa-onnx voices on the graphics card
 The synthesizer "sherpa-onnx-vulkan-rs": sd_sherpa-onnx with the Vulkan
 plugin of onnxruntime, for Piper voices and MMS models. Settings in
 /etc/speech-dispatcher/modules/sherpa-onnx-vulkan-rs.conf.
X
dpkg-deb --root-owner-group -Zxz -b "$W" "$OUT/${P}_${VER}_all.deb"
