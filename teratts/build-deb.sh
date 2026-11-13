#!/bin/sh
# Builds speech-dispatcher-teratts (sd_teratts and its default config) and
# teratts-model-v2 (the TeraTTSv2 release of TeraSpace without RUAccent:
# the stress comes from the Vosk dictionary) here.
# MODEL: the download of huggingface.co/TeraSpace/TeraTTSv2.
# MODULE_ONLY=1: only the module.
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-/home/grigory/Storage/Claude/tts-models/TeraTTSv2}
OUT=${OUT:-/home/grigory/Storage/Claude/teratts-deb}
VER=0.1-2~local
MVER=2.0-1~local
rm -rf "$OUT/root"; mkdir -p "$OUT/root"

MIT='License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.'

# ------------------------------------------------------------ the module
W=$OUT/root/speech-dispatcher-teratts
D=$W/usr/share/doc/speech-dispatcher-teratts
mkdir -p $W/DEBIAN $W/usr/lib/speech-dispatcher-modules $W/etc/speech-dispatcher/modules $D
$HERE/build.sh
install -m755 $HERE/sd_teratts $W/usr/lib/speech-dispatcher-modules/sd_teratts
strip $W/usr/lib/speech-dispatcher-modules/sd_teratts
install -m644 $HERE/teratts.conf $W/etc/speech-dispatcher/modules/teratts.conf
echo /etc/speech-dispatcher/modules/teratts.conf > $W/DEBIAN/conffiles
cat > $D/copyright <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sd_teratts

Files: *
Copyright: 2026 Grigory Tuboltsev
$MIT
Comment: teratts.c follows teratts.py of TeraSpace/TeraTTSv2 (the text
 preparation and the order of its four ONNX graphs). Linked statically with
 libspeechd_module from speech-dispatcher (LGPL-2.1+). The module source is
 in this directory (*.c.gz, *.h).
X
for f in sd_teratts.c teratts.c; do gzip -9n -c $HERE/$f > $D/$f.gz; done
for f in ru-stress.c num-words.c; do gzip -9n -c $HERE/../common/$f > $D/$f.gz; done
install -m644 $HERE/teratts.h $HERE/../common/ru-stress.h $HERE/../common/num-words.h $D/
cat > $W/DEBIAN/control <<X
Package: speech-dispatcher-teratts
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Depends: libc6, libglib2.0-0t64, libjson-glib-1.0-0, libonnxruntime1.23,
 speech-dispatcher (>= 0.12), teratts-model-v2
Recommends: vosk-tts-model-ru-0.9-multi
Installed-Size: $(du -sk --total $W/usr $W/etc | tail -1 | cut -f1)
Description: Speech Dispatcher module for TeraTTSv2 voices
 Speaks with the TeraTTSv2 voices of TeraSpace (Russian and English, 44.1 kHz)
 through ONNX Runtime, without Python. Latin words in Russian text are read
 as English; numbers are spelt out; the Russian stress marks come from the
 dictionary of Vosk TTS. The model is loaded into the module once and stays
 loaded. Settings in /etc/speech-dispatcher/modules/teratts.conf.
X
dpkg-deb --root-owner-group -b $W $OUT/speech-dispatcher-teratts_${VER}_amd64.deb

[ -n "$MODULE_ONLY" ] && exit 0

# ------------------------------------------------------------- the model
W=$OUT/root/teratts-model-v2
P=$W/usr/share/teratts/teratts-v2
D=$W/usr/share/doc/teratts-model-v2
mkdir -p $W/DEBIAN $P/models $D
install -m644 $MODEL/models/text_encoder.onnx $MODEL/models/duration_predictor.onnx \
  $MODEL/models/sampler_distilled_cfg3_8step.onnx $MODEL/models/vocoder.onnx $P/models/
cp -r $MODEL/styles $P/
chmod -R u=rwX,go=rX $P/styles
install -m644 $MODEL/unicode_indexer.json $MODEL/config.json $P/
# the licence asks for both files with every copy: next to the model and in doc
install -m644 $MODEL/LICENSE $MODEL/LICENSE-OPENRAIL $MODEL/README.md $P/
install -m644 $MODEL/LICENSE $D/LICENSE
install -m644 $MODEL/LICENSE-OPENRAIL $D/LICENSE-OPENRAIL
cat > $D/copyright <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: TeraTTSv2
Source: https://huggingface.co/TeraSpace/TeraTTSv2

Files: *
Copyright: 2026 TeraSpace (TeraTTSv2); Supertone (Supertonic, which it derives from)
License: OpenRAIL-M with Commercial Attribution 1.0
 The full terms are in LICENSE and LICENSE-OPENRAIL in this directory and in
 /usr/share/teratts/teratts-v2. The use restrictions of the BigScience
 Open RAIL-M License (Attachment A) apply to everyone who uses these voices.
 A commercial product or service that uses the model must say "Text-to-speech
 powered by TeraTTSv2 by TeraSpace" with a link to
 https://huggingface.co/TeraSpace/TeraTTSv2 in its public description.
Comment: The release without its ruaccent/ directory (RUAccent, MIT) and
 without the Python scripts; speech-dispatcher-teratts places the stress
 from the Vosk TTS dictionary instead.
X
cat > $W/DEBIAN/control <<X
Package: teratts-model-v2
Version: $MVER
Architecture: all
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Installed-Size: $(du -sk $W/usr | cut -f1)
Homepage: https://huggingface.co/TeraSpace/TeraTTSv2
Description: TeraTTSv2 voice model (Russian and English)
 TeraTTSv2 by TeraSpace, a flow-matching text to speech model (derived from
 Supertonic) with ten voices: four Russian, six English, two of them
 whispering. Used by speech-dispatcher-teratts.
 .
 Licence: OpenRAIL-M with a commercial attribution condition.
X
dpkg-deb --root-owner-group -Zzstd -z3 -b $W $OUT/teratts-model-v2_${MVER}_all.deb
rm -rf "$OUT/root"
ls -la $OUT
