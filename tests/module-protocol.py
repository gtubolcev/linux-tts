#!/usr/bin/python3
"""Protocol tests of tts-module with the fake engine sd_dummy.

usage: module-protocol.py SD_DUMMY [TEST...]
       module-protocol.py --real SD_MODULE CONF_LINES [TEST...]

The second form runs the engine-independent tests against a real module
(CONF_LINES: its configuration, "\n" for line breaks).

Every test starts a module with its own configuration and talks to it like
speech-dispatcher does.  Fails with a message on the first broken
expectation of a test; runs all tests and exits 1 if any failed.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time

REAL = sys.argv[1] == "--real"
if REAL:
    EXE, REAL_CONF, WANT = sys.argv[2], sys.argv[3].replace("\\n", "\n") + "\n", sys.argv[4:]
else:
    EXE, REAL_CONF, WANT = sys.argv[1], "", sys.argv[2:]
LONG = " ".join("This is sentence number %d." % i for i in range(40))


class Fail(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Fail(what)


def unescape(b):
    out = bytearray()
    i = 0
    while i < len(b):
        if b[i] == 0x7D and i + 1 < len(b):
            out.append(b[i + 1] ^ 0x20)
            i += 2
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


class Mod:
    def __init__(self, conf=""):
        self.dir = tempfile.TemporaryDirectory()
        path = os.path.join(self.dir.name, "dummy.conf")
        with open(path, "w") as f:
            f.write("TextNormalization no\nDebug 1\n" + REAL_CONF + conf)
        self.err_path = os.path.join(self.dir.name, "stderr")
        self.p = subprocess.Popen([EXE, path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open(self.err_path, "wb"))
        self.lines = []      # (time, line); audio as "AUDIO n"
        self.cond = threading.Condition()
        self.pos = 0
        self.eof = None
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b""
        while True:
            d = os.read(self.p.stdout.fileno(), 65536)
            if not d:
                with self.cond:
                    self.eof = time.time()
                    self.cond.notify_all()
                return
            buf += d
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                if raw.startswith(b"705-AUDIO\0"):
                    data = unescape(raw[10:])
                    ok = len(data) % 2 == 0 and (REAL or data == b"\x0a\x7d" * (len(data) // 2))
                    line = "AUDIO %d" % (len(data) // 2) if ok else "BADAUDIO"
                elif raw.startswith(b"705"):
                    continue
                else:
                    line = raw.decode("utf-8", "replace")
                with self.cond:
                    self.lines.append((time.time(), line))
                    self.cond.notify_all()

    def send(self, s):
        self.p.stdin.write(s.encode())
        self.p.stdin.flush()

    def expect(self, prefix, timeout=5.0, since=None):
        """the next line starting with prefix after the cursor (or the line
        index since); moves the cursor"""
        end = time.time() + timeout
        with self.cond:
            while True:
                for i in range(self.pos if since is None else since, len(self.lines)):
                    if self.lines[i][1].startswith(prefix):
                        self.pos = i + 1
                        return self.lines[i]
                if self.eof is not None or time.time() >= end:
                    raise Fail("no %r; got %r" % (prefix, [l for _, l in self.lines[-12:]]))
                self.cond.wait(end - time.time())

    def cmd(self, s, prefix, timeout=5.0):
        self.send(s)
        return self.expect(prefix, timeout)

    def start(self):
        self.cmd("INIT\n", "299 ")
        self.cmd("AUDIO\naudio_output_method=server\n.\n", "203 OK AUDIO INIT")
        return self

    def set(self, **kw):
        self.send("SET\n")
        self.expect("203 OK RECEIVING")
        self.cmd("".join("%s=%s\n" % kv for kv in kw.items()) + ".\n", "203 OK SETTINGS RECEIVED")

    def speak(self, text, kind="SPEAK"):
        self.cmd(kind + "\n", "202 ")
        self.cmd(text + "\n.\n", "200 OK SPEAKING")

    def events(self, start=0):
        """the 70x events and audio after line index start"""
        with self.cond:
            return [l for _, l in self.lines[start:]
                    if l[:3] in ("700", "701", "702", "703", "704") and l != "700 INDEX MARK"
                    or l.startswith("AUDIO") or l == "BADAUDIO"]

    def stderr(self):
        with open(self.err_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def said(self):
        return [l[len("dummy: say "):] for l in self.stderr().splitlines()
                if l.startswith("dummy: say ")]

    def close(self):
        if self.p.poll() is None:
            try:
                self.p.stdin.close()
            except BrokenPipeError:
                pass
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                raise Fail("did not exit after EOF")
        check(self.p.returncode >= 0, "died of signal %d" % -self.p.returncode)
        check("BADAUDIO" not in [l for _, l in self.lines], "bad audio data")
        err = self.stderr()
        check("AddressSanitizer" not in err and "runtime error" not in err,
              "sanitizer: " + err[-3000:])


def terminal_events(evs):
    return [e for e in evs if e[:3] in ("701", "702", "703", "704")]


# ---------------------------------------------------------------- tests

def t_basic(m):
    m.send("LIST VOICES\n")
    m.expect("200 OK VOICE LIST")
    voices = [l for _, l in m.lines if l.startswith("200-")]
    check(len(voices) == 4, "voices: %r" % voices)
    m.set(synthesis_voice="dummy-en-male-1")
    m.speak("<speak>Hello world. Second sentence.</speak>")
    m.expect("702 END")
    evs = m.events()
    check(evs[0] == "701 BEGIN" and evs[-1] == "702 END", evs)
    samples = sum(int(e.split()[1]) for e in evs if e.startswith("AUDIO"))
    check(samples == 100 * (len("Hello world.") + len("Second sentence.")), "samples %d" % samples)
    check(m.said() == ["dummy-en-male-1|Hello world.", "dummy-en-male-1|Second sentence."], m.said())


def t_empty(m):
    m.speak("<speak> </speak>")
    m.expect("702 END")
    check(m.events() == ["701 BEGIN", "702 END"], m.events())


def t_marks(m):
    m.speak('<speak>One.<mark name="m1"/> Two, &amp; three.<mark name="m2"/></speak>')
    m.expect("702 END")
    check(m.events() == ["701 BEGIN", "AUDIO 400", "700-m1", "AUDIO 1300", "700-m2", "702 END"],
          m.events())
    check(m.said()[1].endswith("|Two, & three."), m.said())


def t_entities_utf8(m):
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("<speak>Привет, мир &lt;3</speak>")
    m.expect("702 END")
    check(m.said() == ["dummy-ru-female-1|Привет, мир <3"], m.said())


def t_stop(m):
    m.speak(LONG)
    m.expect("AUDIO")
    t0 = time.time()
    m.send("STOP\n")
    t, _ = m.expect("703 STOP")
    check(t - t0 < 0.3, "STOP took %.2f s" % (t - t0))
    time.sleep(0.4)
    evs = m.events()
    check(evs[-1] == "703 STOP", "after STOP: %r" % evs[-3:])
    check(len(m.said()) < 10, "kept speaking: %d pieces" % len(m.said()))


def t_stop_speak_one_write(m):
    """the bug of the synchronous modules: STOP and SPEAK in one read"""
    m.speak(LONG)
    m.expect("AUDIO")
    t0 = time.time()
    mark = len(m.lines)
    m.send("STOP\nSPEAK\n")
    m.expect("202 ")
    m.cmd("After the stop.\n.\n", "200 OK SPEAKING")
    t, _ = m.expect("703 STOP", since=mark)
    check(t - t0 < 0.3, "STOP took %.2f s" % (t - t0))
    m.expect("701 BEGIN")
    m.expect("702 END")
    time.sleep(0.4)
    check(terminal_events(m.events()) == ["701 BEGIN", "703 STOP", "701 BEGIN", "702 END"],
          terminal_events(m.events()))
    said = m.said()
    check(said[-1].endswith("|After the stop."), said[-3:])
    check(len(said) < 10, "the first message went on: %d pieces" % len(said))


def t_stop_during_load(m):
    m.speak("Hello.")
    m.expect("701 BEGIN")
    time.sleep(0.2)
    t0 = time.time()
    m.send("STOP\n")
    t, _ = m.expect("703 STOP")
    check(t - t0 < 0.3, "STOP during load took %.2f s" % (t - t0))
    m.speak("Again.")
    m.expect("702 END", 5)
    check(m.said() == ["dummy-en-female-1|Again."], m.said())


def t_failed_model(m):
    m.set(synthesis_voice="dummy-de-1")
    m.speak("Hallo.")
    m.expect("702 END")
    m.cmd("SPEAK\n", "202 ")
    m.cmd("Noch einmal.\n.\n", "301 ")
    m.set(synthesis_voice="dummy-en-female-1")
    m.speak("Hello.")
    m.expect("702 END")
    check(m.said() == ["dummy-en-female-1|Hello."], m.said())


def t_synth_failure(m):
    m.speak("One. FAIL here. Three.")
    m.expect("702 END")
    check(len(m.said()) == 3, m.said())
    check("synthesis failed" in m.stderr(), "no error logged")


def t_pause(m):
    text = "<speak>" + "".join('Sentence %d.<mark name="m%d"/> ' % (i, i) for i in range(30)) + "</speak>"
    m.speak(text)
    m.expect("700-m1")
    t0 = time.time()
    m.send("PAUSE\n")
    t, _ = m.expect("704 PAUSE")
    check(t - t0 < 0.3, "PAUSE took %.2f s" % (t - t0))
    time.sleep(0.3)
    evs = m.events()
    check(evs[-1] == "704 PAUSE", evs[-3:])
    marks = [e for e in evs if e.startswith("700-")]
    check(marks == ["700-m%d" % i for i in range(len(marks))], marks)
    # a STOP after the pause changes nothing
    m.send("STOP\n")
    time.sleep(0.2)
    check(m.events()[-1] == "704 PAUSE", m.events()[-2:])


def t_char_key(m):
    m.speak("a", "CHAR")
    m.expect("702 END")
    m.speak("shift_a", "KEY")
    m.expect("702 END")
    m.speak("space", "CHAR")
    m.expect("702 END")
    check(m.said()[:2] == ["dummy-en-female-1|a", "dummy-en-female-1|shift a"], m.said())


def t_settings_snapshot(m):
    m.set(synthesis_voice="dummy-en-male-1")
    m.speak(LONG)
    m.expect("AUDIO")
    m.set(synthesis_voice="dummy-ru-female-1")
    time.sleep(0.3)
    m.send("STOP\n")
    m.expect("703 STOP")
    check(all(s.startswith("dummy-en-male-1|") for s in m.said()), m.said())
    m.speak("Привет.")
    m.expect("702 END")
    check(m.said()[-1] == "dummy-ru-female-1|Привет.", m.said()[-1])


def t_voice_choice(m):
    m.set(language="ru")
    m.speak("Да.")
    m.expect("702 END")
    m.set(language="fr", voice_type="male1", synthesis_voice="NULL")
    m.speak("Oui.")
    m.expect("702 END")
    m.set(language="en", voice_type="female2")
    m.speak("Yes.")
    m.expect("702 END")
    check([s.split("|")[0] for s in m.said()] == ["dummy-ru-female-1", "dummy-en-male-1",
                                                  "dummy-en-female-1"], m.said())


def t_one_model(m):
    m.speak("Hello.")
    m.expect("702 END")
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("Привет.")
    m.expect("702 END")
    err = m.stderr()
    check("unloaded en (0)" in err and "loaded ru (1)" in err, err)


def t_idle_unload(m):
    m.speak("Hello.")
    m.expect("702 END")
    time.sleep(1.6)
    check("unloaded en (0)" in m.stderr(), "not unloaded")
    m.speak("Hello again.")
    m.expect("702 END")
    check(m.stderr().count("loaded en (1)") == 2, m.stderr())


def t_flood(m):
    n = 40
    m.send("".join("SPEAK\nMessage %d. With two sentences.\n.\nSTOP\n" % i for i in range(n)))
    for i in range(n):
        m.expect("200 OK SPEAKING")
    m.speak("The last one.")
    end = time.time() + 10
    while time.time() < end:
        t = terminal_events(m.events())
        if len(t) >= 2 * (n + 1):
            break
        time.sleep(0.05)
    t = terminal_events(m.events())
    check(len(t) == 2 * (n + 1), "events: %d" % len(t))
    check(all(t[i] == "701 BEGIN" for i in range(0, len(t), 2)), t)
    check(all(t[i] in ("702 END", "703 STOP") for i in range(1, len(t) - 1, 2)), t)
    check(t[-1] == "702 END", t[-2:])
    check(m.said()[-1].endswith("|The last one."), m.said()[-1])


def t_set_flood_during_speech(m):
    m.speak(LONG)
    m.expect("AUDIO")
    for i in range(50):
        m.set(rate=str(i * 4 - 100), volume=str(i), pitch="7", punctuation_mode="all")
    m.send("STOP\n")
    m.expect("703 STOP")
    m.speak("Done.")
    m.expect("702 END")


def t_quiet_volume(m):
    m.set(volume="-100")
    m.speak("Hush.")
    m.expect("702 END")
    check(any(l == "BADAUDIO" for _, l in m.lines), "volume -100 did not change the samples")
    m.lines = [x for x in m.lines if x[1] != "BADAUDIO"]


def t_quit(m):
    t0 = time.time()
    m.cmd("QUIT\n", "210 OK QUIT")
    m.p.wait(2)
    check(time.time() - t0 < 1, "QUIT took %.2f s" % (time.time() - t0))


def t_quit_during_load(m):
    m.speak("Hello.")
    m.expect("701 BEGIN")
    time.sleep(0.2)
    t0 = time.time()
    m.cmd("QUIT\n", "210 OK QUIT", 3)
    m.p.wait(3)
    check(time.time() - t0 < 2.5, "QUIT took %.2f s" % (time.time() - t0))


def t_eof_during_speech(m):
    m.speak(LONG)
    m.expect("AUDIO", 60 if REAL else 5)
    t0 = time.time()
    m.p.stdin.close()
    m.p.wait(3)
    check(time.time() - t0 < 2.5, "EOF took %.2f s" % (time.time() - t0))


def t_preload(m):
    time.sleep(0.6)
    check("loaded ru (1)" in m.stderr(), "no preload: " + m.stderr())


TESTS = [
    (t_basic, ""),
    (t_empty, ""),
    (t_marks, ""),
    (t_entities_utf8, ""),
    (t_stop, "DummySynthMs 100\n"),
    (t_stop_speak_one_write, "DummySynthMs 100\n"),
    (t_stop_during_load, "DummyLoadMs 1500\n"),
    (t_failed_model, ""),
    (t_synth_failure, ""),
    (t_pause, "DummySynthMs 50\n"),
    (t_char_key, ""),
    (t_settings_snapshot, "DummySynthMs 50\n"),
    (t_voice_choice, ""),
    (t_one_model, ""),
    (t_idle_unload, "DummyIdleTimeout 1\n"),
    (t_flood, "DummySynthMs 20\n"),
    (t_set_flood_during_speech, "DummySynthMs 50\n"),
    (t_quiet_volume, ""),
    (t_quit, ""),
    (t_quit_during_load, "DummyLoadMs 5000\n"),
    (t_eof_during_speech, "DummySynthMs 100\n"),
    (t_preload, 'DummyPreload yes\nDummyVoice "ru" "dummy-ru-female-1"\n'),
]


def r_speak(m):
    m.speak("<speak>Привет. Это проверка модуля.<mark name=\"m1\"/> Третье предложение.</speak>")
    m.expect("702 END", 60)
    evs = m.events()
    check(evs[0] == "701 BEGIN" and evs[-1] == "702 END" and "700-m1" in evs, evs)
    check(sum(int(e.split()[1]) for e in evs if e.startswith("AUDIO")) > 16000, "too little audio")


def r_stop_during_load(m):
    m.speak("Привет.")
    m.expect("701 BEGIN")
    time.sleep(0.5)
    if any(l.startswith("702 END") for _, l in m.lines):
        return   # loads faster than that (Piper)
    t0 = time.time()
    m.send("STOP\n")
    t, _ = m.expect("703 STOP")
    check(t - t0 < 0.3, "STOP during load took %.2f s" % (t - t0))
    m.speak("Ещё раз.")
    m.expect("702 END", 60)


def r_stop_speak(m):
    m.speak("Загрузка.")
    m.expect("702 END", 60)
    m.speak(" ".join("Это предложение номер %d." % i for i in range(30)))
    m.expect("AUDIO", 30)
    t0 = time.time()
    mark = len(m.lines)
    m.send("STOP\nSPEAK\n")
    m.expect("202 ")
    m.cmd("После остановки.\n.\n", "200 OK SPEAKING")
    t, _ = m.expect("703 STOP", since=mark)
    check(t - t0 < 0.3, "STOP took %.2f s" % (t - t0))
    m.expect("701 BEGIN")
    m.expect("702 END", 30)


def r_quit_during_load(m):
    m.speak("Привет.")
    m.expect("701 BEGIN")
    time.sleep(0.5)
    t0 = time.time()
    m.cmd("QUIT\n", "210 OK QUIT", 4)
    m.p.wait(4)
    check(time.time() - t0 < 3, "QUIT took %.2f s" % (time.time() - t0))


REAL_TESTS = [(r_speak, ""), (r_stop_during_load, ""), (r_stop_speak, ""),
              (r_quit_during_load, ""), (t_quit, ""), (t_eof_during_speech, "")]


def main():
    want = WANT
    failed = 0
    for fn, conf in (REAL_TESTS if REAL else TESTS):
        name = fn.__name__[2:]
        if want and name not in want:
            continue
        m = Mod(conf)
        t0 = time.time()
        try:
            m.start()
            fn(m)
            m.close()
            print("ok   %-28s %.1f s" % (name, time.time() - t0))
        except Exception as e:
            failed += 1
            print("FAIL %-28s %s: %s" % (name, type(e).__name__, e))
            print("     stderr: " + m.stderr()[-1500:].replace("\n", "\n     "))
            if m.p.poll() is None:
                m.p.kill()
    sys.exit(1 if failed else 0)


main()
