#!/bin/sh
# ci.sh [full|minimal] - build and run the tests on Ubuntu, as root or with sudo.
# full: the engines from ppa:gtubolcev/linux-tts (onnxruntime, sherpa-onnx),
# all modules; minimal: distribution packages only, the frame and its tests.
# Used by .github/workflows/ci.yml; runs the same in a plain ubuntu container.
set -eu
mode=${1:-full}
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq
pkgs="build-essential meson pkgconf python3 espeak-ng libespeak-ng-dev
  libglib2.0-dev libjson-glib-dev libsonic-dev libspeechd-dev"
if [ "$mode" = full ]; then
  $SUDO apt-get install -y -qq --no-install-recommends software-properties-common gpg-agent >/dev/null
  $SUDO add-apt-repository -y -n ppa:gtubolcev/linux-tts >/dev/null
  $SUDO apt-get update -qq
  pkgs="$pkgs libsherpa-onnx-tts-dev libonnxruntime-tts-dev"
  opts="-Dvosk=enabled -Dkokoro=enabled -Dteratts=enabled -Dsherpa=enabled -Dsilero=enabled"
else
  opts="-Dvosk=disabled -Dkokoro=disabled -Dteratts=disabled -Dsherpa=disabled"
fi
# shellcheck disable=SC2086
$SUDO apt-get install -y -qq --no-install-recommends $pkgs >/dev/null
# shellcheck disable=SC2086
meson setup _ci $opts --werror -Dwarning_level=2
meson compile -C _ci
meson test -C _ci --print-errorlogs
