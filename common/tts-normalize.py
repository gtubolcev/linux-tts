#!/usr/bin/python3 -I
# tts-normalize - the normalizer helper of the speech-dispatcher modules
#
# Started by common/tts-norm.c with the socket as stdin and stdout.
# Prints "READY" once ru_normalizr is loaded (or "ERROR<TAB>why" and
# exits), then answers each "<id><TAB><lang><TAB><text>" line with
# "<id><TAB><normalized text>".  Any failure on one request answers with
# the text as it came, so the module never waits for a timeout because of
# one odd sentence.
#
# Copyright (C) 2026 Grigory Tuboltsev
# SPDX-License-Identifier: MIT

import sys
import warnings

MAX_LINE = 1 << 20


def one_line(text):
    return text.replace("\t", " ").replace("\r", " ").replace("\n", " ")


def main():
    # speechd may run us with LANG=C
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    out = sys.stdout
    # old dependencies warn at import; that is not for the speechd log
    warnings.simplefilter("ignore", SyntaxWarning)
    warnings.simplefilter("ignore", DeprecationWarning)
    try:
        from ru_normalizr import NormalizeOptions, Normalizer

        ru = Normalizer(NormalizeOptions.tts())
        ru.normalize("1")  # load the dictionaries now, not on the first message
    except Exception as e:  # noqa: BLE001 - any import failure means "not available"
        out.write("ERROR\t%s\n" % one_line("%s: %s" % (type(e).__name__, e)))
        out.flush()
        return 1
    out.write("READY\n")
    out.flush()

    stdin = sys.stdin.buffer
    while True:
        raw = stdin.readline(MAX_LINE)
        if not raw:
            return 0
        if not raw.endswith(b"\n"):
            # longer than any request the module sends: drop the rest
            while raw and not raw.endswith(b"\n"):
                raw = stdin.readline(MAX_LINE)
            continue
        line = raw[:-1].decode("utf-8", "replace")
        parts = line.split("\t", 2)
        if len(parts) != 3 or not parts[0].isdigit():
            continue
        rid, lang, text = parts
        result = text
        if lang.lower().startswith("ru"):
            try:
                result = one_line(ru.normalize(text))
            except Exception as e:  # noqa: BLE001 - a bug in one rule must not stop speech
                sys.stderr.write("tts-normalize: %s: %s\n" % (type(e).__name__, e))
                sys.stderr.flush()
        out.write("%s\t%s\n" % (rid, result))
        out.flush()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BrokenPipeError, KeyboardInterrupt):
        sys.exit(0)
