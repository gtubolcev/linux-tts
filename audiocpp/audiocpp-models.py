#!/usr/bin/python3 -I
"""Installs audio.cpp models that the PPA may not ship (their licences are
not free) into the user's model directory, ~/.local/share/audiocpp-tts.

usage: audiocpp-models.py list
       audiocpp-models.py install ID [--force]
       audiocpp-models.py remove ID

The catalogue models/models.json pins every download by URL (a fixed
revision), size and SHA-256; converted files are checked against the
expected SHA-256 too. Output is JSON: `list` prints one document; `install`
prints one object per line:
  {"stage": "download", "file": NAME, "done": BYTES, "total": BYTES}
  {"stage": "convert"}   {"stage": "check"}
  {"stage": "done", "dir": DIR}
  {"stage": "error", "code": CODE, "message": TEXT}
and exits 0 only after "done". SIGTERM or SIGINT cancels it and removes
the partial download. Only one install of a model runs at a time.

Run with python3 -I: the downloads are untrusted data.  The converters run
as separate python3 -I processes in the temporary directory.
"""
import errno, fcntl, hashlib, json, os, re, shutil, signal, subprocess, sys
import tempfile, time, urllib.error, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "models"
CATALOGUE = DATA / "models.json"
TIMEOUT = 30          # seconds without data before a retry
RETRIES = 3
CHUNK = 1 << 16
MARGIN = 64 << 20     # free space kept on top of what the install needs
USER_AGENT = "audiocpp-models/1"
PY_PACKAGES = {"numpy": "python3-numpy", "onnx": "python3-onnx"}


class Failure(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def emit(**kw):
    sys.stdout.write(json.dumps(kw, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def models_dir():
    base = os.environ.get("XDG_DATA_HOME") or ""
    if not os.path.isabs(base):
        base = os.path.join(os.path.expanduser("~"), ".local", "share")
    return Path(base) / "audiocpp-tts"


def load_catalogue():
    with open(CATALOGUE, encoding="utf-8") as f:
        cat = json.load(f)
    if cat.get("version") != 1:
        raise Failure("catalogue", "unknown catalogue version")
    ids = set()
    for m in cat["models"]:
        if not re.fullmatch(r"[a-z0-9][a-z0-9._-]*", m["id"]) or m["id"] in ids:
            raise Failure("catalogue", "bad model id %r" % m["id"])
        ids.add(m["id"])
        for d in m["downloads"]:
            safe_rel(d["to"])
            if not d["url"].startswith("https://"):
                raise Failure("catalogue", "not an https URL: " + d["url"])
    return cat["models"]


def safe_rel(p):
    """a relative path that stays inside its directory"""
    parts = Path(p).parts
    if not parts or Path(p).is_absolute() or any(x in ("..", ".", "") for x in parts):
        raise Failure("catalogue", "bad path %r" % p)
    return Path(*parts)


def missing_requirements(m):
    out = []
    for mod in m.get("requires", []):
        r = subprocess.run([sys.executable, "-I", "-c", "import " + mod],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           cwd="/")
        if r.returncode != 0:
            out.append(PY_PACKAGES.get(mod, mod))
    for pkg, path in m.get("packages", {}).items():
        if not os.path.exists(path):
            out.append(pkg)
    return out


def read_source(d):
    try:
        with open(d / "SOURCE", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def installed_state(m, base):
    """"ours": installed by this tool; "other": a model directory of the same
    name put there by hand; None: not installed"""
    d = base / m["id"]
    if not (d / "audiocpp.ini").is_file():
        return None
    src = read_source(d)
    return "ours" if src and src.get("id") == m["id"] else "other"


def cmd_list(models):
    base = models_dir()
    res = []
    for m in models:
        res.append({
            "id": m["id"], "label": m["label"], "model": m["model"],
            "module": "audiocpp", "languages": m["languages"],
            "download": sum(d["size"] for d in m["downloads"]),
            "disk": m["disk"], "licence": m["licence"],
            "homepage": m["homepage"], "note": m.get("note", ""),
            "installed": installed_state(m, base),
            "missing": missing_requirements(m),
        })
    json.dump({"dir": str(base), "models": res}, sys.stdout, ensure_ascii=False, indent=1)
    sys.stdout.write("\n")


class Progress:
    def __init__(self, total):
        self.total, self.done, self.last = total, 0, 0.0

    def add(self, name, n, force=False):
        self.done += n
        now = time.monotonic()
        if force or now - self.last >= 0.25:
            self.last = now
            emit(stage="download", file=name, done=self.done, total=self.total)


def download(d, dest, prog):
    """one file to dest, checked; retried from the start on network errors"""
    name = d["to"]
    start = prog.done
    for attempt in range(RETRIES):
        prog.done = start
        h = hashlib.sha256()
        size = 0
        try:
            req = urllib.request.Request(d["url"], headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r, open(dest, "wb") as f:
                if not r.geturl().startswith("https://"):
                    raise Failure("download", "redirected away from https: " + name)
                while True:
                    buf = r.read(CHUNK)
                    if not buf:
                        break
                    size += len(buf)
                    if size > d["size"]:
                        raise Failure("checksum", "%s is larger than expected" % name)
                    h.update(buf)
                    f.write(buf)
                    prog.add(name, len(buf))
            if size != d["size"] or h.hexdigest() != d["sha256"]:
                raise Failure("checksum", "%s is not the expected file (size %d, sha256 %s)"
                              % (name, size, h.hexdigest()))
            prog.add(name, 0, force=True)
            return
        except Failure:
            raise
        except urllib.error.HTTPError as e:
            if e.code < 500 or attempt == RETRIES - 1:
                raise Failure("download", "%s: HTTP %d" % (name, e.code))
        except OSError as e:
            if isinstance(e, OSError) and e.errno == errno.ENOSPC:
                raise Failure("space", "the disk is full")
            if attempt == RETRIES - 1:
                raise Failure("download", "%s: %s" % (name, getattr(e, "reason", e)))
        time.sleep(2 * (attempt + 1))


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for buf in iter(lambda: f.read(1 << 20), b""):
            h.update(buf)
    return h.hexdigest()


def cmd_install(models, mid, force):
    m = next((x for x in models if x["id"] == mid), None)
    if m is None:
        raise Failure("unknown", "no model %s in the catalogue" % mid)
    base = models_dir()
    base.mkdir(parents=True, exist_ok=True)
    lock = open(base / (".install-%s.lock" % mid), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        raise Failure("busy", "%s is being installed already" % mid)

    target = base / mid
    state = installed_state(m, base)
    if target.exists() and not force:
        if state is None:
            raise Failure("exists", "%s exists and is not a model directory" % target)
        emit(stage="done", dir=str(target), already=True)
        return
    if target.exists() and state != "ours":
        raise Failure("exists", "%s was not installed by this tool; remove it by hand" % target)

    missing = missing_requirements(m)
    if missing:
        raise Failure("requires", "needs " + ", ".join(missing))
    total = sum(d["size"] for d in m["downloads"])
    need = total + m["disk"] + MARGIN
    free = shutil.disk_usage(base).free
    if free < need:
        raise Failure("space", "needs %d MB free in %s, %d MB are"
                      % (need >> 20, base, free >> 20))

    # leftovers of a killed install
    for old in base.glob(".install-%s-*" % mid):
        shutil.rmtree(old, ignore_errors=True)
    tmp = Path(tempfile.mkdtemp(prefix=".install-%s-" % mid, dir=base))
    try:
        src, out = tmp / "src", tmp / "out"
        src.mkdir()
        out.mkdir()
        prog = Progress(total)
        for d in m["downloads"]:
            dest = src / safe_rel(d["to"])
            dest.parent.mkdir(parents=True, exist_ok=True)
            download(d, dest, prog)

        if m.get("convert"):
            emit(stage="convert")
            conv = safe_rel(m["convert"][0])
            args = [a.format(data=DATA, src=src, out=out) for a in m["convert"][1:]]
            log = tmp / "convert.log"
            with open(log, "w") as lf:
                r = subprocess.run([sys.executable, "-I", str(HERE / conv)] + args,
                                   cwd=tmp, stdin=subprocess.DEVNULL,
                                   stdout=lf, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                tail = log.read_text(errors="replace").strip().splitlines()[-3:]
                raise Failure("convert", "conversion failed: " + " / ".join(tail))

        for d in m["downloads"]:
            if d.get("keep"):
                rel = safe_rel(d["to"])
                (out / rel.name).unlink(missing_ok=True)
                shutil.move(str(src / rel), str(out / rel.name))
        for name, to in m.get("copy", {}).items():
            shutil.copyfile(DATA / safe_rel(name), out / safe_rel(to))

        emit(stage="check")
        for rel, sha in m.get("result", {}).items():
            p = out / safe_rel(rel)
            if not p.is_file() or sha256_file(p) != sha:
                raise Failure("checksum", "the converted %s is not the expected file" % rel)

        with open(out / "SOURCE", "w", encoding="utf-8") as f:
            json.dump({"id": mid, "label": m["label"], "homepage": m["homepage"],
                       "licence": m["licence"], "note": m.get("note", ""),
                       "installed": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                       "downloads": [{k: d[k] for k in ("url", "size", "sha256")}
                                     for d in m["downloads"]]},
                      f, ensure_ascii=False, indent=1)
        for p in out.rglob("*"):
            p.chmod(0o755 if p.is_dir() else 0o644)
        out.chmod(0o755)

        if target.exists():
            old = tmp / "old"
            os.rename(target, old)
        os.rename(out, target)
        emit(stage="done", dir=str(target))
    except OSError as e:
        if e.errno == errno.ENOSPC:
            raise Failure("space", "the disk is full")
        raise Failure("io", str(e))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cmd_remove(models, mid):
    m = next((x for x in models if x["id"] == mid), None)
    if m is None:
        raise Failure("unknown", "no model %s in the catalogue" % mid)
    base = models_dir()
    target = base / mid
    state = installed_state(m, base)
    if state is None and not target.exists():
        emit(stage="done", dir=str(target))
        return
    if state != "ours":
        raise Failure("exists", "%s was not installed by this tool" % target)
    gone = base / (".remove-%s-%d" % (mid, os.getpid()))
    os.rename(target, gone)
    shutil.rmtree(gone, ignore_errors=True)
    emit(stage="done", dir=str(target))


def on_signal(signum, frame):
    raise Failure("cancelled", "cancelled")


def main(argv):
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGHUP, on_signal)
    os.umask(0o022)
    if len(argv) < 2 or argv[1] not in ("list", "install", "remove") \
            or (argv[1] != "list" and len(argv) < 3):
        sys.exit(__doc__.strip())
    try:
        models = load_catalogue()
        if argv[1] == "list":
            cmd_list(models)
        elif argv[1] == "install":
            cmd_install(models, argv[2], "--force" in argv[3:])
        else:
            cmd_remove(models, argv[2])
    except Failure as e:
        if argv[1] == "list":
            sys.stderr.write("audiocpp-models: %s\n" % e)
        else:
            emit(stage="error", code=e.code, message=str(e))
        return 1
    except (OSError, ValueError, KeyError) as e:
        emit(stage="error", code="internal", message=repr(e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
