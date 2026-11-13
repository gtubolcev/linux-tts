#!/usr/bin/env bash
# Builds a signed source package of the committed HEAD and uploads it to the
# Launchpad PPA, which builds the .debs.
#
#   scripts/upload-ppa.sh [codename]
#
# The upstream version is the one in meson.build, plus +git<UTC date and time>.<commit>
# (the time keeps the versions in order; the commit hash alone does not)
# of the last commit outside debian/ and scripts/ when that is not the tag
# v<version>: the orig tarball is that commit without debian/, debian/ comes
# from HEAD, so a packaging change keeps the tarball. The package version
# adds -1~ppa<N>~<codename> (N counted per codename from the PPA: a version
# can be uploaded only once). PPA=... overrides the target (ppa:gtubolcev/linux-tts);
# SIGN_KEY=... the key; NO_UPLOAD=1 only builds the source package
# (unsigned) into _build/ppa/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
PPA_ARCHIVE=$(sed -E 's|^ppa:([^/]+)/(.+)$|~\1/+archive/ubuntu/\2|' <<< "$PPA")
SRC=$(sed -n 's/^Source: //p' "$ROOT/debian/control")
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}

git -C "$ROOT" diff --quiet HEAD || { echo "error: uncommitted changes" >&2; exit 1; }
CODE=$(git -C "$ROOT" log -1 --format=%H -- . ':(exclude)debian' ':(exclude)scripts')
UPSTREAM=$(git -C "$ROOT" show "$CODE:meson.build" | sed -n "s/^  version: '\(.*\)',$/\1/p")
if ! git -C "$ROOT" tag --points-at "$CODE" | grep -qx "v$UPSTREAM"; then
  UPSTREAM=$UPSTREAM+git$(TZ=UTC0 git -C "$ROOT" log -1 --date=format-local:%Y%m%d%H%M%S --format=%cd "$CODE").$(git -C "$ROOT" rev-parse --short=7 "$CODE")
fi

# the next upload number for versions $1<N>~$DIST
next_upload() {
  curl -fsS --retry 5 --retry-all-errors "https://api.launchpad.net/devel/${PPA_ARCHIVE}?ws.op=getPublishedSources&source_name=$SRC&exact_match=true" \
    | python3 -c 'import json, sys
prefix = sys.argv[1]
suffix = "~" + sys.argv[2]
nums = [v[len(prefix):-len(suffix)] for v in (e["source_package_version"] for e in json.load(sys.stdin)["entries"])
        if v.startswith(prefix) and v.endswith(suffix)]
print(max((int(n) for n in nums if n.isdigit()), default=0) + 1)' "$1" "$DIST"
}

WORK=$ROOT/_build/ppa/$DIST
rm -rf "$WORK"
mkdir -p "$WORK"
git -C "$ROOT" archive --format=tar --prefix="$SRC-$UPSTREAM/" "$CODE" -- . ':(exclude)debian' \
  | xz -9 > "$WORK/${SRC}_$UPSTREAM.orig.tar.xz"
tar -xf "$WORK/${SRC}_$UPSTREAM.orig.tar.xz" -C "$WORK"
git -C "$ROOT" archive --format=tar --prefix="$SRC-$UPSTREAM/" HEAD -- debian | tar -x -C "$WORK"
cd "$WORK/$SRC-$UPSTREAM"
VERSION=$UPSTREAM-1~ppa
VERSION=$VERSION$(next_upload "$VERSION")~$DIST
dch --newversion "$VERSION" --distribution "$DIST" --force-distribution \
  "Build of $(git -C "$ROOT" rev-parse --short=7 HEAD) for the PPA."
if [ -n "${NO_UPLOAD:-}" ]; then
  dpkg-buildpackage -S -d -us -uc -sa
  exit 0
fi
dpkg-buildpackage -S -d -sa ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
