#!/bin/sh
# Builds a .deb for every voice in voices.list and piper-espeak-ng-data, the
# espeak-ng data the voices share, from the sherpa-onnx tts-models release.
#
#   DL     where the tarballs are (downloaded when missing)
#   OUT    where the .debs go
#
# Voices land in /usr/share/sherpa-onnx/tts/vits-piper-<key>/, where
# sd_piper-sherpa-onnx looks for them; their espeak-ng-data is a link to
# /usr/share/sherpa-onnx/piper-espeak-ng-data (the same 19 MB in every
# tarball, slightly different from the system espeak-ng-data: the one the
# voices were trained with).
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
DL=${DL:-/home/grigory/Storage/Claude/piper-voices/dl}
OUT=${OUT:-/home/grigory/Storage/Claude/piper-voices/debs}
URL=https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models
VER=1.0-1~local
MAINT="Grigory Tuboltsev <gtubolcev@gmail.com>"
WORK=${WORK:-/home/grigory/Storage/Claude/piper-voices/work}
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT" "$DL"

# piper-voice-<lang>-<region>-<name>-<quality>
pkg_name () {
  echo "piper-voice-$1" | tr 'A-Z_' 'a-z-'
}

licence_text () {
  case $1 in
    CC0-1.0) echo " https://creativecommons.org/publicdomain/zero/1.0/legalcode" ;;
    CC-BY-4.0) echo " https://creativecommons.org/licenses/by/4.0/legalcode" ;;
    public-domain) echo " The recordings and the model are in the public domain." ;;
    *) echo "unknown licence $1" >&2; exit 1 ;;
  esac
}

grep -v '^#' "$HERE/voices.list" | while read -r KEY LIC; do
  [ -n "$KEY" ] || continue
  TAR=$DL/vits-piper-$KEY.tar.bz2
  [ -s "$TAR" ] || curl -fL --retry 3 -o "$TAR" "$URL/vits-piper-$KEY.tar.bz2"
  X=$WORK/x-$KEY
  mkdir "$X"
  tar -xjf "$TAR" -C "$X" --no-same-owner --no-same-permissions
  SRC=$X/vits-piper-$KEY
  [ -f "$SRC/$KEY.onnx" ] && [ -f "$SRC/tokens.txt" ] && [ -d "$SRC/espeak-ng-data" ] \
    || { echo "$TAR: unexpected contents" >&2; exit 1; }
  if grep -q '"phoneme_type": *"text"' "$SRC/$KEY.onnx.json"; then
    echo "$KEY: phoneme_type text is not supported" >&2; exit 1
  fi

  # the shared espeak-ng data: taken from the first voice, the same in all
  if [ ! -d "$WORK/espeak-ng-data" ]; then
    cp -r "$SRC/espeak-ng-data" "$WORK/espeak-ng-data"
  elif ! diff -rq "$WORK/espeak-ng-data" "$SRC/espeak-ng-data" >/dev/null; then
    echo "$KEY: its espeak-ng-data differs from the others" >&2; exit 1
  fi

  P=$(pkg_name "$KEY")
  W=$WORK/$P
  D=$W/usr/share/sherpa-onnx/tts/vits-piper-$KEY
  mkdir -p "$W/DEBIAN" "$D" "$W/usr/share/doc/$P"
  install -m644 "$SRC/$KEY.onnx" "$SRC/$KEY.onnx.json" "$SRC/tokens.txt" "$D/"
  ln -s ../../piper-espeak-ng-data "$D/espeak-ng-data"
  install -m644 "$SRC/MODEL_CARD" "$W/usr/share/doc/$P/MODEL_CARD"
  {
    echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    echo "Upstream-Name: Piper voice $KEY"
    echo "Source: https://huggingface.co/rhasspy/piper-voices"
    echo
    echo "Files: *"
    echo "Copyright: see /usr/share/doc/$P/MODEL_CARD"
    echo "License: $LIC"
    licence_text "$LIC"
    echo "Comment: converted for sherpa-onnx by its authors:"
    echo " $URL/vits-piper-$KEY.tar.bz2"
  } > "$W/usr/share/doc/$P/copyright"

  LANG_=${KEY%%-*}; REST=${KEY#*-}; NAME=${REST%-*}; QUALITY=${REST##*-}
  cat > "$W/DEBIAN/control" <<X
Package: $P
Version: $VER
Architecture: all
Maintainer: $MAINT
Section: sound
Priority: optional
Depends: piper-espeak-ng-data
Provides: piper-voice
Enhances: speech-dispatcher-piper-sherpa-onnx
Installed-Size: $(du -sk "$W/usr" | cut -f1)
Description: Piper voice "$NAME" ($LANG_, $QUALITY quality)
 A neural voice for the speech-dispatcher synthesizer piper-sherpa-onnx,
 in the sherpa-onnx form of the Piper voice $KEY.
X
  dpkg-deb --root-owner-group -b "$W" "$OUT/${P}_${VER}_all.deb" >/dev/null
  echo "$P"
  rm -rf "$X" "$W"
done

# the espeak-ng data of the voices
P=piper-espeak-ng-data
W=$WORK/$P
mkdir -p "$W/DEBIAN" "$W/usr/share/sherpa-onnx" "$W/usr/share/doc/$P"
cp -r "$WORK/espeak-ng-data" "$W/usr/share/sherpa-onnx/piper-espeak-ng-data"
find "$W/usr" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
cat > "$W/usr/share/doc/$P/copyright" <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: espeak-ng (the data Piper voices were trained with)
Source: https://github.com/rhasspy/espeak-ng

Files: *
Copyright: 1995-2014 Jonathan Duddington
 2015-2025 Reece H. Dunn and the espeak-ng contributors
License: GPL-3+
 On Debian systems the full text of the GNU General Public License version 3
 is in /usr/share/common-licenses/GPL-3.
X
cat > "$W/DEBIAN/control" <<X
Package: $P
Version: $VER
Architecture: all
Maintainer: $MAINT
Section: sound
Priority: optional
Installed-Size: $(du -sk "$W/usr" | cut -f1)
Description: espeak-ng data for Piper voices
 The pronunciation rules and dictionaries Piper voices were trained with,
 shared by the piper-voice-* packages. They differ slightly from the data
 of the espeak-ng package, so they are kept apart.
X
dpkg-deb --root-owner-group -b "$W" "$OUT/${P}_${VER}_all.deb" >/dev/null
echo "$P"
rm -rf "$WORK"
