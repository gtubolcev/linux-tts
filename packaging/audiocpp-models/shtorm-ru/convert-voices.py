#!/usr/bin/python3 -I
"""Turns Shtorm-Russian-References-206-v1.zip (mp3, 24 kHz mono) into the
voices/ directory of the model package: one WAV per voice (audio.cpp reads
WAV references) and the [voice ...] sections for audiocpp.ini.

usage: convert-voices.py ZIP OUTDIR     (run it in a container: the mp3 files
are untrusted data and go through ffmpeg)

Every file is checked against the size and SHA-256 in the zip's index.json
before it is decoded. Prints the number of voices; writes OUTDIR/voices/*.wav,
OUTDIR/voices.ini and OUTDIR/index.json (the zip's own index, kept for the
labels)."""
import hashlib, json, os, re, subprocess, sys, tempfile, zipfile

zpath, out = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(zpath)
index = json.loads(z.read("index.json"))
assert index["codec"] == "mp3" and index["sample_rate"] == 24000 and index["channels"] == 1
os.makedirs(os.path.join(out, "voices"), exist_ok=True)
sections, names = [], set()
files = sorted(index["files"], key=lambda f: (f["gender"] != "f", f["age_group"] != "adult", f["id"]))
with tempfile.TemporaryDirectory() as tmp:
    for f in files:
        data = z.read(os.path.basename(f["file"]))
        if len(data) != f["size"] or hashlib.sha256(data).hexdigest() != f["sha256"]:
            sys.exit("bad file " + f["file"])
        m = re.fullmatch(r"pack_qvd_([mf]\d{4})_\w+", f["id"])
        person = re.sub(r"[^A-Za-z]", "", f["person_name"])
        if not m or not person:
            sys.exit("odd id " + f["id"])
        name = "%s-%s" % (person, m.group(1))
        assert name not in names
        names.add(name)
        src = os.path.join(tmp, name + ".mp3")
        with open(src, "wb") as fh:
            fh.write(data)
        subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-y", "-i", src, "-ac", "1",
                        "-ar", "24000", "-c:a", "pcm_s16le", "-bitexact",
                        "-map_metadata", "-1", os.path.join(out, "voices", name + ".wav")],
                       check=True)
        sections.append("[voice %s]\ngender=%s\nreference=voices/%s.wav\n"
                        % (name, "female" if f["gender"] == "f" else "male", name))
open(os.path.join(out, "voices.ini"), "w").write("\n".join(sections))
with open(os.path.join(out, "index.json"), "w", encoding="utf-8") as fh:
    json.dump(index, fh, ensure_ascii=False, indent=1)
print(len(sections))
