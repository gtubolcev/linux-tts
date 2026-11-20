#!/bin/sh
# clean-install.sh OUT METAPACKAGE VOICE TEXT [DEBDIR]
#
# What a new user gets: in a fresh ubuntu container (as root), add the PPA,
# "apt install METAPACKAGE" with the recommends, then one sentence through
# sd_sherpa-onnx with VOICE to OUT/<METAPACKAGE>.wav (never played), its
# text after the normalizer to OUT/copy/, the installed packages and sizes
# to OUT/<METAPACKAGE>.pkgs. DEBDIR: local .debs that take precedence over
# the PPA (packages not uploaded yet).
set -eu
OUT=$1 META=$2 VOICE=$3 TEXT=$4 DEBDIR=${5:-}
HERE=$(cd "$(dirname "$0")" && pwd)
export DEBIAN_FRONTEND=noninteractive
mkdir -p "$OUT"
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates software-properties-common \
  gpg-agent python3 dpkg-dev >/dev/null
add-apt-repository -y -n ppa:gtubolcev/linux-tts >/dev/null
if [ -n "$DEBDIR" ]; then
  (cd "$DEBDIR" && dpkg-scanpackages -m . > Packages 2>/dev/null)
  echo "deb [trusted=yes] file:$DEBDIR ./" > /etc/apt/sources.list.d/local.list
  printf 'Package: *\nPin: origin ""\nPin-Priority: 1001\n' > /etc/apt/preferences.d/local
fi
apt-get update -qq
before=$(dpkg-query -W -f '${Package}\n' | sort)
apt-get install -y -qq "$META" > "$OUT/$META.apt" 2>&1 || { tail -20 "$OUT/$META.apt"; exit 1; }
dpkg-query -W -f '${Package} ${Version} ${Installed-Size}\n' | sort > "$OUT/all.pkgs"
echo "$before" | join -v1 "$OUT/all.pkgs" - > "$OUT/$META.pkgs"
echo "$META: $(wc -l < "$OUT/$META.pkgs") new packages, $(awk '{s+=$3} END {printf "%d MB", s/1024}' "$OUT/$META.pkgs")"
for p in speech-dispatcher speech-dispatcher-sherpa-onnx speech-dispatcher-ru-normalizr; do
  grep "^$p " "$OUT/$META.pkgs" || true
done
echo "DefaultModule: $(grep -E '^ *DefaultModule' /etc/speech-dispatcher/speechd.conf || echo unset)"
mkdir -p -m 700 /run/user/0
conf=$OUT/sherpa-onnx.conf
cp /etc/speech-dispatcher/modules/sherpa-onnx.conf "$conf"
echo "AudioCopyDir $OUT/copy" >> "$conf"
XDG_RUNTIME_DIR=/run/user/0 python3 -I "$HERE/modtest.py" \
  /usr/lib/speech-dispatcher-modules/sd_sherpa-onnx "$conf" "$OUT/$META" "$VOICE" "$TEXT"
cat "$OUT"/copy/*.txt 2>/dev/null; echo
