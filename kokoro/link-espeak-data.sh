#!/bin/sh
# link-espeak-data.sh SYSTEM-DATA KOKORO-DATA
# links everything of the system's espeak-ng-data but ru_dict into
# KOKORO-DATA, so espeak-ng finds its phoneme tables next to our ru_dict
set -e
SYS=$1
DEST=${DESTDIR}${MESON_INSTALL_PREFIX%/}/${2#/}
case "$2" in /*) DEST=${DESTDIR}$2 ;; esac
for f in "$SYS"/*; do
  b=$(basename "$f")
  [ "$b" = ru_dict ] || ln -sfn "$f" "$DEST/$b"
done
