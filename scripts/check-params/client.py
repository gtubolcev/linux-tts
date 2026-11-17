#!/usr/bin/python3 -I
"""Sends the test messages to the private speech-dispatcher, one at a time.

    client.py <socket> <module> <language> <tests.json>

Writes the time window of every message to tests.json, so analyze.py can
match the WAV files to the settings."""
import json, sys, threading, time
import speechd

sock, module, lang, out = sys.argv[1:5]

TEXTS = {
    'ru': ('Так говорит ваш компьютер, и это тестовая фраза.',
           'Привет, мир; тест: да! (скобки) «кавычки» - тире @ # 5% ?',
           'Москва и ООН. Я. Ты.'),
    'en': ('This is how your computer speaks, and this is a test.',
           'Hello, world; test: yes! (brackets) "quotes" - dash @ # 5% ?',
           'London and NATO. I. You.'),
    'de': ('So spricht Ihr Computer, und das ist ein Test.',
           'Hallo, Welt; Test: ja! (Klammern) "Zitat" - Strich @ # 5% ?',
           'Berlin und NATO. Ich. Du.'),
}
plain, punct, caps = TEXTS.get(lang, TEXTS['en'])

c = speechd.SSIPClient('check-params', socket_path=sock, autospawn=False)
c.set_output_module(module)
c.set_language(lang)
done = threading.Event()


def cb(kind, *a, **k):
    if kind in (speechd.CallbackType.END, speechd.CallbackType.CANCEL):
        done.set()


tests = []


def say(name, text, **settings):
    c.set_rate(settings.get('rate', 0))
    c.set_pitch(settings.get('pitch', 0))
    c.set_volume(settings.get('volume', 0))
    c.set_punctuation(settings.get('punct', speechd.PunctuationMode.NONE))
    c.set_cap_let_recogn(settings.get('caps', 'none'))
    done.clear()
    t0 = time.time()
    c.speak(text, callback=cb, event_types=(speechd.CallbackType.END,
                                            speechd.CallbackType.CANCEL))
    ok = done.wait(300)
    tests.append({'name': name, 'start': t0, 'end': time.time(),
                  'ok': ok, 'text': text})
    time.sleep(1)


say('warmup', plain)
for v in (-100, -50, 0, 50, 100):
    say('rate %+d' % v, plain, rate=v)
for v in (-100, 0, 100):
    say('pitch %+d' % v, plain, pitch=v)
for v in (-100, -50, 0, 100):
    say('volume %+d' % v, plain, volume=v)
P = speechd.PunctuationMode
for n, m in (('none', P.NONE), ('some', P.SOME), ('most', P.MOST), ('all', P.ALL)):
    say('punct ' + n, punct, punct=m)
for n in ('none', 'spell', 'icon'):
    say('caps ' + n, caps, caps=n)

c.close()
json.dump(tests, open(out, 'w'), ensure_ascii=False, indent=1)
