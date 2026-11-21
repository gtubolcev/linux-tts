#!/bin/sh
# Makes the source package kokoro-tts-model-mul for the PPA: Kokoro-82M v1.0
# (hexgrad, Apache-2.0) in the ONNX form of onnx-community, with the voices
# of the languages sd_kokoro has a front end for (misaki's first letter: a
# en-US, b en-GB, e es, f fr, h hi, i it, p pt-BR; not j ja, z zh). sd_kokoro
# finds it as /usr/share/kokoro-tts/kokoro-mul. The files are pinned by
# revision and checked against SHA256SUMS.
#
#   make-source.sh [codename]
#
#   DL        where the downloads are (fetched when missing)
#   OUT       where the source package goes
#   REVISION  the Debian revision (default 2~ppa1)
#   ORIG      -sa uploads the orig tarball, -sd (default) not: it is in the PPA
#   SIGN_KEY  sign with this key; unsigned without it
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
DIST=${1:-resolute}
DL=${DL:-/home/grigory/Storage/Claude/kokoro-mul/dl}
OUT=${OUT:-/home/grigory/Storage/Claude/kokoro-mul/ppa}
ONNX_REV=1939ad2a8e416c0acfeecc08a694d14ef25f2231   # onnx-community/Kokoro-82M-v1.0-ONNX
ONNX=https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/$ONNX_REV
# onnx-community's config.json has only "model_type"; sd_kokoro reads the
# phoneme ids ("vocab") from hexgrad's, shipped in debian/ so that the orig
# tarball of 1-1 stays
HEXGRAD_REV=f3ff3571791e39611d31c381e3a41a3af07b4987   # hexgrad/Kokoro-82M
HEXGRAD_CONFIG_SHA256=5abb01e2403b072bf03d04fde160443e209d7a0dad49a423be15196b9b43c17f
UPSTREAM=1.0
SRC=kokoro-tts-model-mul
VERSION=$UPSTREAM-${REVISION:-2~ppa1}
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}
MAINT="$DEBFULLNAME <$DEBEMAIL>"
VOICES=$(grep -v '^#' "$HERE/voices.list" | grep -v '^[[:space:]]*$')

mkdir -p "$DL/voices"
get () {  # get <path in the repo> <local file>
  [ -s "$2" ] || curl -fL --retry 3 -o "$2" "$ONNX/$1"
}
get config.json "$DL/config.json"
get onnx/model.onnx "$DL/model.onnx"
for v in $VOICES; do get "voices/$v.bin" "$DL/voices/$v.bin"; done
(cd "$DL" && sha256sum -c --quiet "$HERE/SHA256SUMS")
[ -s "$DL/config-hexgrad.json" ] || curl -fL --retry 3 -o "$DL/config-hexgrad.json" \
  "https://huggingface.co/hexgrad/Kokoro-82M/resolve/$HEXGRAD_REV/config.json"
echo "$HEXGRAD_CONFIG_SHA256  $DL/config-hexgrad.json" | sha256sum -c --quiet

rm -rf "$OUT"
S=$OUT/$SRC-$UPSTREAM
mkdir -p "$S/voices"
cp "$DL/config.json" "$DL/model.onnx" "$S/"
for v in $VOICES; do cp "$DL/voices/$v.bin" "$S/voices/"; done
cp "$HERE/voices.list" "$HERE/SHA256SUMS" "$S/"
find "$S" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
MTIME=2026-11-21T12:00:01+01:00   # that of the orig tarball in the PPA
tar --sort=name --mtime="$MTIME" --owner=0 --group=0 --numeric-owner \
  -C "$OUT" -cf - "$SRC-$UPSTREAM" | xz -T0 -6 > "$OUT/${SRC}_$UPSTREAM.orig.tar.xz"

D=$S/debian
mkdir -p "$D/source"
echo "3.0 (quilt)" > "$D/source/format"
cp "$DL/config-hexgrad.json" "$D/config.json"
chmod 644 "$D/config.json"
cat > "$D/rules" <<'X'
#!/usr/bin/make -f
%:
	dh $@

override_dh_builddeb:
	dh_builddeb -- -Zxz -z1
X
chmod 755 "$D/rules"
cat > "$D/changelog" <<X
$SRC ($VERSION~$DIST) $DIST; urgency=medium

  * config.json of hexgrad/Kokoro-82M: onnx-community's has no "vocab",
    sd_kokoro could not load the model.
  * Kokoro-82M v1.0 for sd_kokoro: en-US, en-GB, es, fr, hi, it, pt-BR.

 -- $MAINT  $(date -R)
X
cat > "$D/install" <<X
debian/config.json usr/share/kokoro-tts/kokoro-mul/
model.onnx usr/share/kokoro-tts/kokoro-mul/
voices/*.bin usr/share/kokoro-tts/kokoro-mul/voices/
X
mv "$D/install" "$D/$SRC.install"
NV=$(echo "$VOICES" | wc -l)
cat > "$D/control" <<X
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
Enhances: speech-dispatcher-kokoro
Description: Kokoro-82M voices for English, Spanish, French, Hindi, Italian, Portuguese
 The multilingual neural speech model Kokoro-82M v1.0 with $NV voices for
 American and British English, Spanish, French, Hindi, Italian and
 Brazilian Portuguese, for the speech-dispatcher synthesizer kokoro
 (speech-dispatcher-kokoro). The model is the ONNX export of
 onnx-community.
X
cat > "$D/copyright" <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Kokoro-82M
Source: https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX
Comment: revision $ONNX_REV, the ONNX export of
 https://huggingface.co/hexgrad/Kokoro-82M

Files: *
Copyright: 2024-2025 hexgrad
License: Apache-2.0

Files: debian/config.json
Copyright: 2024-2025 hexgrad
Comment: https://huggingface.co/hexgrad/Kokoro-82M revision $HEXGRAD_REV
License: Apache-2.0

Files: debian/*
Copyright: 2026 $MAINT
License: Apache-2.0

License: Apache-2.0
 On Debian systems the full text of the Apache License version 2.0 is in
 /usr/share/common-licenses/Apache-2.0.
X

cd "$S"
if [ -n "${SIGN_KEY:-}" ]; then
  dpkg-buildpackage -S -d ${ORIG:--sd} --sign-key="$SIGN_KEY"
else
  dpkg-buildpackage -S -d -us -uc ${ORIG:--sd}
fi
