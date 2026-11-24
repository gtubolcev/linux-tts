#!/bin/sh
# Makes the source package audiocpp-tts-model-shtorm-ru for the PPA:
# Shtorm-PocketTTS-RU (ArtShtorm/Shtorm-PocketTTS-RU, CC BY 4.0, revision
# 33f6c32), the Russian Pocket TTS model, "fast" variant, q8_0, with the 206
# designed reference voices of davnozdu/supertonic-android (release
# shtorm-designed-voices-v1, repository GPL-3.0; the voices carry no licence
# of their own, they are treated as GPL-3.0 until the author says otherwise).
# sd_audiocpp finds it as /usr/share/audiocpp-tts/shtorm-ru.
#
#   make-source.sh [codename]
#
#   DL        shtorm-ru-fast-q8_0.gguf (made by hand with audiocpp_gguf, see
#             RESEARCH-tts-engines.md) and refs/ (convert-voices.py output)
#   OUT       where the source package goes
#   REVISION  the Debian revision (default 1~ppa1)
#   ORIG      -sa (default) uploads the orig tarball, -sd not
#   SIGN_KEY  sign with this key; unsigned without it
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
DIST=${1:-resolute}
DL=${DL:-/home/grigory/Storage/Claude/shtorm-model/dl}
OUT=${OUT:-/home/grigory/Storage/Claude/shtorm-model/ppa}
UPSTREAM=0~20261124.33f6c32
SRC=audiocpp-tts-model-shtorm-ru
VERSION=$UPSTREAM-${REVISION:-1~ppa1}
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}
MAINT="$DEBFULLNAME <$DEBEMAIL>"

(cd "$DL" && echo "1d684745d9cbe5cca302a34142acc9399bb906853a22948596cc577c5fc9f5c3  shtorm-ru-fast-q8_0.gguf" | sha256sum -c --quiet)
[ "$(ls "$DL/refs/voices" | wc -l)" = 206 ]

rm -rf "$OUT"
S=$OUT/$SRC-$UPSTREAM
mkdir -p "$S/voices"
cp "$DL/shtorm-ru-fast-q8_0.gguf" "$S/"
cp "$DL"/refs/voices/*.wav "$S/voices/"
cp "$DL/refs/index.json" "$S/voices-index.json"
cat "$HERE/audiocpp.head.ini" "$DL/refs/voices.ini" > "$S/audiocpp.ini"
cp "$HERE/README.Shtorm" "$S/"
find "$S" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
MTIME=2026-11-24T12:00:00+01:00
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

  * Shtorm-PocketTTS-RU (fast, q8_0) with 206 reference voices for
    sd_audiocpp.

 -- $MAINT  $(date -R)
Y
M=usr/share/audiocpp-tts/shtorm-ru
cat > "$D/$SRC.install" <<Y
audiocpp.ini $M/
shtorm-ru-fast-q8_0.gguf $M/
voices $M/
Y
cat > "$D/$SRC.docs" <<Y
README.Shtorm
voices-index.json
Y
cat > "$D/control" <<Y
Source: $SRC
Section: sound
Priority: optional
Maintainer: $MAINT
Build-Depends: debhelper-compat (= 13)
Standards-Version: 4.7.0
Homepage: https://huggingface.co/ArtShtorm/Shtorm-PocketTTS-RU
Rules-Requires-Root: no

Package: $SRC
Architecture: all
Depends: \${misc:Depends}
Recommends: speech-dispatcher-audiocpp, libaudiocpp-tts
Enhances: speech-dispatcher-audiocpp
Description: Shtorm Pocket TTS, a Russian voice model with 206 voices for audio.cpp
 Shtorm-PocketTTS-RU is a Russian text to speech model on the Pocket TTS
 architecture (24 kHz). It clones the voice of a short reference clip; this
 package ships 206 designed reference voices (women, men and children), so
 the speech-dispatcher synthesizer audiocpp (speech-dispatcher-audiocpp)
 offers each of them as a voice.
Y
cat > "$D/copyright" <<Y
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Shtorm-PocketTTS-RU
Source: https://huggingface.co/ArtShtorm/Shtorm-PocketTTS-RU
Comment: shtorm-ru-fast-q8_0.gguf is the "fast" model of revision 33f6c32,
 converted to GGUF (q8_0) with audio.cpp's audiocpp_gguf. The voices are
 the reference clips of the release shtorm-designed-voices-v1 of
 https://github.com/davnozdu/supertonic-android, generated with Qwen3-TTS
 VoiceDesign (Apache-2.0), decoded from mp3 to 24 kHz mono WAV unchanged
 otherwise. The upstream repository is GPL-3.0; the clips carry no licence
 of their own and are distributed under it.

Files: shtorm-ru-fast-q8_0.gguf
Copyright: ArtShtorm
License: CC-BY-4.0

Files: voices/* voices-index.json
Copyright: davnozdu
License: GPL-3+

Files: audiocpp.ini README.Shtorm debian/*
Copyright: 2026 $MAINT
License: GPL-3+

License: GPL-3+
 On Debian systems the full text of the GNU General Public License version 3
 is in /usr/share/common-licenses/GPL-3.

License: CC-BY-4.0
 Creative Commons Attribution 4.0 International, see
 https://creativecommons.org/licenses/by/4.0/legalcode. You may share and
 adapt the work if you give appropriate credit, link to the licence and
 indicate changes (here: conversion to GGUF and 8-bit quantisation).
Y

cd "$S"
if [ -n "${SIGN_KEY:-}" ]; then
  dpkg-buildpackage -S -d ${ORIG:--sa} --sign-key="$SIGN_KEY"
else
  dpkg-buildpackage -S -d -us -uc ${ORIG:--sa}
fi
