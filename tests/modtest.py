#!/usr/bin/python3 -I
"""modtest.py MODULE CONF OUT VOICE TEXT

One SPEAK through the speech-dispatcher module protocol to a real module:
INIT, LIST VOICES, the first voice whose name contains VOICE, the audio to
OUT.wav (never played), the module's stderr to OUT.err.  Exit 0 when the
module said 702 END and gave audio.

Copyright (C) 2026 Grigory Tuboltsev
SPDX-License-Identifier: MIT
"""
import select
import subprocess
import sys
import time
import wave


def unescape(b):
    out, i = bytearray(), 0
    while i < len(b):
        if b[i] == 0x7D and i + 1 < len(b):
            out.append(b[i + 1] ^ 0x20)
            i += 2
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


def main():
    mod, conf, out, want, text = sys.argv[1:6]
    with open(out + ".err", "w") as err:
        p = subprocess.Popen([mod, conf], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=err, bufsize=0)
    t0 = time.time()

    def send(s):
        p.stdin.write(s.encode())

    def line(timeout=120):
        if not select.select([p.stdout], [], [], timeout)[0]:
            raise TimeoutError
        return p.stdout.readline()

    send("INIT\n")
    while True:
        lin = line().decode(errors="replace")
        if not lin or lin[:3] == "399":
            print(f"{out}: INIT failed: {lin.strip()}")
            return 1
        if lin[:3] == "299":
            break
    send("LIST VOICES\n")
    names = []
    while True:
        lin = line().decode(errors="replace")
        if not lin or lin.startswith("200 "):
            break
        names.append(lin[4:].split("\t")[0])
    voice = next((n for n in names if want in n), None)
    print(f"{out}: {len(names)} voices, using {voice}")
    if voice is None:
        return 1
    send("AUDIO\naudio_output_method=server\n.\n")
    while not line().startswith(b"203"):
        pass
    send(f"SET\nsynthesis_voice={voice}\n.\n")
    line()
    send("SPEAK\n")
    line()
    send(f"{text}\n.\n")
    pcm, rate, ended = b"", 0, False
    try:
        while True:
            lin = line()
            if not lin:
                print(f"{out}: the module died")
                break
            if lin.startswith(b"705-AUDIO"):
                data = lin[len(b"705-AUDIO\0"):]
                while not data.endswith(b"\n705 AUDIO\n"):
                    data += line()
                pcm += unescape(data[:-len(b"\n705 AUDIO\n")])
            elif lin.startswith(b"705-") and b"sample_rate=" in lin:
                rate = int(lin.split(b"sample_rate=")[1].split()[0])
            elif lin.startswith((b"702", b"703", b"704")):
                ended = lin.startswith(b"702")
                break
    except TimeoutError:
        print(f"{out}: timeout")
    send("QUIT\n")
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    rate = rate or 22050
    if pcm:
        with wave.open(out + ".wav", "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm)
    print(f"{out}: {len(pcm) / 2 / rate:.2f} s audio, {time.time() - t0:.1f} s")
    return 0 if ended and pcm else 1


if __name__ == "__main__":
    sys.exit(main())
