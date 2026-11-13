#!/bin/sh
# compiles teratts-test and sd_teratts in the sd-kokoro-build image
set -e
cd "$(dirname "$0")/.."
docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w/teratts sd-kokoro-build sh -c '
F="-O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -I../common $(pkg-config --cflags glib-2.0 json-glib-1.0)"
L="$(pkg-config --libs glib-2.0 json-glib-1.0 libonnxruntime) -lm"
C="teratts.c ../common/num-words.c ../common/ru-stress.c"
gcc $F $C teratts-test.c -o teratts-test $L
if [ -f sd_teratts.c ]; then
  gcc $F -I/usr/include/speech-dispatcher -I../kokoro/shim $C sd_teratts.c -o sd_teratts \
    -lspeechd_module $L -lpthread
fi'
