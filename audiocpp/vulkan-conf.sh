#!/bin/sh
# vulkan-conf.sh IN OUT: writes audiocpp-vulkan-rs.conf from audiocpp.conf
# with the back end "vulkan"
set -eu
{
  echo "# sd_audiocpp-vulkan-rs: the synthesizer audiocpp on the graphics card"
  echo "# (ggml's Vulkan back end). The settings are those of audiocpp.conf."
  echo
  sed -e '1,/^$/d' -e 's/^#\(AudiocppBackend\) .*/\1 "vulkan"/' "$1"
} > "$2.tmp"
grep -q '^AudiocppBackend "vulkan"' "$2.tmp"
mv "$2.tmp" "$2"
