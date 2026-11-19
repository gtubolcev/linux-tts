#!/bin/sh
# vulkan-conf.sh IN OUT: writes sherpa-onnx-vulkan-rs.conf from
# sherpa-onnx.conf: the provider "vulkan" and the families that run there
set -eu
{
  echo "# sd_sherpa-onnx-vulkan-rs: the synthesizer sherpa-onnx on the graphics card,"
  echo "# through the onnx-vulkan-rs plugin of onnxruntime (onnxruntime-ep-vulkan);"
  echo "# hidden without it. Only voices that run there are listed: Piper voices,"
  echo "# VITS models on characters (MMS) and Supertonic. The settings are those of"
  echo "# sherpa-onnx.conf."
  echo
  sed -e '1,/^$/d' -e 's/^#\(SherpaProvider\) .*/\1 "vulkan"/' \
      -e 's/^\(SherpaFamilies\) .*/\1 "supertonic piper vits"/' "$1"
} > "$2.tmp"
grep -q '^SherpaProvider "vulkan"' "$2.tmp"
grep -q '^SherpaFamilies "supertonic piper vits"' "$2.tmp"
mv "$2.tmp" "$2"
