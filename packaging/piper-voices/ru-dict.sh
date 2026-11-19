#!/bin/sh
# ru-dict.sh DICTSOURCE STRESS-DIR DATA
# Rebuilds DATA/ru_dict (the espeak-ng data of the Piper voices) from the
# rhasspy espeak-ng dictsource the voices were trained with, plus the rules
# of STRESS-DIR (sherpa/espeak-ru in linux-tts) that honour the stress mark
# U+0301. Text without stress marks gives the same phonemes as before.
set -e
SRC=$(cd "$1" && pwd); STRESS=$(cd "$2" && pwd); DATA=$(cd "$3" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 -I "$STRESS/merge-stress-rules.py" "$SRC/ru_rules" "$STRESS/ru_rules_stress" "$T/ru_rules"
cp "$SRC/ru_list" "$SRC/ru_emoji" "$SRC/extra/ru_listx" "$T/"
mkdir "$T/root"
cp -r "$DATA" "$T/root/espeak-ng-data"
chmod -R u+w "$T/root"
rm -f "$T/root/espeak-ng-data/ru_dict"
(cd "$T" && espeak-ng --path="$T/root" --compile=ru >"$T/log" 2>&1) || { cat "$T/log" >&2; exit 1; }
# the check: an accent moves the stress
A=$(espeak-ng --path="$T/root" -v ru -q -x 'за́мок')
B=$(espeak-ng --path="$T/root" -v ru -q -x 'замо́к')
[ "$A" != "$B" ] || { echo "ru_dict ignores the stress mark" >&2; exit 1; }
cp "$T/root/espeak-ng-data/ru_dict" "$DATA/ru_dict"
