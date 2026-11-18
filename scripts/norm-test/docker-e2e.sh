#!/bin/sh
# Inside ubuntu:resolute (as root): installs the built module .debs, the model
# .debs and speech-dispatcher from the linux-tts PPA, then speaks
# texts-ru.txt through every module twice, with and without normalization,
# into /out/norm/<module>-ru and /out/raw/<module>-ru (WAV files only; the
# container has no sound device).
#   docker run -v <debs>:/debs -v <out>:/out -v <this dir's parent>:/s ...
#     ubuntu:resolute sh /s/norm-test/docker-e2e.sh <module>...
set -eu
export DEBIAN_FRONTEND=noninteractive
echo 'APT::Sandbox::User "root";' > /etc/apt/apt.conf.d/00sandbox
echo "deb [trusted=yes] http://ppa.launchpadcontent.net/gtubolcev/linux-tts/ubuntu resolute main" > /etc/apt/sources.list.d/ppa.list
apt-get update -q >/dev/null 2>&1
apt-get install -y -q --no-install-recommends speech-dispatcher speech-dispatcher-audio-plugins \
  python3-speechd python3-numpy alsa-utils piper-voice-ru-ru-denis-medium /debs/*.deb >/tmp/apt.log 2>&1 \
  || { tail -30 /tmp/apt.log; exit 1; }
dpkg -l | awk '/speech-dispatcher-|sherpa-onnx|tts-model/ {print $2, $3}'
for M in "$@"; do
  for V in norm raw; do
    N=yes; [ $V = raw ] && N=no
    OUT=/out/$V NORM=$N sh /s/norm-test/run.sh "$M" ru
    python3 -I /s/norm-test/collect.py /out/$V/$M-ru | grep -v " ok$" || true
  done
done
