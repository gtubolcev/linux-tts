#!/bin/sh
# compile-ru-dict.sh ESPEAK-NG SYSTEM-DATA SOURCE-DIR OUTPUT
# compiles ru_rules, ru_list and ru_emoji of SOURCE-DIR into OUTPUT
# against a copy of the system's espeak-ng-data (phoneme tables)
set -e
ESPEAK=$1; DATA=$2; SRC=$3; OUT=$4
OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
T=$OUT.tmp
rm -rf "$T"; mkdir -p "$T"
cp -r "$DATA" "$T/espeak-ng-data"
chmod -R u+w "$T"
rm -f "$T/espeak-ng-data/ru_dict"
(cd "$SRC" && "$ESPEAK" --path="$T" --compile=ru >/dev/null)
mv "$T/espeak-ng-data/ru_dict" "$OUT"
rm -rf "$T"
