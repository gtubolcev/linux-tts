#!/bin/sh
# Makes the source package ruaccent-models for the PPA: the RUAccent
# (ruaccent/accentuator, MIT) models that resolve Russian homographs from
# the context, for TextStressHomographs tiny|turbo of the modules
# (common/ru-homograph.c). Binary packages ruaccent-model-tiny (tiny2.1) and
# ruaccent-model-turbo (turbo3.1), each self-contained in
# /usr/share/linux-tts/ruaccent/<size>: the model and its tokenizer, the
# dictionaries omographs/yo_homographs and the ё model in yo/, the same
# layout as ruaccent-models.py makes in ~/.local/share. The files and their
# sha256 come from data/ruaccent/models.json (pinned revision); the helper
# itself downloads and checks them.
#
#   make-source.sh [codename]
#
#   DL        where the downloads are (fetched when missing)
#   OUT       where the source package goes
#   REVISION  the Debian revision (default 1~ppa1)
#   ORIG      -sa (default) uploads the orig tarball, -sd not: it is in the PPA
#   SIGN_KEY  sign with this key; unsigned without it
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$(dirname "$HERE")")
DIST=${1:-resolute}
DL=${DL:-/home/grigory/Storage/Claude/ruaccent-models/dl}
OUT=${OUT:-/home/grigory/Storage/Claude/ruaccent-models/ppa}
CAT=$ROOT/data/ruaccent/models.json
REV=$(python3 -I -c 'import json, re, sys
c = json.load(open(sys.argv[1]))
print(re.search(r"/resolve/([0-9a-f]{40})/", c["models"][0]["downloads"][0]["url"]).group(1))' "$CAT")
UPSTREAM=1.0+git${REV%${REV#???????}}
SRC=ruaccent-models
VERSION=$UPSTREAM-${REVISION:-1~ppa1}
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}
MAINT="$DEBFULLNAME <$DEBEMAIL>"

# the helper with the catalogue next to it, as installed
mkdir -p "$DL/helper/models" "$DL/xdg"
cp "$ROOT/audiocpp/audiocpp-models.py" "$DL/helper/ruaccent-models.py"
cp "$CAT" "$DL/helper/models/models.json"
for size in tiny turbo; do
  [ -f "$DL/xdg/linux-tts/ruaccent/$size/model.onnx" ] \
    || XDG_DATA_HOME="$DL/xdg" python3 -I "$DL/helper/ruaccent-models.py" install $size >/dev/null
done

rm -rf "$OUT"
S=$OUT/$SRC-$UPSTREAM
mkdir -p "$S"
cp -r "$DL/xdg/linux-tts/ruaccent/tiny" "$DL/xdg/linux-tts/ruaccent/turbo" "$S/"
cp "$CAT" "$S/models.json"
find "$S" -name '.*' -exec rm -rf {} +
find "$S" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
MTIME=2026-11-25T12:00:00+01:00
tar --sort=name --mtime="$MTIME" --owner=0 --group=0 --numeric-owner \
  -C "$OUT" -cf - "$SRC-$UPSTREAM" | xz -T0 -6 > "$OUT/${SRC}_$UPSTREAM.orig.tar.xz"

D=$S/debian
mkdir -p "$D/source"
echo "3.0 (quilt)" > "$D/source/format"
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

  * RUAccent tiny2.1 and turbo3.1 for TextStressHomographs of the
    speech-dispatcher-modules, revision $REV of ruaccent/accentuator.

 -- $MAINT  $(date -R)
X
for size in tiny turbo; do
  echo "$size usr/share/linux-tts/ruaccent/" > "$D/ruaccent-model-$size.install"
done
cat > "$D/control" <<X
Source: $SRC
Section: sound
Priority: optional
Maintainer: $MAINT
Build-Depends: debhelper-compat (= 13)
Standards-Version: 4.7.0
Homepage: https://huggingface.co/ruaccent/accentuator
Rules-Requires-Root: no

Package: ruaccent-model-tiny
Architecture: all
Depends: \${misc:Depends}
Enhances: speech-dispatcher-text-normalizer
Description: RUAccent small model for the stress of Russian homographs
 RUAccent tiny2.1 chooses the stress of Russian homographs from the
 sentence (за́мок / замо́к) and puts ё where the context asks for it (все /
 всё). The speech-dispatcher-modules voices use it with
 "TextStressHomographs tiny"; about 1 ms a sentence.

Package: ruaccent-model-turbo
Architecture: all
Depends: \${misc:Depends}
Enhances: speech-dispatcher-text-normalizer
Description: RUAccent large model for the stress of Russian homographs
 RUAccent turbo3.1 chooses the stress of Russian homographs from the
 sentence (за́мок / замо́к) and puts ё where the context asks for it (все /
 всё), a little more accurately than the small model, with more memory
 (about 570 MB while loaded) and time (about 10 ms a sentence). The
 speech-dispatcher-modules voices use it with "TextStressHomographs turbo".
X
cat > "$D/copyright" <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: RUAccent
Source: https://huggingface.co/ruaccent/accentuator
Comment: revision $REV; the files as listed with their sha256 in models.json

Files: *
Copyright: 2023-2025 Denis Petrov and the RUAccent contributors
License: MIT

Files: debian/* models.json
Copyright: 2026 $MAINT
License: MIT

License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 DEALINGS IN THE SOFTWARE.
X

cd "$S"
if [ -n "${SIGN_KEY:-}" ]; then
  dpkg-buildpackage -S -d ${ORIG:--sa} --sign-key="$SIGN_KEY"
else
  dpkg-buildpackage -S -d -us -uc ${ORIG:--sa}
fi
