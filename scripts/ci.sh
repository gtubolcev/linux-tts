#!/bin/sh
# ci.sh [full|minimal|sanitize] - build and run the tests on Ubuntu, as root or
# with sudo.  full: the engines from ppa:gtubolcev/linux-tts (onnxruntime,
# sherpa-onnx), all modules; minimal: distribution packages only, the frame
# and its tests; sanitize: minimal under AddressSanitizer and UBSan (the
# protocol tests look for their reports in the module's stderr).
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
  opts="-Dvosk=disabled -Dkokoro=disabled -Dteratts=disabled -Dsherpa=disabled -Dsilero=disabled"
fi
if [ "$mode" = sanitize ]; then
  opts="$opts -Db_sanitize=address,undefined -Db_lundef=false"
  # leaks of the process-lifetime caches are not errors here; the rest aborts
  export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
  export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
fi
# shellcheck disable=SC2086
$SUDO apt-get install -y -qq --no-install-recommends $pkgs >/dev/null
if [ "$mode" = full ]; then
  # the PPA keeps its own library versions in a private path
  PKG_CONFIG_PATH=/usr/lib/$(gcc -print-multiarch)/linux-tts/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
  export PKG_CONFIG_PATH
fi
# shellcheck disable=SC2086
meson setup _ci $opts --werror -Dwarning_level=2
meson compile -C _ci
meson test -C _ci --print-errorlogs
