"""Drives sd_silero through the speech-dispatcher module protocol (test only)."""
import subprocess, sys, time

def start(conf="test.conf"):
    p = subprocess.Popen(["./sd_silero", conf], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=open("proto.err", "a"), bufsize=0)
    return p

def send(p, s): p.stdin.write(s.encode())
def line(p): return p.stdout.readline().decode(errors="replace").rstrip()

def init(p):
    send(p, "INIT\n")
    while True:
        l = line(p); print("  ", l)
        if l[:3] in ("299", "399"): return l.startswith("299")

def audio(p):
    send(p, "AUDIO\naudio_output_method=server\n.\n")
    while not line(p).startswith("203"): pass

def voices(p):
    send(p, "LIST VOICES\n"); out = []
    while True:
        l = line(p)
        if l.startswith("200 "): return out
        out.append(l)

def speak(p, text, settings, stop_after=None):
    send(p, "SET\n" + "".join(f"{k}={v}\n" for k, v in settings.items()) + ".\n"); line(p)
    t0 = time.time()
    send(p, "SPEAK\n"); line(p); send(p, f"<speak>{text}</speak>\n.\n")
    first, tracks, samples, end = None, 0, 0, None
    while True:
        l = line(p)
        if l.startswith("705-num_samples"):
            tracks += 1; samples += int(l.split("=")[1])
            if first is None:
                first = time.time() - t0
                if stop_after is not None:
                    send(p, "STOP\n")
        if l[:3] in ("702", "703"):
            end = l; break
    print(f"   first audio {first if first is None else round(first, 2)} s, total {time.time()-t0:.2f} s, "
          f"{tracks} track(s), {samples/48000:.2f} s audio, {end}")

def quit(p):
    send(p, "QUIT\n"); p.wait(10)

if __name__ == "__main__":
    p = start(); print("init:", init(p)); audio(p)
    print("voices:", voices(p))
    speak(p, "Первое сообщение запускает сервер. Второе предложение, уже быстрее.", {"synthesis_voice": "silero-ru-aidar"})
    speak(p, "Второе сообщение, женский голос.", {"synthesis_voice": "NULL", "voice_type": "FEMALE2", "language": "ru"})
    speak(p, "Быстро и высоко.", {"rate": "60", "pitch": "50", "volume": "-20"})
    speak(p, "Длинный текст, который прервут. " * 6, {"rate": "0", "pitch": "0"}, stop_after=True)
    speak(p, "   ", {})
    speak(p, "После остановки всё ещё говорю.", {})
    quit(p)
    print("module 2 (adopts the server):")
    p = start(); init(p); audio(p)
    speak(p, "Новый модуль, старый сервер.", {"synthesis_voice": "silero-ru-eugene"})
    quit(p)
