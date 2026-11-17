#!/usr/bin/python3 -I
"""Receives the WAV stream of the ALSA file plugin (one per opened device)
and splits it at pauses into WAV files, one per spoken message:
    record.py <dir>"""
import os, select, struct, sys, time, wave

out = sys.argv[1]
fd = sys.stdin.buffer.raw
hdr = b''
while len(hdr) < 44:
    b = os.read(fd.fileno(), 44 - len(hdr))
    if not b:
        sys.exit(0)
    hdr += b
channels, rate = struct.unpack('<HI', hdr[22:28])
bits = struct.unpack('<H', hdr[34:36])[0]
pcm = bytearray()
first = 0


def flush():
    global pcm
    if pcm.strip(b'\0'):
        # named by the time the message began
        path = os.path.join(out, 'alsa-%d.wav' % first)
        w = wave.open(path, 'wb')
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        w.writeframes(bytes(pcm))
        w.close()
    pcm = bytearray()


while True:
    r, _, _ = select.select([fd], [], [], 0.4)
    if not r:
        flush()
        continue
    b = os.read(fd.fileno(), 65536)
    if not b:
        break
    if not pcm:
        first = time.time_ns()
    pcm += b
flush()
