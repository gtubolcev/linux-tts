#!/bin/sh
# Builds speech-dispatcher-silero (sd_silero + default config) into this directory
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
VER=0.1-2~local
W=$HERE/deb/speech-dispatcher-silero
rm -rf "$HERE/deb"; mkdir -p $W/DEBIAN $W/usr/lib/speech-dispatcher-modules \
    $W/etc/speech-dispatcher/modules $W/usr/share/doc/speech-dispatcher-silero
docker run --rm -u $(id -u):$(id -g) -v $HERE:/w -w /w sd-build sh -c \
  'gcc -O2 -Wall -Wextra -Wno-unused-parameter -I/usr/include/speech-dispatcher -Ishim $(pkg-config --cflags glib-2.0) sd_silero.c -o sd_silero -lspeechd_module $(pkg-config --libs glib-2.0) -lpthread -lm'
install -m755 $HERE/sd_silero $W/usr/lib/speech-dispatcher-modules/sd_silero
mkdir -p $W/usr/bin && install -m755 $HERE/silero-server $W/usr/bin/silero-server
strip $W/usr/lib/speech-dispatcher-modules/sd_silero
install -m644 $HERE/silero.conf $W/etc/speech-dispatcher/modules/silero.conf
echo /etc/speech-dispatcher/modules/silero.conf > $W/DEBIAN/conffiles
cat > $W/usr/share/doc/speech-dispatcher-silero/copyright <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sd_silero, silero-server

Files: *
Copyright: 2026 Grigory Tuboltsev
License: MIT
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
 SOFTWARE.
Comment: statically linked with libspeechd_module from speech-dispatcher
 (LGPL-2.1+), which is allowed for the object code; the module source is
 /usr/share/doc/speech-dispatcher-silero/sd_silero.c.gz
X
gzip -9n -c $HERE/sd_silero.c > $W/usr/share/doc/speech-dispatcher-silero/sd_silero.c.gz
cat > $W/DEBIAN/control <<X
Package: speech-dispatcher-silero
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Depends: libc6, libglib2.0-0t64, speech-dispatcher (>= 0.12), python3, python3-numpy, python3-onnxruntime, python3-num2words
Installed-Size: $(du -sk --total $W/usr $W/etc | tail -1 | cut -f1)
Description: Speech Dispatcher module for Silero TTS (Russian)
 Speaks with the neural Silero voices (v5_6_ru: aidar, baya, eugene,
 kseniya, xenia). The model runs in a resident silero-server (ONNX Runtime,
 CPU), loaded once; it survives speech-dispatcher reloads and stops after an
 hour without speech. Settings in /etc/speech-dispatcher/modules/silero.conf.
 .
 The model and its runtime are licensed CC BY-NC-SA 4.0 (non-commercial use
 only) and are not part of this package; install them, with their licences,
 into ~/.local/share/silero-tts/v5_6_ru.
X
dpkg-deb --root-owner-group -b $W $HERE/speech-dispatcher-silero_${VER}_amd64.deb
