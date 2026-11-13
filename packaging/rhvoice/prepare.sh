#!/usr/bin/env python3
# Drops the voice packages (rhvoice-<language>) from debian/control: some
# voices are CC-BY-NC-ND, the PPA carries only the engine and the patched
# Speech Dispatcher module. The voices of the Ubuntu archive (multiverse)
# work with it, they do not depend on the engine's version. The language
# data and the voices that those packages carried are removed after the
# install, or dh_missing stops the build.
import re, pathlib
p = pathlib.Path("debian/control")
stanzas = p.read_text().split("\n\n")
keep = [s for s in stanzas
        if not re.search(r"^Package: rhvoice-(?!.*-dev)\S+$", s, re.M)]
assert len(keep) < len(stanzas)
p.write_text("\n\n".join(keep))

# the language data and the voices ship only in the dropped packages
for f in pathlib.Path("debian").glob("rhvoice-*.install"):
    if not f.name.startswith("rhvoice-dev"):
        f.unlink()
r = pathlib.Path("debian/rules")
rules = r.read_text()
old = "\tscons DESTDIR=$(CURDIR)/debian/tmp install\n"
assert old in rules
r.write_text(rules.replace(old, old +
    "\trm -rf debian/tmp/usr/share/RHVoice/languages debian/tmp/usr/share/RHVoice/voices\n"))
