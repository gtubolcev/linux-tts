#!/usr/bin/env bash
# Builds a signed Ubuntu source package with our patches and uploads it to
# the Launchpad PPA, which builds the .debs.
#
#   packaging/upload-ubuntu-ppa.sh <source-package> [codename]
#
# The patches are packaging/<pkg>/patches/series (or the port in
# packaging/<pkg>/<codename>/patches); they are added to debian/patches.
# packaging/<pkg>/suffix holds the version suffix (e.g. +fix2): the version
# is the newest Ubuntu one of that release plus the suffix and ~ppa<N>~<codename>.
# packaging/<pkg>/prepare.sh, when present, runs in the unpacked source
# after the patches (e.g. to drop binary packages).
#
# The orig tarball is not uploaded, Launchpad takes it from the archive.
# PPA=... overrides the target (ppa:gtubolcev/linux-tts); SIGN_KEY=... the
# key; NO_UPLOAD=1 only builds the source package (unsigned) into _build/.
set -euo pipefail

PKG=${1:?usage: $0 <source-package> [codename]}
. /etc/os-release
DIST=${2:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
PPA_ARCHIVE=$(sed -E 's|^ppa:([^/]+)/(.+)$|~\1/+archive/ubuntu/\2|' <<< "$PPA")
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
WORK=$ROOT/_build/ubuntu/$PKG-$DIST
SUFFIX=$(cat "$HERE/$PKG/suffix")
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}

# the next upload number for versions $1<N>~$DIST of source package $2
next_upload() {
  curl -fsS --retry 5 --retry-all-errors "https://api.launchpad.net/devel/${PPA_ARCHIVE}?ws.op=getPublishedSources&source_name=$2&exact_match=true" \
    | python3 -c 'import json, sys
prefix = sys.argv[1]
suffix = "~" + sys.argv[2]
nums = [v[len(prefix):-len(suffix)] for v in (e["source_package_version"] for e in json.load(sys.stdin)["entries"])
        if v.startswith(prefix) and v.endswith(suffix)]
print(max((int(n) for n in nums if n.isdigit()), default=0) + 1)' "$1" "$DIST"
}

PATCHDIR=$HERE/$PKG/patches
[ -d "$HERE/$PKG/$DIST" ] && PATCHDIR=$HERE/$PKG/$DIST/patches

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

read -r UBUNTU_VERSION COMPONENT < <(curl -fsS --retry 5 --retry-all-errors "https://api.launchpad.net/devel/ubuntu/+archive/primary?ws.op=getPublishedSources&source_name=$PKG&exact_match=true&status=Published&distro_series=https://api.launchpad.net/devel/ubuntu/$DIST" \
  | python3 -c 'import json, sys; print("\n".join(e["source_package_version"] + " " + e["component_name"] for e in json.load(sys.stdin)["entries"]))' \
  | sort -V | tail -1) || true
[ -n "${UBUNTU_VERSION:-}" ] || { echo "error: no $PKG in Ubuntu $DIST" >&2; exit 1; }
# dget does not always fail on a missing file: a download counts once it unpacks
POOL=${PKG:0:1}
[[ $PKG == lib* ]] && POOL=${PKG:0:4}
for try in 1 2 3 4 5; do
  dget --download-only --allow-unauthenticated \
    "http://archive.ubuntu.com/ubuntu/pool/$COMPONENT/$POOL/$PKG/${PKG}_${UBUNTU_VERSION#*:}.dsc" \
    && dpkg-source -x ./*.dsc && break
  [ "$try" -lt 5 ] || exit 1
  rm -rf ./*
  sleep 60
done
SRCDIR=$(find . -mindepth 1 -maxdepth 1 -type d | head -1)
cd "$SRCDIR"

mapfile -t PATCHES < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$PATCHDIR/series")
for p in "${PATCHES[@]}"; do
  cp "$PATCHDIR/$p" "debian/patches/linux-tts-$p"
  echo "linux-tts-$p" >> debian/patches/series
  # the source package keeps them unapplied; check that they apply exactly
  patch -p1 -F0 --dry-run -s < "debian/patches/linux-tts-$p" >/dev/null 2>&1 \
    || { echo "error: $p does not apply" >&2; exit 1; }
done
[ -x "$HERE/$PKG/prepare.sh" ] && "$HERE/$PKG/prepare.sh"

VERSION="$(dpkg-parsechangelog -S Version)$SUFFIX~ppa"
VERSION=$VERSION$(next_upload "$VERSION" "$PKG")~$DIST
dch --newversion "$VERSION" --distribution "$DIST" --force-distribution \
  "Apply the patches of linux-tts (https://github.com/gtubolcev/linux-tts): ${PATCHES[*]}"
[ -f "$HERE/$PKG/changelog" ] && while read -r line; do dch -a "$line"; done < "$HERE/$PKG/changelog"

if [ -n "${NO_UPLOAD:-}" ]; then
  dpkg-buildpackage -S -d -nc -us -uc -sd
  exit 0
fi
dpkg-buildpackage -S -d -nc -sd ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
