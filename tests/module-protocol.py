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
    def __init__(self, conf="", cache=None, run_files=None):
        self.dir = tempfile.TemporaryDirectory()
        path = os.path.join(self.dir.name, "dummy.conf")
        with open(path, "w") as f:
            f.write("TextNormalization no\nDebug 1\n" + REAL_CONF
                    + conf.replace("@DIR@", self.dir.name))
        self.err_path = os.path.join(self.dir.name, "stderr")
        # own runtime dir: .load / .state of the test, not of the session
        os.mkdir(os.path.join(self.dir.name, "run"), 0o700)
        if run_files:
            os.mkdir(os.path.join(self.dir.name, "run", "linux-tts"), 0o700)
            for name, text in run_files.items():
                with open(os.path.join(self.dir.name, "run", "linux-tts", name), "w") as f:
                    f.write(text)
        env = dict(os.environ, XDG_CACHE_HOME=cache or os.path.join(self.dir.name, "cache"),
                   XDG_RUNTIME_DIR=os.path.join(self.dir.name, "run"))
        self.p = subprocess.Popen([EXE, path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open(self.err_path, "wb"), env=env)
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
                    # the dummy's samples are all 0x7d0a (a newline and the
                    # escape byte); volume and rate change them
                    same = REAL or data == b"\x0a\x7d" * (len(data) // 2)
                    line = ("BADAUDIO" if len(data) % 2 else
                            "AUDIO %d%s" % (len(data) // 2, "" if same else " changed"))
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


def t_long_piece(m):
    """a text without sentence marks reaches the engine in pieces of at most
    400 characters, cut at commas or spaces, nothing lost"""
    words = " ".join("w%d" % i for i in range(1500))
    text = "Start, " + words.replace("w700 ", "w700, ") + " end"
    m.speak("<speak>%s</speak>" % text)
    m.expect("702 END", 20)
    said = [l.split("|", 1)[1] for l in m.said()]
    check(len(said) > 10 and max(len(x) for x in said) <= 400, [len(x) for x in said])
    check(" ".join(said) == text, said[:3])
    check(any(x.endswith("w700,") for x in said), [x[-8:] for x in said])


def t_bad_utf8(m):
    """invalid UTF-8 is split into sentences as well, not given as one piece"""
    body = b"<speak>One \xff two. Three \xfe four. " + b"x " * 300 + b"</speak>"
    m.send("SPEAK\n")
    m.expect("202 ")
    m.p.stdin.write(body + b"\n.\n")
    m.p.stdin.flush()
    m.expect("702 END", 10)
    said = [l.split("|", 1)[1] for l in m.said()]
    check(len(said) >= 4 and said[0].startswith("One") and said[0].endswith("two.")
          and max(len(x) for x in said) <= 400, said)


def t_bad_mark(m):
    """a mark name with a newline would fake "702 END"; it is dropped"""
    m.speak('<speak>One.<mark name="a\n702 END"/> Two.<mark name="ok"/></speak>')
    m.expect("702 END")
    evs = m.events()
    check("700-ok" in evs and not any(e.startswith("700-a") for e in evs), evs)
    check(evs.count("702 END") == 1, evs)


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


def write_models(m, **models):
    for name, text in models.items():
        with open(os.path.join(m.dir.name, name), "w") as f:
            f.write(text)


def children(pid):
    out = []
    for t in os.listdir("/proc/%d/task" % pid):
        with open("/proc/%d/task/%s/children" % (pid, t)) as f:
            out += f.read().split()
    return out


def probe_cache(m):
    try:
        with open(os.path.join(m.dir.name, "cache", "linux-tts", "probe-dummy.ini")) as f:
            return f.read()
    except OSError:
        return ""


def t_probe_crash(m):
    """a model that crashes the engine fails in the test load, not in the module"""
    write_models(m, en="ok", ru="crash")
    m.speak("Hello.")
    m.expect("702 END")
    check("result=ok" in probe_cache(m), "no ok in the cache: " + probe_cache(m))
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("Привет.")
    m.expect("702 END")
    check("crashed the engine (signal 6" in m.stderr(), m.stderr()[-800:])
    check("crashed the engine" in probe_cache(m), probe_cache(m))
    check("loaded ru" not in m.stderr(), "ru loaded in the module")
    m.set(synthesis_voice="dummy-en-female-1")
    m.speak("Again.")
    m.expect("702 END")
    check(m.said() == ["dummy-en-female-1|Hello.", "dummy-en-female-1|Again."], m.said())
    check(m.stderr().count("test load of dummy-en-female-1 (") == 1, "en tested twice")


def t_probe_exit(m):
    """an engine that exits itself in the test load (sherpa-onnx _Exit(-1))
    is remembered like a crash; the module does not load the model"""
    write_models(m, en="ok", ru="exit")
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("Привет.")
    m.expect("702 END")
    check("made the engine exit (code 255)" in m.stderr(), m.stderr()[-800:])
    check("made the engine exit" in probe_cache(m), probe_cache(m))
    m.set(synthesis_voice="dummy-en-female-1")
    m.speak("Hello.")
    m.expect("702 END")
    check(m.said() == ["dummy-en-female-1|Hello."], m.said())


def t_probe_killed(m):
    """a test load killed from outside (OOM killer) fails, but is not
    remembered as a broken model"""
    write_models(m, en="ok", ru="kill")
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("Привет.")
    m.expect("702 END")
    check("was killed (signal 9" in m.stderr(), m.stderr()[-800:])
    check(os.path.join(m.dir.name, "ru") + "]" not in probe_cache(m), probe_cache(m))
    check("loaded ru" not in m.stderr(), "ru loaded in the module")


def t_probe_cached(m):
    """a remembered result is used without a test load; changed files are tested again"""
    write_models(m, en="ok")
    os.makedirs(os.path.join(m.dir.name, "cache", "linux-tts"), exist_ok=True)
    with open(os.path.join(m.dir.name, "cache", "linux-tts", "probe-dummy.ini"), "w") as f:
        f.write("[%s]\nstamp=nothing\nresult=ok\n" % os.path.join(m.dir.name, "en"))
    m.speak("Hello.")
    m.expect("702 END")
    check("test load of dummy-en-female-1 (" in m.stderr(), "old stamp not tested")
    m.set(synthesis_voice="dummy-ru-female-1")
    write_models(m, ru="ok")
    m.speak("Привет.")
    m.expect("702 END")
    m.set(synthesis_voice="dummy-en-female-1")
    m.speak("Again.")     # en was unloaded for ru (one model): loads again, from the cache
    m.expect("702 END")
    check(m.stderr().count("test load of dummy-en-female-1 (") == 1, m.stderr()[-800:])


def t_probe_subdir(m):
    """a change deeper in a model's directory (espeak-ng-data/...) tests it again"""
    write_models(m, en="ok")
    os.makedirs(os.path.join(m.dir.name, "ru", "data", "voices"))
    write_models(m, **{"ru/data/voices/x": "1"})
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("Привет.")
    m.expect("702 END")
    m.set(synthesis_voice="dummy-en-female-1")
    m.speak("Hello.")     # unloads ru (one model)
    m.expect("702 END")
    write_models(m, **{"ru/data/voices/x": "22"})
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("Ещё.")
    m.expect("702 END")
    check(m.stderr().count("test load of dummy-ru-female-1 (") == 2, m.stderr()[-800:])


def t_probe_engine(m):
    """the verdict is kept for one engine: the same engine takes it from the
    cache, another salt (provider, plugin) tests again"""
    write_models(m, en="ok")
    cache = os.path.join(m.dir.name, "cache")
    m.speak("Hello.")
    m.expect("702 END")
    m.close()
    counts = []
    for salt in ("a", "a", "b"):
        n = Mod("DummyModelDir %s\nDummySalt %s\n" % (m.dir.name, salt), cache=cache)
        try:
            n.start()
            n.speak("Hello.")
            n.expect("702 END")
            counts.append(n.stderr().count("test load of dummy-en-female-1 ("))
        finally:
            n.close()
    check(counts == [1, 0, 1], counts)


def t_probe_hang(m):
    """a test load that hangs is killed after ProbeTimeout; the module goes on"""
    write_models(m, en="ok", ru="hang")
    m.set(synthesis_voice="dummy-ru-female-1")
    t0 = time.time()
    m.speak("Привет.")
    m.expect("702 END", 5)
    check(time.time() - t0 < 3, "took %.1f s" % (time.time() - t0))
    check("did not finish in 1 s" in m.stderr(), m.stderr()[-800:])
    check(children(m.p.pid) == [], "child left: %s" % children(m.p.pid))
    check(os.path.join(m.dir.name, "ru") + "]" not in probe_cache(m), "timeout remembered")
    m.set(synthesis_voice="dummy-en-female-1")
    m.speak("Hello.")
    m.expect("702 END", 5)
    check(m.said() == ["dummy-en-female-1|Hello."], m.said())


def t_probe_quit(m):
    """QUIT during a test load kills the child"""
    write_models(m, en="hang")
    m.speak("Hello.")
    m.expect("701 BEGIN")
    time.sleep(0.5)
    kids = children(m.p.pid)
    check(len(kids) == 1, "children %s" % kids)
    m.cmd("QUIT\n", "210 OK QUIT", 4)
    m.p.wait(4)
    time.sleep(0.2)
    check(not os.path.exists("/proc/" + kids[0]) or "Z" in open("/proc/%s/stat" % kids[0]).read().split()[2],
          "child %s still runs" % kids[0])


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
    m.speak("control_alt_kp-enter", "KEY")
    m.expect("702 END")
    m.speak("f12", "KEY")
    m.expect("702 END")
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("щ", "CHAR")
    m.expect("702 END")
    m.speak(",", "CHAR")
    m.expect("702 END")
    m.speak("shift_b", "KEY")
    m.expect("702 END")
    m.speak("Ё", "CHAR")
    m.expect("702 END")
    check(m.said() == ["dummy-en-female-1|ay", "dummy-en-female-1|shift ay",
                       "dummy-en-female-1|space",
                       "dummy-en-female-1|control alt keypad enter",
                       "dummy-en-female-1|F12",
                       "dummy-ru-female-1|ща", "dummy-ru-female-1|запятая",
                       "dummy-ru-female-1|шифт би", "dummy-ru-female-1|ё"], m.said())


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
    check(any(l.endswith(" changed") for _, l in m.lines), "volume -100 did not change the samples")


def t_rate(m):
    # rate 100: 4 times, the model 2 of it, the frame (sonic) the rest
    def samples(rate):
        m.set(rate=str(rate))
        start = len(m.lines)
        m.speak("Hello world.")
        m.expect("702 END")
        return sum(int(e.split()[1]) for e in m.events(start) if e.startswith("AUDIO"))
    full = samples(0)
    check(full == 1200, "rate 0: %d" % full)
    check(samples(-100) == 2400, "rate -100")
    check(samples(50) == 600, "rate 50 (2 times, all by the model)")
    fast = samples(100)
    sonic = "dummy: sonic" in m.stderr()
    speeds = [l for l in m.stderr().splitlines() if l.startswith("dummy: speed")]
    check(speeds == ["dummy: speed 0.50", "dummy: speed 2.00",
                     "dummy: speed %s" % ("2.00" if sonic else "4.00")], speeds)
    if not sonic:
        # the model does it all
        check(fast == 300, "rate 100 without sonic: %d" % fast)
    else:
        check(250 <= fast <= 350, "rate 100: %d samples, not ~300" % fast)
    m.set(rate="0")


def t_loud_volume(m):
    m.set(volume="100")
    m.speak("Loud.")
    m.expect("702 END")
    # the full-scale samples are compressed, not wrapped around
    check(any(l.endswith(" changed") for _, l in m.lines), "volume 100 did not change the samples")


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
    # wait, not a fixed sleep: on a loaded builder the load takes longer
    check(wait_stderr(m, "loaded ru (1)", 10), "no preload: " + m.stderr())


def wait_stderr(m, text, secs):
    end = time.time() + secs
    while time.time() < end:
        if text in m.stderr():
            return True
        time.sleep(0.05)
    return False


def write_load(m, voice):
    path = os.path.join(m.dir.name, "run", "linux-tts", "dummy.load")
    with open(path + ".new", "w") as f:
        f.write(voice + "\n")
    os.replace(path + ".new", path)


def t_load_request_during_wait(m):
    """a .load request while a message waits for its model does not take
    the message's place: the message is spoken (it hung until STOP)"""
    write_load(m, "dummy-en-female-1")
    check(wait_stderr(m, "load request: dummy-en-female-1", 3), "no load request")
    m.set(synthesis_voice="dummy-ru-female-1")
    m.speak("Привет.")
    m.expect("701 BEGIN")
    write_load(m, "dummy-en-male-1")
    check(wait_stderr(m, "load request: dummy-en-male-1", 3), "no second load request")
    m.expect("702 END", 10)
    check(m.said() == ["dummy-ru-female-1|Привет."], m.said())


def read_state(m):
    try:
        with open(os.path.join(m.dir.name, "run", "linux-tts", "dummy.state")) as f:
            return f.read().strip()
    except OSError:
        return None


def wait_state(m, want, secs):
    """the state file says want (a prefix); every state seen on the way"""
    seen = []
    end = time.time() + secs
    while time.time() < end:
        st = read_state(m)
        if not seen or seen[-1] != st:
            seen.append(st)
        if st is not None and st.startswith(want):
            return seen
        time.sleep(0.02)
    raise Fail("state never %r: %r" % (want, seen))


def t_load_state(m):
    """the dialog's .load request: loading, ready; a voice that fails;
    an unknown name loads the default; the file removed unloads"""
    check(read_state(m) == "idle", "start: %r" % read_state(m))
    write_load(m, "dummy-en-male-1")
    seen = wait_state(m, "ready", 5)
    check("loading" in seen, "no loading on the way: %r" % seen)
    check("loaded en (1)" in m.stderr(), m.stderr()[-600:])
    write_load(m, "dummy-de-1")
    wait_state(m, "failed", 5)
    check(read_state(m) == "failed bad model", read_state(m))
    write_load(m, "no-such-voice")
    wait_state(m, "ready", 5)
    check("no voice no-such-voice, loading the default one" in m.stderr(), m.stderr()[-600:])
    # nothing of this was spoken
    check(m.said() == [], m.said())
    os.unlink(os.path.join(m.dir.name, "run", "linux-tts", "dummy.load"))
    wait_state(m, "idle", 5)
    check(wait_stderr(m, "unloaded en", 3), "not unloaded: " + m.stderr()[-600:])
    # and speech still works after all that
    m.speak("Hello.")
    m.expect("702 END")


def t_load_stale(m):
    """a .load left from before the module started is old news"""
    time.sleep(2.5)    # the main thread looks every 2 s
    check("load request" not in m.stderr() and "loaded" not in m.stderr(),
          "stale request taken: " + m.stderr()[-600:])
    check(read_state(m) == "idle", read_state(m))


def t_load_idle(m):
    """a loaded voice goes after IdleTimeout even while .load stays"""
    write_load(m, "dummy-en-female-1")
    wait_state(m, "ready", 5)
    wait_state(m, "idle", 4)
    check("unloaded en (0)" in m.stderr(), m.stderr()[-600:])


def t_load_garbage(m):
    """a .load file of binary junk, huge, or empty: no crash, the default
    voice ("1") for empty"""
    path = os.path.join(m.dir.name, "run", "linux-tts", "dummy.load")
    for data in (b"\xff\xfe\x00\x01\n", b"x" * (1 << 20), b""):
        with open(path + ".new", "wb") as f:
            f.write(data)
        os.replace(path + ".new", path)
        time.sleep(2.3)
    wait_state(m, "ready", 5)
    m.speak("Hello.")
    m.expect("702 END")


def t_config_reload(m):
    """the settings dialog changes the file without restarting the server"""
    path = os.path.join(m.dir.name, "dummy.conf")
    base = open(path).read()

    def write(extra):
        with open(path + ".new", "w") as f:
            f.write(base + extra)
        os.replace(path + ".new", path)     # as g_file_set_contents

    m.set(language="en", voice_type="female1")
    m.speak("One.")
    m.expect("702 END")
    copies = os.path.join(m.dir.name, "copies")
    os.mkdir(copies)
    write('DummyVoice "en" "dummy-en-male-1"\nAudioCopyDir %s\n' % copies)
    m.speak("Two.")
    m.expect("702 END")
    check(os.path.exists(os.path.join(copies, "output1.wav")), "no copy after the change")
    write("")
    m.speak("Three.")
    m.expect("702 END")
    check([s.split("|")[0] for s in m.said()] == ["dummy-en-female-1", "dummy-en-male-1",
                                                  "dummy-en-female-1"], m.said())
    check(not os.path.exists(os.path.join(copies, "output2.wav")), "copy still on")
    # without a message: the main thread looks every 2 s
    check("loaded ru" not in m.stderr(), "ru loaded early")
    write("DummyPreload dummy-ru-female-1\n")
    end = time.time() + 4
    while "loaded ru (" not in m.stderr() and time.time() < end:
        time.sleep(0.1)
    check("loaded ru (" in m.stderr(), "Preload not taken: " + m.stderr()[-800:])


TESTS = [
    (t_basic, ""),
    (t_empty, ""),
    (t_marks, ""),
    (t_bad_mark, ""),
    (t_long_piece, ""),
    (t_bad_utf8, ""),
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
    (t_rate, ""),
    (t_loud_volume, ""),
    (t_quit, ""),
    (t_quit_during_load, "DummyLoadMs 5000\n"),
    (t_eof_during_speech, "DummySynthMs 100\n"),
    (t_preload, 'DummyPreload yes\nDummyVoice "ru" "dummy-ru-female-1"\n'),
    (t_config_reload, ""),
    (t_load_request_during_wait, "DummyLoadMs 3000\n"),
    (t_load_state, "DummyLoadMs 300\nDummyIdleTimeout 60\n"),
    (t_load_idle, "DummyIdleTimeout 1\n"),
    (t_load_stale, ("", {"dummy.load": "dummy-en-female-1\n"})),
    (t_load_garbage, ""),
    (t_probe_crash, "DummyModelDir @DIR@\n"),
    (t_probe_exit, "DummyModelDir @DIR@\n"),
    (t_probe_killed, "DummyModelDir @DIR@\n"),
    (t_probe_cached, "DummyModelDir @DIR@\n"),
    (t_probe_subdir, "DummyModelDir @DIR@\n"),
    (t_probe_engine, "DummyModelDir @DIR@\n"),
    (t_probe_hang, "DummyModelDir @DIR@\nDummyProbeTimeout 1\n"),
    (t_probe_quit, "DummyModelDir @DIR@\n"),
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
    t0 = time.time()
    m.send("STOP\n")
    end = None
    while end is None and time.time() - t0 < 5:
        end = next(((t, l) for t, l in m.lines if l.startswith(("702 END", "703 STOP"))), None)
        time.sleep(0.02)
    check(end is not None, "no 703 STOP after STOP")
    t, line = end
    if line.startswith("702"):
        # the model was there already (Piper, an adopted server): the
        # message ended before STOP came
        check(t - t0 < 0.1, "702 END %.2f s after STOP" % (t - t0))
    else:
        check(t - t0 < 0.3, "STOP during load took %.2f s" % (t - t0))
    m.pos = len(m.lines)
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


def r_rate(m):
    # speed 1, 2 (the model), 4 (the model 2, sonic 2); with AudioCopyDir
    # set the copies are there to listen to
    def samples(rate):
        m.set(rate=str(rate))
        start = len(m.lines)
        m.speak("Съешь же ещё этих мягких французских булок, да выпей чаю.")
        m.expect("702 END", 60)
        return sum(int(e.split()[1]) for e in m.events(start) if e.startswith("AUDIO"))
    n0, n50, n100 = samples(0), samples(50), samples(100)
    m.set(rate="0")
    check(0.35 < n50 / n0 < 0.7 and 0.15 < n100 / n0 < 0.35,
          "samples %d %d %d" % (n0, n50, n100))


def r_char_key(m):
    for text, kind in (("щ", "CHAR"), (",", "CHAR"), ("control_alt_delete", "KEY")):
        start = len(m.lines)
        m.speak(text, kind)
        m.expect("702 END", 60)
        n = sum(int(e.split()[1]) for e in m.events(start) if e.startswith("AUDIO"))
        check(n > 1000, "%s %r: %d samples" % (kind, text, n))


REAL_TESTS = [(r_speak, ""), (r_rate, ""), (r_char_key, ""), (r_stop_during_load, ""), (r_stop_speak, ""),
              (r_quit_during_load, ""), (t_quit, ""), (t_eof_during_speech, "")]


def main():
    want = WANT
    failed = 0
    for fn, conf in (REAL_TESTS if REAL else TESTS):
        name = fn.__name__[2:]
        if want and name not in want:
            continue
        conf, run_files = conf if isinstance(conf, tuple) else (conf, None)
        m = Mod(conf, run_files=run_files)
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
