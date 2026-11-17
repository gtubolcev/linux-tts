#!/usr/bin/python3 -I
"""Sends the normalization test sentences, one message each, and writes the
time window of every message to tests.json.
    client.py <socket> <module> <language> <texts.tsv> <tests.json>"""
import json, sys, threading, time
import speechd

sock, module, lang, texts, out = sys.argv[1:6]
c = speechd.SSIPClient('norm-test', socket_path=sock, autospawn=False)
c.set_output_module(module)
c.set_language(lang)
c.set_punctuation(speechd.PunctuationMode.NONE)
done = threading.Event()


def cb(kind, *a, **k):
    if kind in (speechd.CallbackType.END, speechd.CallbackType.CANCEL):
        done.set()


tests = []
lines = [l.rstrip('\n').split('\t', 1) for l in open(texts, encoding='utf-8') if '\t' in l]
for name, text in [('warmup', 'Проверка.')] + lines:
    done.clear()
    t0 = time.time()
    c.speak(text, callback=cb, event_types=(speechd.CallbackType.END,
                                            speechd.CallbackType.CANCEL))
    ok = done.wait(300)
    tests.append({'name': name, 'start': t0, 'end': time.time(), 'ok': ok, 'text': text})
    time.sleep(1.2)
json.dump(tests, open(out, 'w'), ensure_ascii=False, indent=1)
c.close()
