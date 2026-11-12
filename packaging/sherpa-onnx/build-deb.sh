#!/bin/sh
# Package the sherpa-onnx build (cmake --install into $B/stage) as a local .deb.
# Uses the system onnxruntime (libonnxruntime1.23); the onnxruntime copies that
# cmake installs next to the library are left out.
set -e
umask 022
VER=2.0.1-3~local
B=/home/grigory/Storage/Claude/sherpa-build
S=$(dirname "$(readlink -f "$0")")/src
ST=$B/stage/usr
PKG=$B/sherpa-onnx_$VER
LIB=usr/lib/x86_64-linux-gnu
rm -rf "$PKG"
mkdir -p "$PKG/usr/bin" "$PKG/$LIB/pkgconfig" "$PKG/usr/include" "$PKG/usr/share/doc/sherpa-onnx" "$PKG/DEBIAN"
for f in "$ST"/bin/*; do
  install -m755 "$f" "$PKG/usr/bin/"
done
install -m644 "$ST/lib/libsherpa-onnx-c-api.so" "$ST/lib/libsherpa-onnx-cxx-api.so" "$PKG/$LIB/"
cp -r "$ST/include/sherpa-onnx" "$PKG/usr/include/"
strip --strip-unneeded "$PKG"/usr/bin/* "$PKG/$LIB"/*.so
for f in "$PKG"/usr/bin/* "$PKG/$LIB"/*.so; do patchelf --remove-rpath "$f"; done
cat > "$PKG/$LIB/pkgconfig/sherpa-onnx.pc" <<PC
prefix=/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-gnu

Name: sherpa-onnx
Description: Speech recognition and synthesis with ONNX Runtime
URL: https://github.com/k2-fsa/sherpa-onnx
Version: 2.0.1
Cflags: -I\${includedir}
Libs: -lsherpa-onnx-c-api
Libs.private: -lonnxruntime
PC
install -m755 "$(dirname "$(readlink -f "$0")")/sherpa-say" "$PKG/usr/bin/"
cp "$S/LICENSE" "$PKG/usr/share/doc/sherpa-onnx/copyright"
cat > "$PKG/DEBIAN/control" <<CTL
Package: sherpa-onnx
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Installed-Size: $(du -sk "$PKG" | cut -f1)
Depends: libonnxruntime1.23, libc6, libstdc++6, libgcc-s1
Recommends: pulseaudio-utils | alsa-utils
Section: sound
Priority: optional
Homepage: https://github.com/k2-fsa/sherpa-onnx
Description: local speech recognition and synthesis (sherpa-onnx)
 sherpa-onnx runs speech models with ONNX Runtime on the CPU:
 text to speech (VITS/Piper, Matcha, Kokoro, Kitten, Pocket TTS,
 Supertonic, ZipVoice), speech recognition, VAD, speaker diarization.
 Models: https://github.com/k2-fsa/sherpa-onnx/releases
 .
 This package has the command line tools, the C and C++ API libraries
 and their headers. Piper voices need phonemes from outside since 2.0
 (sherpa-onnx no longer bundles espeak-ng).
CTL
dpkg-deb --root-owner-group -b "$PKG" "$(dirname "$(readlink -f "$0")")/sherpa-onnx_${VER}_amd64.deb"
