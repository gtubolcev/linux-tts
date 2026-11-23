#!/bin/sh
# Makes the source package audiocpp-tts-model-tiny-voca-ru for the PPA:
# tiny-voca-ru (egnch/tiny-voca-ru, Apache-2.0), the 4.5M-parameter Russian
# model on the tiny-voca architecture, voices F and M, for sd_audiocpp
# (family tiny_voca_ru, libaudiocpp-tts 0.9.6+ds-3 or later). sd_audiocpp finds
# it as /usr/share/audiocpp-tts/tiny-voca-ru.
#
# The GGUF is the float32 weights of model.safetensors bit for bit, made by
# tools/community_models/convert_tiny_voca_ru.py of audio.cpp (our patch
# tiny-voca-ru.patch in packaging/audiocpp-tts) from revision $HF_REV; the
# original config.json, LICENSE, NOTICE and README.md ship next to it.
#
#   make-source.sh [codename]
#
#   DL        tiny-voca-ru-orig.gguf (made by hand) and the downloads
#   OUT       where the source package goes
#   REVISION  the Debian revision (default 1~ppa1)
#   ORIG      -sa (default) uploads the orig tarball, -sd not
#   SIGN_KEY  sign with this key; unsigned without it
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
DIST=${1:-resolute}
DL=${DL:-/home/grigory/Storage/Claude/tvru-model/dl}
OUT=${OUT:-/home/grigory/Storage/Claude/tvru-model/ppa}
HF_REV=2723b0e71fea216001bd4f6caf0866eb06879339   # egnch/tiny-voca-ru
HF=https://huggingface.co/egnch/tiny-voca-ru/resolve/$HF_REV
UPSTREAM=0~20261123.2723b0e
SRC=audiocpp-tts-model-tiny-voca-ru
VERSION=$UPSTREAM-${REVISION:-1~ppa1}
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}
MAINT="$DEBFULLNAME <$DEBEMAIL>"
FILES="tiny-voca-ru-orig.gguf config.json LICENSE NOTICE README.md"

mkdir -p "$DL"
for f in LICENSE NOTICE README.md config.json; do
  [ -s "$DL/$f" ] || curl -fL --retry 3 -o "$DL/$f" "$HF/$f"
done
(cd "$DL" && sha256sum -c --quiet "$HERE/SHA256SUMS")

rm -rf "$OUT"
S=$OUT/$SRC-$UPSTREAM
mkdir -p "$S"
for f in $FILES; do cp "$DL/$f" "$S/"; done
cp "$HERE/audiocpp.ini" "$HERE/SHA256SUMS" "$S/"
find "$S" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
MTIME=2026-11-23T12:00:00+01:00
tar --sort=name --mtime="$MTIME" --owner=0 --group=0 --numeric-owner \
  -C "$OUT" -cf - "$SRC-$UPSTREAM" | xz -T0 -6 > "$OUT/${SRC}_$UPSTREAM.orig.tar.xz"

D=$S/debian
mkdir -p "$D/source"
echo "3.0 (quilt)" > "$D/source/format"
cat > "$D/rules" <<'X'
#!/usr/bin/make -f
%:
	dh $@
X
chmod 755 "$D/rules"
cat > "$D/changelog" <<X
$SRC ($VERSION~$DIST) $DIST; urgency=medium

  * tiny-voca-ru for sd_audiocpp (egnch/tiny-voca-ru, revision
    $HF_REV).

 -- $MAINT  $(date -R)
X
M=usr/share/audiocpp-tts/tiny-voca-ru
cat > "$D/$SRC.install" <<X
audiocpp.ini $M/
tiny-voca-ru-orig.gguf $M/
config.json $M/
X
cat > "$D/$SRC.docs" <<X
README.md
NOTICE
X
cat > "$D/control" <<X
Source: $SRC
Section: sound
Priority: optional
Maintainer: $MAINT
Build-Depends: debhelper-compat (= 13)
Standards-Version: 4.7.0
Homepage: https://huggingface.co/egnch/tiny-voca-ru
Rules-Requires-Root: no

Package: $SRC
Architecture: all
Depends: \${misc:Depends}
Recommends: speech-dispatcher-audiocpp, libaudiocpp-tts (>= 0.9.6+ds-3~)
Enhances: speech-dispatcher-audiocpp
Description: tiny-voca-ru, a small Russian voice model for audio.cpp
 tiny-voca-ru is a 4.5M-parameter Russian text to speech model (24 kHz,
 a female and a male voice) on the tiny-voca architecture. It needs very
 little memory and CPU time. This package holds it for the
 speech-dispatcher synthesizer audiocpp (speech-dispatcher-audiocpp), which
 gives it normalised text with stress marks.
X
cat > "$D/copyright" <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: tiny-voca-ru
Source: https://huggingface.co/egnch/tiny-voca-ru
Comment: revision $HF_REV. tiny-voca-ru-orig.gguf holds the weights of
 model.safetensors unchanged (float32), converted with audio.cpp's
 convert_tiny_voca_ru.py.

Files: *
Copyright: 2026 Evgeniy Chetvertkov
           2026 Muhammed Nazeem (tiny-voca)
License: Apache-2.0

Files: audiocpp.ini debian/*
Copyright: 2026 $MAINT
License: Apache-2.0

License: Apache-2.0
 On Debian systems the full text of the Apache License version 2.0 is in
 /usr/share/common-licenses/Apache-2.0.
X

cd "$S"
if [ -n "${SIGN_KEY:-}" ]; then
  dpkg-buildpackage -S -d ${ORIG:--sa} --sign-key="$SIGN_KEY"
else
  dpkg-buildpackage -S -d -us -uc ${ORIG:--sa}
fi
