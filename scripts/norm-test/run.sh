#!/bin/sh
# Speaks the normalization test sentences through one output module, in a
# private speech-dispatcher that can play nothing (see check-params/run.sh):
#   run.sh <module> <language>...    e.g. run.sh sherpa ru
# The module talks to proxy.py, which keeps the audio in WAV files; the
# private server has its own socket, configuration (a copy of the system and
# the user configuration) and runtime dir, and no reachable audio server.
# Results: $OUT/<module>-<lang>/ (default /home/grigory/Storage/Claude/norm-test).
# Then: collect.py <result dir> joins each message into <test>.16k.wav, and
# asr.py (in a container with vosk) transcribes them all.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); CP=$HERE/../check-params
M=$1; shift
OUT=${OUT:-/home/grigory/Storage/Claude/norm-test}
case $M in *-generic) BIN=sd_generic ;; *) BIN=sd_$M ;; esac

for L in "$@"; do
  W=$OUT/$M-$L
  rm -rf "$W"; mkdir -p "$W/conf" "$W/mods" "$W/run" "$W/xdg" "$W/log"
  cp -r /etc/speech-dispatcher/. "$W/conf/"
  cp -r "$HOME/.config/speech-dispatcher/." "$W/conf/"
  # this module only, under its own name
  grep -v '^ *\(AddModule\|DefaultModule\|AudioOutputMethod\)' "$HOME/.config/speech-dispatcher/speechd.conf" > "$W/conf/speechd.conf"
  cat >> "$W/conf/speechd.conf" <<X
AddModule "$M" "sd_proxy" "$M.conf"
DefaultModule "$M"
AudioOutputMethod "alsa"
AudioALSADevice "default"
X
  # generic modules play themselves: write the WAV instead.  $OUTPUT is
  # expanded unquoted in the module's shell line, so a ">" in it would not
  # redirect; use a small script.
  if [ "$BIN" = sd_generic ]; then
    printf '#!/bin/sh\nexec cat > %s/gen-$(date +%%s%%N).wav\n' "$W" > "$W/savewav"
    chmod +x "$W/savewav"
    sed -i "s|OUTPUT=\\\\'\$PLAY_COMMAND\\\\'|OUTPUT=\\\\'$W/savewav\\\\'|; s|OUTPUT=\\\\'aplay\\\\'|OUTPUT=\\\\'$W/savewav\\\\'|g" "$W/conf/modules/$M.conf"
    grep -q "savewav" "$W/conf/modules/$M.conf" || { echo "cannot redirect $M output" >&2; exit 1; }
  fi
  printf '#!/bin/sh\nexec python3 -I %s/proxy.py /usr/lib/speech-dispatcher-modules/%s "$@"\n' "$CP" "$BIN" > "$W/mods/sd_proxy"
  chmod +x "$W/mods/sd_proxy"
  # the private server and the modules play into record.py, nothing else
  cat > "$W/asound.conf" <<X
pcm.!default {
  type file
  slave { pcm { type null } }
  file "|exec python3 -I $CP/record.py $W"
  format "wav"
}
X

  env CHECK_OUT="$W" XDG_RUNTIME_DIR="$W/run" XDG_CONFIG_HOME="$W/xdg" \
      PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent \
      ALSA_CONFIG_PATH="$W/asound.conf" \
      speech-dispatcher -s -t 0 -l 4 -C "$W/conf" -m "$W/mods" \
        -S "$W/run/sock" -P "$W/run/pid" -L "$W/log" &
  S=$!
  i=0; while [ ! -S "$W/run/sock" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
  python3 -I "$HERE/client.py" "$W/run/sock" "$M" "$L" "$HERE/texts-$L.txt" "$W/tests.json" || echo "client failed" >&2
  kill $S; wait $S 2>/dev/null || true
done
