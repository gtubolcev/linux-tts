#!/usr/bin/python3 -I
"""Transcribes <dir>/<engine>-ru/<test>.16k.wav (from collect.py) with a Vosk
model and writes {test: {text, engine: transcript}} as JSON.  Run it in a
container: the model is a download.
    asr.py <vosk model dir> <results dir> <out.json>"""
import glob, json, os, sys, wave
from vosk import Model, KaldiRecognizer, SetLogLevel

SetLogLevel(-1)
model = Model(sys.argv[1])
res = {}
for d in sorted(glob.glob(os.path.join(sys.argv[2], '*-ru'))):
    eng = os.path.basename(d)[:-3]
    for t in json.load(open(os.path.join(d, 'tests.json'))):
        if t['name'] == 'warmup':
            continue
        w = wave.open(os.path.join(d, t['name'] + '.16k.wav'))
        r = KaldiRecognizer(model, 16000)
        while True:
            b = w.readframes(8000)
            if not b:
                break
            r.AcceptWaveform(b)
        res.setdefault(t['name'], {'text': t['text']})[eng] = json.loads(r.FinalResult())['text']
    print(eng, file=sys.stderr)
json.dump(res, open(sys.argv[3], 'w'), ensure_ascii=False, indent=1)
