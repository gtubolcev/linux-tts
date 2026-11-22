#!/usr/bin/python3 -I
"""check-distro-versions: are our patched distribution packages still ahead?

The PPA carries speech-dispatcher (+fixN: reload leaks) and rhvoice (+voiceN:
RHVoiceVoice) under the distribution's package names, built on a given Ubuntu
version.  When Ubuntu publishes a newer version (SRU, security, or a new
release in -proposed), apt prefers it and our patch is gone without a word;
the PPA package then needs a rebuild on top of it.

For every series the PPA builds for, this compares the newest Ubuntu version
(Release, Updates, Security; -proposed reported separately) with the Ubuntu
version our PPA package is based on, through the public Launchpad API
(anonymous).  It prints one line per package and series, and exits 1 when
something needs a rebuild, 2 when Launchpad cannot be asked.

  check-distro-versions.py [--quiet] [--notify]

--quiet prints only what needs doing; --notify also shows it with notify-send
(for a daily systemd --user timer).

Copyright (C) 2026 Grigory Tuboltsev
SPDX-License-Identifier: GPL-3.0-or-later
"""
import re
import shutil
import subprocess
import sys

PPA_OWNER, PPA_NAME = "gtubolcev", "linux-tts"
# source package -> the suffix our version adds to the Ubuntu version
PACKAGES = {"speech-dispatcher": r"\+fix\d+", "rhvoice": r"\+voice\d+"}


def vercmp(a, b):
    """-1, 0, 1 like dpkg --compare-versions"""
    if subprocess.run(["dpkg", "--compare-versions", a, "lt", b]).returncode == 0:
        return -1
    if subprocess.run(["dpkg", "--compare-versions", a, "eq", b]).returncode == 0:
        return 0
    return 1


def newest(versions):
    best = None
    for v in versions:
        if best is None or vercmp(v, best) > 0:
            best = v
    return best


def main():
    quiet = "--quiet" in sys.argv
    notify = "--notify" in sys.argv
    try:
        from launchpadlib.launchpad import Launchpad

        lp = Launchpad.login_anonymously("check-distro-versions", "production",
                                         version="devel")
        ubuntu = lp.distributions["ubuntu"]
        archive = ubuntu.main_archive
        ppa = lp.people[PPA_OWNER].getPPAByName(name=PPA_NAME)
    except Exception as e:  # noqa: BLE001 - network, Launchpad down
        print(f"cannot ask Launchpad: {type(e).__name__}: {e}", file=sys.stderr)
        return 2

    todo = []
    for name, suffix in PACKAGES.items():
        ours = {}
        for p in ppa.getPublishedSources(source_name=name, exact_match=True, status="Published"):
            series = p.distro_series_link.rstrip("/").split("/")[-1]
            ours.setdefault(series, []).append(p.source_package_version)
        for series, versions in sorted(ours.items()):
            mine = newest(versions)
            m = re.match(r"(.*?)" + suffix, mine)
            base = m.group(1) if m else mine
            # a repacked orig tarball (packaging/<pkg>/repack): 1.14.0+ds-3 is
            # built on 1.14.0-3, 1.8.0+dfsg2-3 on 1.8.0+dfsg-3
            base = re.sub(r"\+ds(?=-)|(?<=\+dfsg)2(?=-)|(?<=\+ds)2(?=-)", "", base)
            dseries = ubuntu.getSeries(name_or_version=series)
            released, proposed = [], []
            for p in archive.getPublishedSources(source_name=name, exact_match=True,
                                                 distro_series=dseries, status="Published"):
                (proposed if p.pocket == "Proposed" else released).append(p.source_package_version)
            top = newest(released)
            prop = newest(proposed)
            line = f"{name} {series}: PPA {mine} (on {base}), Ubuntu {top or '-'}"
            if prop:
                line += f", proposed {prop}"
            if top and vercmp(top, base) > 0:
                # apt compares top with mine: newer than our base means a rebuild,
                # even when "+fix" still sorts higher for now
                state = "REBUILD" if vercmp(top, mine) < 0 else "LOST (apt installs Ubuntu's)"
                todo.append(f"{line} -> {state}")
            elif prop and vercmp(prop, base) > 0:
                todo.append(f"{line} -> soon: rebuild on {prop} when it is released")
            elif not quiet:
                print(f"{line} ok")
    for t in todo:
        print(t)
    if todo and notify and shutil.which("notify-send"):
        subprocess.run(["notify-send", "-a", "linux-tts", "PPA linux-tts: rebuild needed",
                        "\n".join(todo)])
    return 1 if todo else 0


if __name__ == "__main__":
    sys.exit(main())
