#!/usr/bin/python3 -I
"""Measures the WAV files of one run.sh directory against its tests.json.

For every test: speech length (silence trimmed), median pitch, loudness of
the speech, and the text speechd sent to the module (after its punctuation
processing). Rate, pitch and volume are compared with the 0 setting."""
import glob, json, os, sys, wave
import numpy as np

d = sys.argv[1]
tests = json.load(open(os.path.join(d, 'tests.json')))


def when(path):
    """record.py names its files by the start time, the others are written
    when the message ends"""
    base = os.path.basename(path)
    if base.startswith('alsa-'):
        return int(base[5:-4]) / 1e9
    return os.path.getmtime(path)


wavs = sorted(glob.glob(os.path.join(d, '*.wav')), key=when)
logs = []
for f in glob.glob(os.path.join(d, '*.proto.log')):
    for line in open(f, errors='replace'):
        t, _, rest = line.partition(' ')
        logs.append((float(t), rest.rstrip('\n')))
logs.sort()


def load(path):
    with wave.open(path) as w:
        sr = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2')
    return x.astype(np.float32) / 32768, sr


def measure(x, sr):
    if len(x) < sr // 10:
        return None
    hop = sr // 100
    n = len(x) // hop
    frames = x[:n * hop].reshape(n, hop)
    e = np.sqrt((frames ** 2).mean(axis=1) + 1e-12)
    voiced = e > e.max() * 0.03
    idx = np.nonzero(voiced)[0]
    length = (idx[-1] - idx[0] + 1) * hop / sr if len(idx) else 0
    loud = 20 * np.log10(np.sqrt((frames[voiced] ** 2).mean()) + 1e-9)
    # pitch: autocorrelation of 40 ms windows of the loudest frames
    win = int(sr * 0.04)
    f0s = []
    lo, hi = int(sr / 600), int(sr / 60)
    for i in idx[::3]:
        s = x[i * hop:i * hop + win]
        if len(s) < win or e[i] < e.max() * 0.15:
            continue
        s = s - s.mean()
        ac = np.correlate(s, s, 'full')[win - 1:]
        if ac[0] <= 0:
            continue
        lag = lo + int(np.argmax(ac[lo:hi]))
        if ac[lag] / ac[0] > 0.5:
            f0s.append(sr / lag)
    f0 = float(np.median(f0s)) if len(f0s) > 5 else 0
    return length, f0, loud


base = {}
rows = []
for t in tests:
    files = [w for w in wavs if t['start'] - 0.2 <= when(w) <= t['end'] + 0.4]
    x = np.zeros(0, np.float32)
    sr = 22050
    for f in files:
        y, sr = load(f)
        x = np.concatenate([x, y])
    m = measure(x, sr) if len(x) else None
    sent = [r for tt, r in logs if t['start'] - 0.2 <= tt <= t['end'] + 0.2]
    speak = [r.split('SPEAK', 1)[1].strip(' |') for r in sent if r.startswith('>> SPEAK')]
    icons = [r for r in sent if r.startswith('<< 706') or r.startswith('>> SOUND_ICON')]
    rows.append((t, m, speak, icons, len(files)))
    kind, _, val = t['name'].partition(' ')
    if val in ('+0', 'none') and m:
        base[kind] = m

print('%-13s %5s %7s %6s %7s  %s' % ('test', 'files', 'len s', 'F0 Hz', 'dBFS', 'vs 0 / text sent'))
for t, m, speak, icons, nf in rows:
    kind, _, val = t['name'].partition(' ')
    if not m:
        print('%-13s %5d  NO AUDIO%s' % (t['name'], nf, '' if t['ok'] else ' (no END)'))
        continue
    length, f0, loud = m
    note = ''
    b = base.get(kind)
    if b and kind == 'rate':
        note = 'length x%.2f' % (length / b[0])
    elif b and kind == 'pitch' and b[1] and f0:
        note = 'F0 x%.2f' % (f0 / b[1])
    elif b and kind == 'volume':
        note = '%+.1f dB' % (loud - b[2])
    elif kind in ('punct', 'caps'):
        note = 'length %+.1f s' % (length - b[0]) if b else ''
        if speak:
            note += '  sent: ' + ' / '.join(speak)[:150]
        if icons:
            note += '  icons: ' + ', '.join(icons)
    print('%-13s %5d %7.2f %6.0f %7.1f  %s' % (t['name'], nf, length, f0, loud, note))
