#!/bin/sh
# Builds speech-dispatcher-ru-normalizr (Architecture: all): ru-normalizr and
# its pure-Python dependencies that Ubuntu lacks, in the private directory
# /usr/lib/speech-dispatcher-modules/python that tts-normalize puts first on
# its sys.path. Nothing goes into the system Python.
#
#   build-deb.sh <ru-normalizr source dir> <out dir>
#
# Run it in a container (ubuntu:<dist>, python3-pip installed): pip downloads
# the packages from PyPI.
set -eu
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$1; OUT=$2
VER=${VER:-$(sed -n 's/^version = "\(.*\)"/\1/p' "$SRC/pyproject.toml")-1~local}
PKG=speech-dispatcher-ru-normalizr
W=$OUT/root/$PKG
P=$W/usr/lib/speech-dispatcher-modules/python
D=$W/usr/share/doc/$PKG
rm -rf "$OUT/root"; mkdir -p "$W/DEBIAN" "$P" "$D"

pip install --break-system-packages -q --no-compile --no-deps \
  --target "$P" -r "$HERE/requirements.txt"
pip install --break-system-packages -q --no-compile --no-deps --target "$P" "$SRC"
rm -rf "$P/bin"
find "$P" -name __pycache__ -prune -exec rm -rf {} +
# the licences stay with each *.dist-info; list them for the copyright file
find "$P" -type d -exec chmod 755 {} +; find "$P" -type f -exec chmod 644 {} +

cat > "$D/copyright" <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Comment: Bundled Python packages; each keeps its licence text in its
 /usr/lib/speech-dispatcher-modules/python/*.dist-info directory.

Files: python/ru_normalizr*
Copyright: Nick Zaitsev and contributors
License: MIT

Files: python/pymorphy3*
Copyright: Mikhail Korobov, Danylo Halaiko and contributors
License: MIT
Comment: pymorphy3-dicts-ru is built from the OpenCorpora dictionary
 (CC BY-SA 3.0, http://opencorpora.org).

Files: python/dawg_python* python/dawg2_python*
Copyright: Mikhail Korobov and contributors
License: MIT

Files: python/eng_to_ipa*
Copyright: Michael Phillips
License: MIT
Comment: includes the CMU Pronouncing Dictionary (BSD-2-Clause).

Files: python/phonenumbers*
Copyright: Google Inc. and David Drysdale
License: Apache-2.0
X
cat > "$W/DEBIAN/control" <<X
Package: $PKG
Version: $VER
Architecture: all
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Depends: python3 (>= 3.10), python3-num2words (>= 0.5.13), python3-num2words (<< 0.6), python3-roman
Enhances: speech-dispatcher-text-normalizer
Section: python
Priority: optional
Installed-Size: $(du -sk "$W/usr" | cut -f1)
Homepage: https://github.com/NickZaitsev/ru-normalizr
Description: Russian text normalizer for the speech-dispatcher modules
 ru-normalizr turns numbers, dates, units and abbreviations of Russian text
 into words before a voice reads it. This package keeps it and the libraries
 Ubuntu lacks (pymorphy3 with its Russian dictionary, eng_to_ipa,
 phonenumbers) in a private directory for tts-normalize.
X
dpkg-deb --root-owner-group -Zxz --build "$W" "$OUT/${PKG}_${VER}_all.deb"
