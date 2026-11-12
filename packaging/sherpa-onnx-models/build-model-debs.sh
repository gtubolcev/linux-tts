#!/bin/sh
# One arch-independent .deb per sherpa-onnx TTS model:
# /usr/share/sherpa-onnx/tts/<name>/ (the files of the upstream tarball).
set -e
umask 022
M=/home/grigory/Storage/Claude/tts-models
OUTDIR=$(dirname "$(readlink -f "$0")")
W=/home/grigory/Storage/Claude/model-debs
mkdir -p "$W"

# name version src-dir section license-name short-description
model() {
  name=$1 ver=$2 src=$3 section=$4 lic=$5 desc=$6
  pkg=sherpa-onnx-tts-$name
  P=$W/$pkg
  rm -rf "$P"
  mkdir -p "$P/usr/share/sherpa-onnx/tts/$name" "$P/usr/share/doc/$pkg" "$P/DEBIAN"
  cp -r "$M/$src"/. "$P/usr/share/sherpa-onnx/tts/$name/"
  rm -f "$P/usr/share/sherpa-onnx/tts/$name"/*.py "$P/usr/share/sherpa-onnx/tts/$name"/*.sh
  if [ -f "$P/usr/share/sherpa-onnx/tts/$name/LICENSE" ]; then
    mv "$P/usr/share/sherpa-onnx/tts/$name/LICENSE" "$P/usr/share/doc/$pkg/copyright"
  else
    printf 'License: %s\n' "$lic" > "$P/usr/share/doc/$pkg/copyright"
  fi
  cat > "$P/DEBIAN/control" <<CTL
Package: $pkg
Version: $ver
Architecture: all
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Installed-Size: $(du -sk "$P" | cut -f1)
Recommends: sherpa-onnx (>= 2.0)
Section: $section
Priority: optional
Homepage: https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models
Description: $desc
 Text to speech model for sherpa-onnx, installed in
 /usr/share/sherpa-onnx/tts/$name. Try it: sherpa-say $name [-l LANG] TEXT
 .
 Licence: $lic
CTL
  dpkg-deb --root-owner-group -Zxz -b "$P" "$OUTDIR/${pkg}_${ver}_all.deb" | tail -1
}

model supertonic-3 2026.05.11-1~local sherpa-onnx-supertonic-3-tts-int8-2026-05-11 \
  sound "OpenRAIL-M (model), MIT (code)" \
  "Supertonic 3 voice (31 languages, int8) for sherpa-onnx"
model pocket 2026.01.26-1~local sherpa-onnx-pocket-tts-int8-2026-01-26 \
  sound "CC-BY-4.0" \
  "Pocket TTS (Kyutai, English, voice from a sample, int8) for sherpa-onnx"
model kitten-nano-en 0.8-1~local kitten-nano-en-v0_8-int8 \
  sound "Apache-2.0" \
  "KittenTTS nano 0.8 voices (English, int8) for sherpa-onnx"
model mms-ru 1.0-1~local vits-mms-rus \
  non-free/sound "CC-BY-NC-4.0 (Meta MMS-TTS, non-commercial use only)" \
  "MMS-TTS Russian voice (Meta, VITS) for sherpa-onnx"
