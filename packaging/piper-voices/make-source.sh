#!/bin/sh
# Makes the source package piper-voices for the PPA: the orig tarball holds
# the voices of voices.list as taken from the sherpa-onnx tts-models release
# (model, its JSON, tokens, MODEL_CARD) and the espeak-ng data they share;
# debian/ is generated from voices.list. The Russian rules of that data are
# rebuilt at build time from the rhasspy espeak-ng sources with the rules of
# sherpa/espeak-ru, which honour the stress mark U+0301 (ru-dict.sh). build-debs.sh builds the same
# packages locally without a source package.
#
#   make-source.sh [codename]
#
#   DL        where the tarballs are (downloaded when missing)
#   OUT       where the source package goes
#   REVISION  the Debian revision (default 1~ppa1)
#   SIGN_KEY  sign with this key; unsigned without it
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
DIST=${1:-resolute}
DL=${DL:-/home/grigory/Storage/Claude/piper-voices/dl}
OUT=${OUT:-/home/grigory/Storage/Claude/piper-voices/ppa}
URL=https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models
UPSTREAM=1.1
# the espeak-ng fork whose data the voices were trained with
ESPEAK=8593723f10cfd9befd50de447f14bf0a9d2a14a4
ESPEAK_SHA=41c199da1355deec93eab6065be99d00b938dd452e78f282ab481db91657e4e4
VERSION=$UPSTREAM-${REVISION:-1~ppa1}
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}
MAINT="$DEBFULLNAME <$DEBEMAIL>"

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

voices () {
  grep -v '^#' "$HERE/voices.list" | grep -v '^[[:space:]]*$'
}

rm -rf "$OUT"
mkdir -p "$OUT" "$DL"
S=$OUT/piper-voices-$UPSTREAM
mkdir -p "$S/voices"
cp "$HERE/voices.list" "$S/"

# the orig tarball: one directory per voice plus the shared espeak-ng data
voices | while read -r KEY LIC; do
  TAR=$DL/vits-piper-$KEY.tar.bz2
  [ -s "$TAR" ] || curl -fL --retry 3 -o "$TAR" "$URL/vits-piper-$KEY.tar.bz2"
  X=$OUT/x
  rm -rf "$X"
  mkdir "$X"
  tar -xjf "$TAR" -C "$X" --no-same-owner --no-same-permissions
  SRC=$X/vits-piper-$KEY
  [ -f "$SRC/$KEY.onnx" ] && [ -f "$SRC/tokens.txt" ] && [ -d "$SRC/espeak-ng-data" ] \
    || { echo "$TAR: unexpected contents" >&2; exit 1; }
  if grep -q '"phoneme_type": *"text"' "$SRC/$KEY.onnx.json"; then
    echo "$KEY: phoneme_type text is not supported" >&2; exit 1
  fi
  if [ ! -d "$S/piper-espeak-ng-data" ]; then
    cp -r "$SRC/espeak-ng-data" "$S/piper-espeak-ng-data"
  elif ! diff -rq "$S/piper-espeak-ng-data" "$SRC/espeak-ng-data" >/dev/null; then
    echo "$KEY: its espeak-ng-data differs from the others" >&2; exit 1
  fi
  mkdir "$S/voices/$KEY"
  cp "$SRC/$KEY.onnx" "$SRC/$KEY.onnx.json" "$SRC/tokens.txt" "$SRC/MODEL_CARD" \
    "$S/voices/$KEY/"
  rm -rf "$X"
  echo "$KEY" >&2
done
# the sources of the Russian dictionary, and the stress rules
ET=$DL/rhasspy-espeak-ng-$ESPEAK.tar.gz
[ -s "$ET" ] || curl -fL --retry 3 -o "$ET" "https://github.com/rhasspy/espeak-ng/archive/$ESPEAK.tar.gz"
echo "$ESPEAK_SHA  $ET" | sha256sum -c --quiet
mkdir -p "$S/espeak-ng-ru/stress"
tar -xzf "$ET" -C "$S/espeak-ng-ru" --strip-components=1 --no-same-owner \
  "espeak-ng-$ESPEAK/dictsource/ru_rules" "espeak-ng-$ESPEAK/dictsource/ru_list" \
  "espeak-ng-$ESPEAK/dictsource/ru_emoji" "espeak-ng-$ESPEAK/dictsource/extra/ru_listx"
cp "$HERE/../../sherpa/espeak-ru/ru_rules_stress" "$HERE/../../sherpa/espeak-ru/merge-stress-rules.py" \
  "$S/espeak-ng-ru/stress/"
find "$S" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
# the tarballs carry 1970 times, which dpkg-source dislikes
NOW=$(date -u +%Y-%m-%dT%H:%M:%SZ)
tar --sort=name --mtime="$NOW" --owner=0 --group=0 --numeric-owner \
  -C "$OUT" -cf - "piper-voices-$UPSTREAM" | xz -T0 -6 > "$OUT/piper-voices_$UPSTREAM.orig.tar.xz"

# debian/
D=$S/debian
mkdir -p "$D/source"
echo "3.0 (quilt)" > "$D/source/format"
cat > "$D/rules" <<'X'
#!/usr/bin/make -f
%:
	dh $@

# the models hardly compress; xz -9 would only cost time
override_dh_auto_build:
	rm -rf build && mkdir build && cp -r piper-espeak-ng-data build/
	sh debian/ru-dict.sh espeak-ng-ru/dictsource espeak-ng-ru/stress build/piper-espeak-ng-data

override_dh_auto_clean:
	rm -rf build

override_dh_builddeb:
	dh_builddeb -- -Zxz -z1
X
chmod 755 "$D/rules"
cp "$HERE/ru-dict.sh" "$D/"
cat > "$D/changelog" <<X
piper-voices ($VERSION~$DIST) $DIST; urgency=medium

  * Piper voices with free licences for sd_sherpa-onnx.
  * The Russian rules honour the stress mark U+0301.

 -- $MAINT  $(date -R)
X

{
  cat <<X
Source: piper-voices
Section: sound
Priority: optional
Maintainer: $MAINT
Build-Depends: debhelper-compat (= 13), espeak-ng, python3
Standards-Version: 4.7.0
Homepage: https://huggingface.co/rhasspy/piper-voices
Rules-Requires-Root: no

Package: piper-espeak-ng-data
Architecture: all
Depends: \${misc:Depends}
Description: espeak-ng data for Piper voices
 The pronunciation rules and dictionaries Piper voices were trained with,
 shared by the piper-voice-* packages. They differ slightly from the data
 of the espeak-ng package, so they are kept apart. The Russian rules also
 honour the stress mark U+0301 after a vowel.
X
  voices | while read -r KEY LIC; do
    LANG_=${KEY%%-*}; REST=${KEY#*-}; NAME=${REST%-*}; QUALITY=${REST##*-}
    cat <<X

Package: $(pkg_name "$KEY")
Architecture: all
Depends: piper-espeak-ng-data (= \${source:Version}), \${misc:Depends}
Provides: piper-voice
Enhances: speech-dispatcher-sherpa-onnx
Description: Piper voice "$NAME" ($LANG_, $QUALITY quality)
 A neural voice for the speech-dispatcher synthesizer sherpa-onnx,
 in the sherpa-onnx form of the Piper voice $KEY.
X
  done
} > "$D/control"

echo "build/piper-espeak-ng-data usr/share/sherpa-onnx/" > "$D/piper-espeak-ng-data.install"

# copyright: the source as a whole, and each binary its own
{
  cat <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: piper-voices
Source: https://huggingface.co/rhasspy/piper-voices
Comment: the voices as converted for sherpa-onnx by its authors:
 $URL/

Files: *
Copyright: see the MODEL_CARD of each voice
License: CC0-1.0 or CC-BY-4.0 or public-domain, per voice: see voices.list

Files: piper-espeak-ng-data/* espeak-ng-ru/*
Copyright: 1995-2014 Jonathan Duddington
 2015-2025 Reece H. Dunn and the espeak-ng contributors
 2022 Kirill Kolov
License: GPL-3+
 On Debian systems the full text of the GNU General Public License version 3
 is in /usr/share/common-licenses/GPL-3.

Files: debian/*
Copyright: 2026 $MAINT
License: CC0-1.0
X
  voices | while read -r KEY LIC; do
    echo
    echo "Files: voices/$KEY/*"
    echo "Copyright: see voices/$KEY/MODEL_CARD"
    echo "License: $LIC"
    licence_text "$LIC"
  done
  echo
  echo "License: CC0-1.0"
  licence_text CC0-1.0
  echo
  echo "License: CC-BY-4.0"
  licence_text CC-BY-4.0
  echo
  echo "License: public-domain"
  licence_text public-domain
} > "$D/copyright"

sed -n '/^Files: piper-espeak-ng-data/,/^$/p' "$D/copyright" | sed '1s/.*/Files: */' > "$D/piper-espeak-ng-data.copyright.body"
{
  echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
  echo "Upstream-Name: espeak-ng (the data Piper voices were trained with)"
  echo "Source: https://github.com/rhasspy/espeak-ng"
  echo
  cat "$D/piper-espeak-ng-data.copyright.body"
} > "$D/piper-espeak-ng-data.copyright"
rm "$D/piper-espeak-ng-data.copyright.body"

voices | while read -r KEY LIC; do
  P=$(pkg_name "$KEY")
  V=usr/share/sherpa-onnx/tts/vits-piper-$KEY
  printf 'voices/%s/%s.onnx %s/\nvoices/%s/%s.onnx.json %s/\nvoices/%s/tokens.txt %s/\n' \
    "$KEY" "$KEY" "$V" "$KEY" "$KEY" "$V" "$KEY" "$V" > "$D/$P.install"
  echo "usr/share/sherpa-onnx/piper-espeak-ng-data $V/espeak-ng-data" > "$D/$P.links"
  echo "voices/$KEY/MODEL_CARD" > "$D/$P.docs"
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
  } > "$D/$P.copyright"
done

cd "$S"
if [ -n "${SIGN_KEY:-}" ]; then
  dpkg-buildpackage -S -d -sa --sign-key="$SIGN_KEY"
else
  dpkg-buildpackage -S -d -us -uc -sa
fi
