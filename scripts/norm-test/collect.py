#!/usr/bin/python3 -I
"""Joins the WAV files of each test message into <dir>/<name>.16k.wav
(16 kHz mono, for speech recognition).
    collect.py <dir>"""
import glob, json, os, sys, wave
import numpy as np

d = sys.argv[1]
tests = json.load(open(os.path.join(d, 'tests.json')))


def when(path):
    base = os.path.basename(path)
    if base.startswith('alsa-'):
        return int(base[5:-4]) / 1e9      # start of the message
    return os.path.getmtime(path)         # written as the message ends


def load(path):
    with wave.open(path) as w:
        sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype='<i2').astype(np.float32)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    if sr != 16000 and len(x):
        t = np.arange(0, len(x) * 16000 // sr) * sr / 16000
        x = np.interp(t, np.arange(len(x)), x)
    return x


wavs = [p for p in glob.glob(os.path.join(d, '*.wav')) if '.16k.' not in p]
for t in tests:
    parts = sorted((p for p in wavs if t['start'] <= when(p) <= t['end'] + 1.0), key=when)
    x = np.concatenate([load(p) for p in parts]) if parts else np.zeros(0, np.float32)
    with wave.open(os.path.join(d, t['name'] + '.16k.wav'), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
        w.writeframes(np.clip(x, -32768, 32767).astype('<i2').tobytes())
    print('%-14s %5.1f s  %d part(s)  %s' % (t['name'], len(x) / 16000, len(parts), 'ok' if t['ok'] else 'NO END'))
