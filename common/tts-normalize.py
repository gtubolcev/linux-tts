#!/usr/bin/python3 -I
# tts-normalize - the text normalizer of the speech-dispatcher modules
#
# One process per user serves all modules (ru_normalizr and its dictionaries
# take ~3 s and tens of MB to load).  common/tts-norm.c starts it as
#   tts-normalize --socket PATH [--idle SECONDS]
# when nobody listens on PATH, and connects to it.  On every connection the
# server prints "READY" once ru_normalizr is loaded (or "ERROR<TAB>why"),
# then answers each "<id><TAB><lang><TAB><text>" line with
# "<id><TAB><normalized text>".  Any failure on one request answers with the
# text as it came, so the module never waits for a timeout because of one odd
# sentence.  The server exits when no module has been connected for --idle
# seconds, and at once when this script or the private ru_normalizr changed
# (a package update): its modules see it go and start the new code.
#
# Without --socket it serves one module over stdin/stdout (the protocol of
# older modules, which start it as a child).
#
# Copyright (C) 2026 Grigory Tuboltsev
# SPDX-License-Identifier: GPL-3.0-or-later

import fcntl
import os
import socket
import sys
import threading
import time
import warnings

MAX_LINE = 1 << 20
# speech-dispatcher-ru-normalizr: ru_normalizr and the libraries Ubuntu lacks,
# outside the system Python; before it on the path, so its versions win
PRIVATE = os.path.join(os.path.dirname(os.path.realpath(__file__)), "python")
if os.path.isdir(PRIVATE):
    sys.path.insert(0, PRIVATE)


def log(msg):
    sys.stderr.write("tts-normalize: %s\n" % msg)
    sys.stderr.flush()


def code_stamp():
    """what this script and the private ru_normalizr look like on disk.

    The package's __init__.py, not its directory: importing it from a writable
    directory creates __pycache__ there, which changes the directory's mtime and
    made the server take itself for updated and leave under its clients.  An
    upgrade by dpkg writes every file anew, __init__.py too."""
    out = []
    for p in (os.path.realpath(__file__), os.path.join(PRIVATE, "ru_normalizr", "__init__.py")):
        try:
            st = os.stat(p)
            out.append((st.st_ino, st.st_size, st.st_mtime_ns))
        except OSError:
            out.append(None)
    return out


def one_line(text):
    return text.replace("\t", " ").replace("\r", " ").replace("\n", " ")


class Engine:
    """ru_normalizr, loaded once; normalize() may be called from any thread."""

    def __init__(self):
        self.loaded = threading.Event()
        self.error = None
        self.ru = None
        self.lock = threading.Lock()   # ru_normalizr is not known to be thread-safe

    def load(self):
        # old dependencies warn at import; that is not for the speechd log
        warnings.simplefilter("ignore", SyntaxWarning)
        warnings.simplefilter("ignore", DeprecationWarning)
        try:
            from ru_normalizr import NormalizeOptions, Normalizer

            ru = Normalizer(NormalizeOptions.tts())
            ru.normalize("1")  # load the dictionaries now, not on the first message
            self.ru = ru
        except Exception as e:  # noqa: BLE001 - any import failure means "not available"
            self.error = one_line("%s: %s" % (type(e).__name__, e))
        self.loaded.set()

    def normalize(self, lang, text):
        if not lang.lower().startswith("ru"):
            return text
        try:
            with self.lock:
                return one_line(self.ru.normalize(text))
        except Exception as e:  # noqa: BLE001 - a bug in one rule must not stop speech
            log("%s: %s" % (type(e).__name__, e))
            return text


def serve_stream(engine, rfile, wfile):
    """One module: READY/ERROR, then request lines until EOF."""
    engine.loaded.wait()
    if engine.error is not None:
        wfile.write(b"ERROR\t" + engine.error.encode("utf-8", "replace") + b"\n")
        wfile.flush()
        return
    wfile.write(b"READY\n")
    wfile.flush()
    while True:
        raw = rfile.readline(MAX_LINE)
        if not raw:
            return
        if not raw.endswith(b"\n"):
            # longer than any request the module sends: drop the rest
            while raw and not raw.endswith(b"\n"):
                raw = rfile.readline(MAX_LINE)
            continue
        parts = raw[:-1].decode("utf-8", "replace").split("\t", 2)
        if len(parts) != 3 or not parts[0].isdigit():
            continue
        rid, lang, text = parts
        wfile.write(("%s\t%s\n" % (rid, engine.normalize(lang, text))).encode("utf-8", "replace"))
        wfile.flush()


def main_stdio():
    engine = Engine()
    engine.load()
    serve_stream(engine, sys.stdin.buffer, sys.stdout.buffer)
    return 1 if engine.error is not None else 0


class Server:
    def __init__(self, path, idle):
        self.path = path
        self.idle = idle
        self.engine = Engine()
        self.clients = 0
        self.last = time.monotonic()
        self.lock = threading.Lock()
        self.stamp = code_stamp()

    def client(self, conn):
        with self.lock:
            self.clients += 1
        try:
            with conn, conn.makefile("rb") as r, conn.makefile("wb") as w:
                serve_stream(self.engine, r, w)
        except OSError:
            pass   # the module went away
        finally:
            with self.lock:
                self.clients -= 1
                self.last = time.monotonic()

    def idle_for(self):
        with self.lock:
            return 0 if self.clients else time.monotonic() - self.last

    def answers(self):
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            probe.connect(self.path)
            return True
        except OSError:
            return False
        finally:
            probe.close()

    def run(self):
        # the server holds the lock file as long as it runs; a second one
        # started at the same time leaves as soon as the socket answers
        lock = open(self.path + ".lock", "a")
        end = time.monotonic() + 30
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if self.answers() or time.monotonic() > end:
                    return 0
                time.sleep(0.05)
        if self.answers():
            return 0   # a server that has just let go of the lock still listens
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(self.path)
        srv.listen(16)
        srv.settimeout(1.0)
        threading.Thread(target=self.engine.load, daemon=True).start()
        try:
            while True:
                try:
                    conn, _ = srv.accept()
                except socket.timeout:
                    loaded = self.engine.loaded.is_set()
                    idle = self.idle_for()
                    # a broken installation does not stay: its clients got ERROR
                    if idle >= self.idle or (loaded and self.engine.error and idle >= 1):
                        break
                    if code_stamp() != self.stamp:
                        log("updated; the modules start the new version")
                        break
                    continue
                conn.settimeout(None)
                threading.Thread(target=self.client, args=(conn,), daemon=True).start()
        finally:
            # still under the lock: a new server waits for it, then binds anew
            try:
                os.unlink(self.path)
            except OSError:
                pass
            srv.close()
        if self.engine.error:
            log(self.engine.error)
        return 0


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = sys.argv[1:]
    if not args:
        return main_stdio()
    path, idle = None, 60.0
    while args:
        opt = args.pop(0)
        if opt == "--socket" and args:
            path = args.pop(0)
        elif opt == "--idle" and args:
            idle = max(float(args.pop(0)), 1.0)
        else:
            log("usage: tts-normalize [--socket PATH [--idle SECONDS]]")
            return 2
    try:
        os.setsid()   # not killed with the module that started it
    except OSError:
        pass
    os.umask(0o077)
    return Server(path, idle).run()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BrokenPipeError, KeyboardInterrupt):
        sys.exit(0)
