#!/bin/sh
# inside claude-audiocpp-build: hexgrad/Kokoro-82M v1.0 (Apache-2.0) .pth + voices -> GGUF q8_0
set -e
R=f3ff3571791e39611d31c381e3a41a3af07b4987
D=/w/dl/kokoro-mul-src; S=/w/km-src; O=/w/km-out
mkdir -p $D/voices $O; rm -rf $S; mkdir -p $S/voices
get () { [ -s "$D/$1" ] || python3 -I -c 'import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "https://huggingface.co/hexgrad/Kokoro-82M/resolve/$R/$1" "$D/$1"; }
get kokoro-v1_0.pth; get config.json
for v in $(grep -v '^#' /w/voices.list); do get voices/$v.pt; done
(cd $D && sha256sum kokoro-v1_0.pth config.json voices/*.pt) > $O/SHA256SUMS.src
cp $D/config.json $S/; ln -s $D/kokoro-v1_0.pth $S/kokoro-v1_0.pth
for v in $(grep -v '^#' /w/voices.list); do cp $D/voices/$v.pt $S/voices/; done
sed -e "s/torch.load(\([a-z]*\), map_location='cpu')/torch.load(\1, map_location='cpu', weights_only=True)/" \
    -e "s/    files.update(load_g2p_tables())/    pass/" \
    -e "s|Path(__file__).resolve().parents\[2\] / 'model_specs' / 'kokoro_tts.json'|Path('/w/src/model_specs/kokoro_tts.json')|" \
    /w/src/tools/models/prepare_kokoro_gguf.py > /w/prep_km.py
grep -c "w/src/model_specs" /w/prep_km.py
cd /w && /w/venv-kr/bin/python -I /w/prep_km.py --source $S --output-dir $O --type q8_0 --overwrite
ls -l $O; sha256sum $O/*.gguf
