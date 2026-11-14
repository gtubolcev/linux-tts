#!/bin/sh
# Builds into this directory:
#   speech-dispatcher-sherpa-onnx        sd_sherpa-onnx, the synthesizers
#                                        sherpa-onnx and sherpa-onnx-vulkan-rs
#   speech-dispatcher-piper-sherpa-onnx  the same module as the synthesizers
#                                        piper-sherpa-onnx and
#                                        piper-sherpa-onnx-vulkan-rs
# speech-dispatcher finds a synthesizer as sd_<name> with modules/<name>.conf;
# the other names are links to sd_sherpa-onnx, the config tells them apart.
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
VER=0.1-6~local
VOICES=$HERE/../packaging/piper-voices/voices.list
MOD=usr/lib/speech-dispatcher-modules
CONF=etc/speech-dispatcher/modules
rm -rf "$HERE/deb"
# NOBUILD=1: sd_sherpa was compiled elsewhere (the build machine)
[ -n "$NOBUILD" ] || "$HERE/build.sh"

# <name>.conf -> <name>-vulkan-rs.conf: the same settings on the graphics card
vulkan_conf () {
  {
    echo "# sd_$1-vulkan-rs: the synthesizer $1 on the graphics card, through"
    echo "# the onnx-vulkan-rs plugin of onnxruntime (onnxruntime-ep-vulkan);"
    echo "# hidden without it. Only voices that run there are listed: $2."
    echo "# The settings are those of $1.conf."
    echo
    sed -e '1,/^$/d' -e "s/^#\($3Provider\) .*/\1 \"vulkan\"/" \
        -e "s/^\($3Families\) .*/\1 \"$4\"/" "$HERE/$1.conf"
  } > "$5"
}

copyright () {
  cat <<'X'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sd_sherpa-onnx

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
 /usr/share/doc/speech-dispatcher-sherpa-onnx/sd_sherpa.c.gz
X
}

# --- speech-dispatcher-sherpa-onnx
P=speech-dispatcher-sherpa-onnx
W=$HERE/deb/$P
mkdir -p "$W/DEBIAN" "$W/$MOD" "$W/$CONF" "$W/usr/share/doc/$P"
install -m755 "$HERE/sd_sherpa" "$W/$MOD/sd_sherpa-onnx"
strip "$W/$MOD/sd_sherpa-onnx"
ln -s sd_sherpa-onnx "$W/$MOD/sd_sherpa-onnx-vulkan-rs"
install -m644 "$HERE/sherpa-onnx.conf" "$W/$CONF/sherpa-onnx.conf"
vulkan_conf sherpa-onnx "VITS models on characters (MMS)" Sherpa vits \
    "$W/$CONF/sherpa-onnx-vulkan-rs.conf"
printf '/%s\n' "$CONF/sherpa-onnx.conf" "$CONF/sherpa-onnx-vulkan-rs.conf" \
    > "$W/DEBIAN/conffiles"
copyright > "$W/usr/share/doc/$P/copyright"
gzip -9n -c "$HERE/sd_sherpa.c" > "$W/usr/share/doc/$P/sd_sherpa.c.gz"
cat > "$W/DEBIAN/control" <<X
Package: $P
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Depends: libc6, libglib2.0-0t64, libespeak-ng1, speech-dispatcher (>= 0.12), sherpa-onnx (>= 2.0.1)
Recommends: sherpa-onnx-tts-supertonic-3 | sherpa-onnx-tts-pocket | sherpa-onnx-tts-mms-ru
Suggests: onnxruntime-ep-vulkan, speech-dispatcher-piper-sherpa-onnx
Replaces: speech-dispatcher-sherpa
Breaks: speech-dispatcher-sherpa
Provides: speech-dispatcher-sherpa
Installed-Size: $(du -sk --total "$W/usr" "$W/etc" | tail -1 | cut -f1)
Description: Speech Dispatcher module for sherpa-onnx voices
 Speaks with neural voices through sherpa-onnx: Supertonic 3 (31 languages,
 among them Russian and English, 5 female and 5 male voices), Pocket TTS
 (English) and MMS (one language per model). The model is loaded into the
 module once and stays loaded, so only the first message waits for it.
 .
 With onnxruntime-ep-vulkan installed, the synthesizer
 "sherpa-onnx-vulkan-rs" speaks the MMS voices on the graphics card.
 Piper voices are the synthesizer "piper-sherpa-onnx"
 (speech-dispatcher-piper-sherpa-onnx).
X
dpkg-deb --root-owner-group -b "$W" "$HERE/${P}_${VER}_amd64.deb"

# --- speech-dispatcher-piper-sherpa-onnx
P=speech-dispatcher-piper-sherpa-onnx
W=$HERE/deb/$P
mkdir -p "$W/DEBIAN" "$W/$MOD" "$W/$CONF" "$W/usr/share/doc/$P"
ln -s sd_sherpa-onnx "$W/$MOD/sd_piper-sherpa-onnx"
ln -s sd_sherpa-onnx "$W/$MOD/sd_piper-sherpa-onnx-vulkan-rs"
install -m644 "$HERE/piper-sherpa-onnx.conf" "$W/$CONF/piper-sherpa-onnx.conf"
vulkan_conf piper-sherpa-onnx "all Piper voices" Piper piper \
    "$W/$CONF/piper-sherpa-onnx-vulkan-rs.conf"
printf '/%s\n' "$CONF/piper-sherpa-onnx.conf" "$CONF/piper-sherpa-onnx-vulkan-rs.conf" \
    > "$W/DEBIAN/conffiles"
copyright > "$W/usr/share/doc/$P/copyright"
# the packaged voices, one Recommends each
RECOMMENDS=$(grep -v '^#' "$VOICES" | awk 'NF { printf "%spiper-voice-%s", sep, tolower($1); sep=", " }' \
             | tr '_' '-')
cat > "$W/DEBIAN/control" <<X
Package: $P
Version: $VER
Architecture: all
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: sound
Priority: optional
Depends: speech-dispatcher-sherpa-onnx (>= $VER), piper-espeak-ng-data, piper-voice
Recommends: $RECOMMENDS
Suggests: onnxruntime-ep-vulkan
Installed-Size: $(du -sk --total "$W/usr" "$W/etc" | tail -1 | cut -f1)
Description: Speech Dispatcher synthesizer for Piper voices
 Speaks with Piper neural voices through sherpa-onnx, as the synthesizer
 "piper-sherpa-onnx" (the module of speech-dispatcher-sherpa-onnx under
 another name). The voices are the piper-voice-* packages; the other Piper
 voices can be installed with the speech settings app.
 .
 With onnxruntime-ep-vulkan installed, the synthesizer
 "piper-sherpa-onnx-vulkan-rs" speaks them on the graphics card.
X
dpkg-deb --root-owner-group -b "$W" "$HERE/${P}_${VER}_all.deb"
