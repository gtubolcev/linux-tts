#!/bin/sh
# Makes the source package audiocpp-tts-model-kokoro-mul for the PPA:
# Kokoro-82M v1.0 (hexgrad/Kokoro-82M revision f3ff357, Apache-2.0) as GGUF
# (q8_0, converted by convert.sh with audio.cpp's prepare_kokoro_gguf.py) with
# its 41 voices in American and British English, Spanish, French, Hindi,
# Italian and Brazilian Portuguese (Japanese and Chinese wait for their front
# ends). sd_audiocpp finds it as /usr/share/audiocpp-tts/kokoro-mul; the
# module makes the phonemes itself (phonemizer=kokoro).
#
#   make-source.sh [codename]
#
#   DL        kokoro-82m-q8_0.gguf (convert.sh output)
#   OUT       where the source package goes
#   REVISION  the Debian revision (default 1~ppa1)
#   ORIG      -sa (default) uploads the orig tarball, -sd not
#   SIGN_KEY  sign with this key; unsigned without it
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
DIST=${1:-resolute}
DL=${DL:-/home/grigory/Storage/Claude/kokoro-mul-acpp/dl}
OUT=${OUT:-/home/grigory/Storage/Claude/kokoro-mul-acpp/ppa}
VOICES=$HERE/../../kokoro-models/voices.list
HEXGRAD_REV=f3ff3571791e39611d31c381e3a41a3af07b4987
UPSTREAM=0~20261125.f3ff357
SRC=audiocpp-tts-model-kokoro-mul
VERSION=$UPSTREAM-${REVISION:-1~ppa1}
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}
MAINT="$DEBFULLNAME <$DEBEMAIL>"

(cd "$DL" && echo "ddc60cafcc71257e942aa657b73384246946fc2ef77fcf3c4ba8f6da5b27ac42  kokoro-82m-q8_0.gguf" | sha256sum -c --quiet)
NV=$(grep -vc '^#' "$VOICES")
[ "$NV" = 41 ]

rm -rf "$OUT"
S=$OUT/$SRC-$UPSTREAM
mkdir -p "$S"
cp "$DL/kokoro-82m-q8_0.gguf" "$S/"
cp "$HERE/SHA256SUMS.src" "$S/"
{
  cat <<'Y'
[model]
family=kokoro_tts
file=kokoro-82m-q8_0.gguf
name=kokoro
language=en-US
text-language=en-us
phonemizer=kokoro
Y
  for v in $(grep -v '^#' "$VOICES"); do
    case $v in
      a?_*) l=en-US ;; b?_*) l=en-GB ;; e?_*) l=es ;; f?_*) l=fr ;;
      h?_*) l=hi ;; i?_*) l=it ;; p?_*) l=pt-BR ;; *) echo "voice $v?" >&2; exit 1 ;;
    esac
    case $v in ?f_*) g=female ;; ?m_*) g=male ;; *) exit 1 ;; esac
    printf '\n[voice %s]\ngender=%s\nvoice-id=%s\nlanguage=%s\n' "$v" "$g" "$v" "$l"
  done
} > "$S/audiocpp.ini"
find "$S" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
MTIME=2026-11-25T12:00:00+01:00
tar --sort=name --mtime="$MTIME" --owner=0 --group=0 --numeric-owner \
  -C "$OUT" -cf - "$SRC-$UPSTREAM" | xz -T0 -6 > "$OUT/${SRC}_$UPSTREAM.orig.tar.xz"

D=$S/debian
mkdir -p "$D/source"
echo "3.0 (quilt)" > "$D/source/format"
cat > "$D/rules" <<'Y'
#!/usr/bin/make -f
%:
	dh $@
Y
chmod 755 "$D/rules"
cat > "$D/changelog" <<Y
$SRC ($VERSION~$DIST) $DIST; urgency=medium

  * Kokoro-82M v1.0 (q8_0) with $NV voices for sd_audiocpp.

 -- $MAINT  $(date -R)
Y
M=usr/share/audiocpp-tts/kokoro-mul
cat > "$D/$SRC.install" <<Y
audiocpp.ini $M/
kokoro-82m-q8_0.gguf $M/
Y
cat > "$D/$SRC.docs" <<Y
SHA256SUMS.src
Y
cat > "$D/control" <<Y
Source: $SRC
Section: sound
Priority: optional
Maintainer: $MAINT
Build-Depends: debhelper-compat (= 13)
Standards-Version: 4.7.0
Homepage: https://huggingface.co/hexgrad/Kokoro-82M
Rules-Requires-Root: no

Package: $SRC
Architecture: all
Depends: \${misc:Depends}
Recommends: speech-dispatcher-audiocpp, libaudiocpp-tts
Enhances: speech-dispatcher-audiocpp
Description: Kokoro-82M voices for English, Spanish, French, Hindi, Italian, Portuguese (audio.cpp)
 The multilingual neural speech model Kokoro-82M v1.0 as a GGUF file for
 audio.cpp, with $NV voices for American and British English, Spanish,
 French, Hindi, Italian and Brazilian Portuguese, for the speech-dispatcher
 synthesizer audiocpp (speech-dispatcher-audiocpp).
Y
cat > "$D/copyright" <<Y
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Kokoro-82M
Source: https://huggingface.co/hexgrad/Kokoro-82M
Comment: revision $HEXGRAD_REV, converted to GGUF (q8_0) with audio.cpp's
 prepare_kokoro_gguf.py; SHA256SUMS.src lists the source files.

Files: kokoro-82m-q8_0.gguf SHA256SUMS.src
Copyright: 2024-2025 hexgrad
License: Apache-2.0

Files: audiocpp.ini debian/*
Copyright: 2026 $MAINT
License: Apache-2.0

License: Apache-2.0
 On Debian systems the full text of the Apache License version 2.0 is in
 /usr/share/common-licenses/Apache-2.0.
Y

cd "$S"
if [ -n "${SIGN_KEY:-}" ]; then
  dpkg-buildpackage -S -d ${ORIG:--sa} --sign-key="$SIGN_KEY"
else
  dpkg-buildpackage -S -d -us -uc ${ORIG:--sa}
fi
