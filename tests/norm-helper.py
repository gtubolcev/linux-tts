#!/usr/bin/python3 -I
"""norm-helper.py TTS-NORM-TEST TTS-NORMALIZE: the one normalizer per user.

Runs tts-norm-test (common/tts-norm.c) against tts-normalize with a fake
ru_normalizr that answers "N[text]<server pid>": modules share one server,
it exits when idle, survives a stale socket, and every failure gives the
text back unchanged.
"""
import hashlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

FAKE = '''
import os, time
class NormalizeOptions:
    @staticmethod
    def tts():
        return None
class Normalizer:
    def __init__(self, options):
        if os.environ.get("FAKE_NORM_FAIL"):
            raise ImportError("fake broken install")
        time.sleep(float(os.environ.get("FAKE_NORM_LOAD", "0.5")))
    def normalize(self, text):
        flag = os.environ.get("FAKE_DIE_FLAG")
        if "DIE" in text and flag and not os.path.exists(flag):
            open(flag, "w").close()
            os._exit(1)
        if "SLOW" in text:
            time.sleep(3)
        if "BOOM" in text:
            raise ValueError("boom")
        return "N[%s]%d" % (text, os.getpid())
'''

TEST, HELPER_SRC = sys.argv[1], sys.argv[2]
ROOT = tempfile.mkdtemp(prefix="norm-helper-")
HELPER = os.path.join(ROOT, "h", "tts-normalize")
os.makedirs(os.path.join(ROOT, "h", "python", "ru_normalizr"))
shutil.copy(HELPER_SRC, HELPER)
with open(os.path.join(ROOT, "h", "python", "ru_normalizr", "__init__.py"), "w") as f:
    f.write(FAKE)
PYTHON = shutil.which("python3")
failed = []


def runtime(name):
    d = os.path.join(ROOT, name)
    os.makedirs(d, mode=0o700)
    return d


def sock_of(rt):
    h = hashlib.sha256((PYTHON + "\n" + HELPER).encode()).hexdigest()[:12]
    return os.path.join(rt, "tts-normalize-%s.sock" % h)


def client(rt, lines, *opts, env=None):
    e = dict(os.environ, XDG_RUNTIME_DIR=rt, **(env or {}))
    return subprocess.Popen([TEST, "--python", PYTHON, "--helper", HELPER, *opts],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                            env=e, stderr=subprocess.DEVNULL) if lines is None else \
        subprocess.run([TEST, "--python", PYTHON, "--helper", HELPER, *opts],
                       input="".join(x + "\n" for x in lines), capture_output=True,
                       text=True, env=e, timeout=60)


def answers(out):
    return [line.split("\t", 1) for line in out.splitlines()]


def server_pids(rt):
    """pids of tts-normalize processes serving this runtime dir"""
    pids = []
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                with open("/proc/%s/cmdline" % p, "rb") as f:
                    cmd = f.read().split(b"\0")
            except OSError:
                continue
            if HELPER.encode() in cmd and sock_of(rt).encode() in cmd:
                pids.append(int(p))
    return pids


def wait_gone(rt, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if not server_pids(rt):
            return True
        time.sleep(0.2)
    return False


def alive(pid):
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def check(name, ok, detail=""):
    print("%-4s %s %s" % ("ok" if ok else "FAIL", name, detail if not ok else ""))
    if not ok:
        failed.append(name)


def t_shared():
    rt = runtime("shared")
    procs = [client(rt, None, "--idle", "2") for _ in range(3)]
    outs = []
    for i, p in enumerate(procs):
        p.stdin.write("слово %d\n" % i)
        p.stdin.flush()
    for p in procs:
        outs.append(p.stdout.readline().rstrip("\n").split("\t", 1)[1])
    pids = {o.rsplit("]", 1)[1] for o in outs}
    time.sleep(0.5)   # servers started at the same time leave once one answers
    check("shared: all normalized", all(o.startswith("N[слово") for o in outs), outs)
    check("shared: one server for three modules", len(pids) == 1 and len(server_pids(rt)) == 1,
          (pids, server_pids(rt)))
    # a module that disconnects does not stop it for the others
    procs[0].stdin.close()
    procs[0].wait(10)
    time.sleep(1.5)   # past the server's 1 s check for an update (and a fresh __pycache__)
    procs[1].stdin.write("ещё\n")
    procs[1].stdin.flush()
    o = procs[1].stdout.readline().rstrip("\n").split("\t", 1)[1]
    check("shared: stays while others are connected", o.endswith("]" + pids.pop()), o)
    for p in procs[1:]:
        p.stdin.close()
        p.wait(10)
    check("shared: exits when idle", wait_gone(rt, 6))
    check("shared: socket removed", not os.path.exists(sock_of(rt)))


def t_mode():
    rt = runtime("mode")
    p = client(rt, None, "--idle", "2")
    p.stdin.write("x\n")
    p.stdin.flush()
    p.stdout.readline()
    mode = os.stat(sock_of(rt)).st_mode & 0o777
    check("mode: socket 0600 or stricter", mode & 0o077 == 0, oct(mode))
    p.stdin.close()
    p.wait(10)
    wait_gone(rt, 6)


def t_stale():
    rt = runtime("stale")
    s = socket.socket(socket.AF_UNIX)
    s.bind(sock_of(rt))
    s.close()   # a socket file nobody listens on (a killed server)
    r = client(rt, ["a"], "--idle", "1")
    a = answers(r.stdout)
    check("stale socket: new server", len(a) == 1 and a[0][1].startswith("N[a]"), r.stdout)
    wait_gone(rt, 5)


def t_restart_after_idle():
    rt = runtime("again")
    r1 = client(rt, ["a"], "--idle", "1")
    p1 = answers(r1.stdout)[0][1].rsplit("]", 1)[1]
    check("again: first server gone", wait_gone(rt, 5))
    r2 = client(rt, ["b"], "--idle", "1")
    a = answers(r2.stdout)
    check("again: a new server after idle exit",
          a and a[0][1].startswith("N[b]") and not a[0][1].endswith("]" + p1), r2.stdout)
    wait_gone(rt, 5)


def t_broken():
    rt = runtime("broken")
    t = time.monotonic()
    r = client(rt, ["5 кг", "ещё"], env={"FAKE_NORM_FAIL": "1"})
    a = answers(r.stdout)
    check("broken: text unchanged", [x[1] for x in a] == ["5 кг", "ещё"], r.stdout)
    check("broken: no wait for the second line", time.monotonic() - t < 5)
    check("broken: server does not stay", wait_gone(rt, 5))


def t_slow_and_boom():
    rt = runtime("slow")
    r = client(rt, ["BOOM", "SLOW", "busy", "after"], "--timeout", "300", "--idle", "1")
    a = answers(r.stdout)
    check("boom: text unchanged", a[0][1] == "BOOM", r.stdout)
    check("slow: unchanged after the timeout", a[1][1] == "SLOW" and int(a[1][0]) < 1500, r.stdout)
    # the server still works on SLOW: a second late answer in a row
    check("slow: one more try", a[2][1] == "busy" and int(a[2][0]) < 1500, r.stdout)
    check("slow: then unnormalized for a while, no waiting", a[3][1] == "after"
          and int(a[3][0]) < 100, r.stdout)
    wait_gone(rt, 8)


def t_late_once():
    """one late answer costs only that text"""
    rt = runtime("late")
    r = client(rt, ["SLOW", "x"], "--timeout", "2500", "--idle", "1")
    a = answers(r.stdout)
    check("late once: the next text normalized", len(a) == 2 and a[0][1] == "SLOW"
          and a[1][1].startswith("N[x]"), r.stdout)
    wait_gone(rt, 8)


def t_crash_reconnect():
    """the server dies during a request: a new one at once, the same text
    normalized, no pause"""
    rt = runtime("crash")
    flag = os.path.join(rt, "died")
    r = client(rt, ["a", "DIE", "b"], "--idle", "1", env={"FAKE_DIE_FLAG": flag})
    a = answers(r.stdout)
    pids = [x[1].rsplit("]", 1)[1] for x in a if x[1].startswith("N[")]
    check("crash: it died", os.path.exists(flag))
    check("crash: all three normalized", len(a) == 3 and all(x[1].startswith("N[") for x in a),
          r.stdout)
    check("crash: by a new server", len(pids) == 3 and pids[0] != pids[1] == pids[2], pids)
    wait_gone(rt, 5)


def t_update():
    """a changed helper script: the old server goes, its module gets the new one"""
    rt = runtime("update")
    p = client(rt, None, "--idle", "5")
    p.stdin.write("a\n")
    p.stdin.flush()
    first = p.stdout.readline().rstrip("\n").split("\t", 1)[1]
    old = int(first.rsplit("]", 1)[1])
    os.utime(HELPER, None)
    t = time.monotonic()
    while alive(old) and time.monotonic() - t < 5:
        time.sleep(0.1)
    p.stdin.write("b\n")
    p.stdin.flush()
    second = p.stdout.readline().rstrip("\n").split("\t", 1)[1]
    p.stdin.close()
    p.wait(10)
    check("update: old server left", not alive(old), (first, second))
    check("update: new server answers", second.startswith("N[b]")
          and second.rsplit("]", 1)[1] != first.rsplit("]", 1)[1], (first, second))
    wait_gone(rt, 8)


def t_cancel():
    """the message is stopped while the normalizer works (it was a NULL
    dereference): no crash, the text passes unchanged"""
    rt = runtime("cancel")
    r = client(rt, ["SLOW", "x"], "--timeout", "8000", "--idle", "2", "--cancel-after", "1500")
    a = answers(r.stdout)
    check("cancel: no crash", r.returncode == 0 and len(a) == 2, "rc=%d %r" % (r.returncode, r.stdout))
    check("cancel: stopped text unchanged", len(a) == 2 and a[0][1] == "SLOW"
          and int(a[0][0]) < 2500, r.stdout)
    wait_gone(rt, 10)


def t_no_helper():
    rt = runtime("none")
    e = dict(os.environ, XDG_RUNTIME_DIR=rt)
    r = subprocess.run([TEST, "--helper", os.path.join(ROOT, "missing")], input="a\n",
                       capture_output=True, text=True, env=e, timeout=20)
    check("no helper: text unchanged", answers(r.stdout)[0][1] == "a", r.stdout)


def t_load_race():
    # many modules at once against a slow load: still one server
    rt = runtime("race")
    env = {"FAKE_NORM_LOAD": "1.5"}
    procs = [client(rt, None, "--idle", "1", env=env) for _ in range(6)]
    for p in procs:
        p.stdin.write("r\n")
        p.stdin.close()
    outs = [p.stdout.read() for p in procs]
    for p in procs:
        p.wait(20)
    pids = {answers(o)[0][1].rsplit("]", 1)[1] for o in outs if answers(o)}
    check("race: all answered by one server", len(pids) == 1
          and all(answers(o)[0][1].startswith("N[r]") for o in outs), outs)
    wait_gone(rt, 5)


try:
    for t in (t_shared, t_mode, t_stale, t_restart_after_idle, t_broken, t_slow_and_boom,
              t_late_once, t_crash_reconnect, t_update, t_cancel, t_no_helper, t_load_race):
        t()
finally:
    for name in os.listdir(ROOT):
        if os.path.isdir(os.path.join(ROOT, name)) and name != "h":
            for pid in server_pids(os.path.join(ROOT, name)):
                os.kill(pid, 9)
    shutil.rmtree(ROOT, ignore_errors=True)
sys.exit(1 if failed else 0)
