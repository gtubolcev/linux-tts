#!/bin/sh
# vulkan-conf.sh IN OUT: writes kokoro-vulkan-rs.conf from kokoro.conf with
# the provider "vulkan"
set -eu
{
  echo "# sd_kokoro-vulkan-rs: the synthesizer kokoro on the graphics card, through"
  echo "# the onnx-vulkan-rs plugin of onnxruntime (onnxruntime-ep-vulkan); hidden"
  echo "# without it. The settings are those of kokoro.conf."
  echo
  sed -e '1,/^$/d' -e 's/^#\(KokoroProvider\) .*/\1 "vulkan"/' "$1"
} > "$2.tmp"
grep -q '^KokoroProvider "vulkan"' "$2.tmp"
mv "$2.tmp" "$2"
