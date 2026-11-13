#!/usr/bin/env python3
# Drops the voice packages (rhvoice-<language>) from debian/control: some
# voices are CC-BY-NC-ND, the PPA carries only the engine and the patched
# Speech Dispatcher module. The voices of the Ubuntu archive (multiverse)
# work with it, they do not depend on the engine's version.
import re, pathlib
p = pathlib.Path("debian/control")
stanzas = p.read_text().split("\n\n")
keep = [s for s in stanzas
        if not re.search(r"^Package: rhvoice-(?!.*-dev)\S+$", s, re.M)]
assert len(keep) < len(stanzas)
p.write_text("\n\n".join(keep))
