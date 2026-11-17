#!/usr/bin/python3 -I
"""Sits between a private speech-dispatcher and a real output module.

Logs every command speechd sends (SET values, the SPEAK text after speechd's
punctuation and capital processing, SOUND_ICON, CHAR, KEY). A module that
sends its audio through the protocol (audio_output_method=server) gets it
written to WAV files instead of passed on; one that opens the audio itself
plays into the ALSA file plugin of run.sh, which feeds record.py.

Started by speechd as the module binary through a wrapper:
    proxy.py <real module binary> <module config>
CHECK_OUT names the directory for the log and the WAV files.
"""
import os, subprocess, sys, threading, time, wave

real, conf = sys.argv[1], sys.argv[2:]
name = os.path.basename(real)
out = os.environ.get('CHECK_OUT', '.')
log = open(os.path.join(out, name + '.proto.log'), 'a', buffering=1)
mod = subprocess.Popen([real] + conf, stdin=subprocess.PIPE,
                       stdout=subprocess.PIPE, bufsize=0)
count = 0
lock = threading.Lock()


def note(*a):
    with lock:
        log.write('%.3f ' % time.time() + ' '.join(str(x) for x in a) + '\n')


def unescape(b):
    o = bytearray()
    i = 0
    while i < len(b):
        if b[i] == 0x7d and i + 1 < len(b):
            o.append(b[i + 1] ^ 0x20)
            i += 2
        else:
            o.append(b[i])
            i += 1
    return bytes(o)


def from_speechd():
    """speechd -> module; multi-line commands end with a line '.'"""
    stdin = sys.stdin.buffer
    block = None
    while True:
        line = stdin.readline()
        if not line:
            break
        text = line.decode('utf-8', 'replace').rstrip('\n')
        if block is not None:
            if text == '.':
                note('>>', block[0], '|'.join(block[1:]))
                block = None
            else:
                block.append(text)
        elif text in ('SET', 'AUDIO', 'SPEAK', 'SOUND_ICON', 'CHAR', 'KEY',
                      'LOGLEVEL', 'DEBUG'):
            block = [text]
        else:
            note('>>', text)
        mod.stdin.write(line)
    mod.stdin.close()


def from_module():
    """module -> speechd, without the audio blocks"""
    global count
    rd = mod.stdout
    pcm = bytearray()
    rate = 0
    while True:
        line = rd.readline()
        if not line:
            break
        if line.startswith(b'705-AUDIO'):
            data = line[len(b'705-AUDIO') + 1:]
            while not data.endswith(b'\n705 AUDIO\n'):
                more = rd.readline()
                if not more:
                    break
                data += more
            pcm += unescape(data[:-len(b'\n705 AUDIO\n')])
            continue
        if line.startswith(b'705-'):
            if line.startswith(b'705-sample_rate='):
                rate = int(line.split(b'=')[1])
            continue
        if line.startswith(b'705 '):
            continue
        text = line.decode('utf-8', 'replace').rstrip('\n')
        if text.startswith(('701', '702', '703', '704', '706')):
            note('<<', text)
        if text.startswith(('702', '703')) and pcm:
            count += 1
            path = os.path.join(out, '%s-%03d.wav' % (name, count))
            w = wave.open(path, 'wb')
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate or 22050)
            w.writeframes(bytes(pcm))
            w.close()
            note('wav', path, rate, len(pcm) // 2)
            pcm = bytearray()
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()


t = threading.Thread(target=from_speechd, daemon=True)
t.start()
from_module()
mod.wait()
