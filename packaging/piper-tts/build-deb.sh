#!/bin/sh
# Package the piper1-gpl build (staged by cmake --install) as piper-tts .deb.
# Layout: /usr/lib/piper-tts/{piper,libpiper.so,espeak-ng-data}, /usr/bin/piper-tts.
# piper looks for espeak-ng-data next to the real executable (/proc/self/exe).
set -e
umask 022
VER=1.8.0-1~local
B=/home/grigory/Storage/Claude/piper-build
S=$(dirname "$(readlink -f "$0")")/src
STAGE=$B/stage/usr/lib/piper-tts
PKG=$B/piper-tts_$VER
rm -rf "$PKG"
L=$PKG/usr/lib/piper-tts
mkdir -p "$L" "$PKG/usr/bin" "$PKG/usr/include" "$PKG/usr/share/doc/piper-tts" "$PKG/DEBIAN"
install -m755 "$STAGE/bin/piper_exe" "$L/piper"
install -m644 "$STAGE/lib/libpiper.so" "$L/"
cp -r "$STAGE/share/espeak-ng-data" "$L/"
install -m644 "$STAGE/include/piper.h" "$PKG/usr/include/"
strip --strip-unneeded "$L/piper" "$L/libpiper.so"
patchelf --set-rpath '$ORIGIN' "$L/piper"
ln -s ../lib/piper-tts/piper "$PKG/usr/bin/piper-tts"
cp "$S/COPYING" "$PKG/usr/share/doc/piper-tts/copyright"
cat > "$PKG/DEBIAN/control" <<CTL
Package: piper-tts
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Installed-Size: $(du -sk "$PKG" | cut -f1)
Depends: libonnxruntime1.23, libc6, libstdc++6, libgcc-s1
Section: sound
Priority: optional
Homepage: https://github.com/OHF-Voice/piper1-gpl
Description: fast local neural text to speech (Piper)
 Piper (piper1-gpl) synthesizes speech with VITS voices exported to ONNX.
 Voices: https://huggingface.co/rhasspy/piper-voices
 .
 The command is piper-tts (the name piper belongs to another package).
 Example: echo Hello | piper-tts -m en_US-lessac-high.onnx -f - | paplay
CTL
dpkg-deb --root-owner-group -b "$PKG" "$(dirname "$(readlink -f "$0")")/piper-tts_${VER}_amd64.deb"
