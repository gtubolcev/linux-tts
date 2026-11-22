# Licensing

This file records where every part of linux-tts comes from, under which licence it is used,
what has to be shipped with it, and the rules for adding new code, models and voices.
State of 2026-11-22 (licence audit). Open problems are listed at the end.

## The repository licence

linux-tts is GPL-3.0-or-later (`LICENSE`). Until 2026-11-20 it was MIT; commit 87e0a71
("Relicense to GPL-3.0-or-later") changed it. At that time all commits were by Grigory
Tuboltsev (with Claude as co-author), here and in the repository the code came from
(gtubolcev/xfce-patches, `speech-settings/modules/`); GitHub lists no other contributors
and no pull requests. Copies published before 2026-11-20 stay available under MIT to those
who got them.

Own files carry

    Copyright (C) 2026 Grigory Tuboltsev
    SPDX-License-Identifier: GPL-3.0-or-later

A file ported from another project also names the upstream, its licence and its copyright
holders (see the table). Most files do not have these lines yet; the list is kept outside
the repository by the audit.

## What the GPL-3.0-or-later repository can take

| Licence of the piece | Copy or port into linux-tts | Link against | Notes |
|---|---|---|---|
| MIT / Expat, BSD-2/3-Clause, ISC, Zlib, Unicode | yes | yes | keep the copyright and permission notice (file header + debian/copyright) |
| Apache-2.0 | yes (into GPL-3) | yes | keep notices, ship upstream NOTICE, mark changed files |
| LGPL-2.1-or-later, GPL-2.0-or-later, GPL-3.0-or-later | yes | yes | |
| GPL-3.0-only (e.g. piper1-gpl) | yes, the result is GPL-3.0-only | yes | avoid where a "+" version exists |
| GPL-2.0-only | no | no | incompatible with GPL-3 |
| MPL-2.0 | only as separate files that stay MPL-2.0 | yes | |
| CC-BY-4.0, CC-BY-SA-3.0/4.0, CC0 (data) | as separate data files | — | attribution; SA data stays under its licence |
| CC-BY-NC*, CC-BY-ND*, OpenRAIL(-M), research-only, custom | never in the repository or a .deb | — | see the distribution rule |

## Distribution rule: PPA, downloader, nowhere

1. **PPA (ppa:gtubolcev/linux-tts) and the repository:** only free licences (DFSG / OSI
   style). Code and data must allow modification and redistribution for any purpose,
   commercial use included. The training data of a model counts: a model is not free if its
   licence or the licence of its training data restricts use (research only,
   non-commercial).
2. **Downloader (xfce4-speech-settings):** non-free models and voices that the user may
   still use privately (NC, ND, OpenRAIL-M, research-only training data). The PPA ships only
   the manifest and the downloader. Before the download the user sees the licence summary
   and full text and must accept it; LICENSE and a SOURCE note (URL, sha256, date, accepted
   licence) are saved next to the files. Free models may use the downloader too, without
   the acceptance step.
3. **Nowhere:** files whose licence does not allow redistribution or private use by others,
   files without any licence (e.g. the JN stress list), and files whose origin is unknown.
   They stay on the developer's machine for tests only.

Upstream source tarballs are distributed too: an orig tarball in the PPA must not contain
non-free files even if no binary package installs them (repack it as `+ds`).

## Inventory: code and data in this repository

Relation: *own* = written here; *port* = rewritten from another language, structure and
data follow the upstream; *copied* = taken verbatim; *generated* = produced by a script in
this repository from upstream data; *consulted* = upstream read for behaviour only.

| Path | Origin (version) | Licence | Holders | Relation | Must ship |
|---|---|---|---|---|---|
| everything not listed below | own | GPL-3.0-or-later | Grigory Tuboltsev | own | — |
| `speechd-module/` | speech-dispatcher 0.12.1 `src/modules` | BSD-2-Clause | 2020-2025 Samuel Thibault | copied | notice in debian/copyright |
| `vosk/vosk.c` | alphacep/vosk-tts (vosk_tts Python package) | Apache-2.0 | Alpha Cephei Inc. | port | notice, "changed" statement |
| `kokoro/ru-g2p.c` | ru_g2p.py of zaakirio/kokoro-ru | Apache-2.0 | zaakirio | port (RUAccent replaced by the Vosk dictionary) | notice |
| `kokoro/en-g2p.c`, `kokoro/misaki/*.tsv` | hexgrad/misaki fba1236 (en.py, us/gb gold/silver) | Apache-2.0 | hexgrad and misaki contributors | port / converted data | notice; dictionary provenance open (misaki issues 107, 113) |
| `kokoro/en-tagger*`, `kokoro/misaki/en-tagger*` | spaCy 3.8, en_core_web_sm 3.8.0, spacy-lookups-data 1.0.5 | Expat | ExplosionAI GmbH | port + converted weights | notice; weights trained on OntoNotes 5 (LDC) |
| `kokoro/espeak-g2p.c` | phonemizer (behaviour of its espeak backend) | GPL-3.0-or-later | Mathieu Bernard, CoML | port | — |
| `kokoro/espeak-ru/*`, `sherpa/espeak-ru/ru_rules_stress` | espeak-ng master 688cb60 (ru_rules, ru_list, ru_emoji) | GPL-3.0-or-later | espeak-ng contributors | copied / modified | notice |
| `teratts/teratts.c` | teratts.py of TeraSpace/TeraTTSv2 | not stated upstream (to check) | TeraSpace | port of the text preparation | — |
| `common/num-words.c` | follows num2words (lang_RU) | GPL-3.0-or-later (own code); upstream LGPL-2.1-or-later | — | consulted / port (to decide) | num2words notice if port |
| `textnorm/rn/` (`rn-*.c/.h`) | NickZaitsev/ru_normalizr 5f3a016 (1.3.0) | MIT | NickZaitsev and contributors | port | MIT notice |
| `textnorm/rn/rn-latdic.h`, `rn-tables-data.h`, `rn-units-data.h`, `rn-patterns.h` | ru_normalizr 5f3a016 data | MIT | NickZaitsev | generated | MIT notice |
| `textnorm/rn/rn-phonemeta.h` | phonenumbers 9.0.43 (libphonenumber metadata) | Apache-2.0 | Google Inc., David Drysdale | generated | notice |
| `textnorm/rn/rn-pyuni.h`, `rn-unidata.h`, `textnorm/cmorphy3/unidata.h` | Python 3.12.3 unicodedata (Unicode 15.0.0) | Unicode-3.0 (Unicode data licence) | Unicode, Inc. | generated | Unicode notice |
| `textnorm/cmorphy3/` | pymorphy3 2.0.6, DAWG-Python (dawg2-python 0.9.0) | MIT | Mikhail Korobov, Danylo Halaiko and contributors | port | MIT notice |
| `textnorm/num2words/` | savoirfairelinux/num2words 0.5.14 lang_RU | LGPL-2.1-or-later (used as GPL-3.0-or-later) | Taro Ogawa, Savoir-faire Linux and contributors | port | keep notice |
| `textnorm/tests/golden/core.jsonl.zst` | ru_normalizr tests and benchmark corpus (Wikisource books), own tests | MIT (rn tests); public domain (books, to check) | NickZaitsev; book authors | generated | — |
| `data/ru-stress/` scripts, `verdicts.json` | own | GPL-3.0-or-later | Grigory Tuboltsev | own | — |
| `ru-stress.tsv` (built, not in git) | Wiktionary via kaikki.org | CC-BY-SA-4.0 | Wiktionary contributors | generated | attribution, licence, SA |
| `firefox/*.patch` | Firefox `dom/media/webspeech/synth/speechd` | MPL-2.0 | Mozilla Foundation and contributors | modified | stays MPL-2.0 |
| `silero/`, `extra/zonos3/` | own (models not included) | GPL-3.0-or-later | Grigory Tuboltsev | own | models only via downloader |
| `scripts/`, `tests/` texts (`texts-ru.txt`, `ref-ru.tsv`, `vosk/texts.txt`) | own test sentences (to confirm) | GPL-3.0-or-later | — | own | — |
| `packaging/*/debian/` | own | per package (MIT, Apache-2.0, GPL-3+) | Grigory Tuboltsev | own | — |
| `packaging/speech-dispatcher/*.patch`, `packaging/rhvoice/patches/*` | own patches to GPL-2+ / LGPL code | upstream licence of the patched file | Grigory Tuboltsev | own | — |

## Inventory: runtime dependencies and PPA packages

| Package (PPA source) | Upstream | Licence | PPA? | Notes |
|---|---|---|---|---|
| speech-dispatcher-modules | this repository | GPL-3.0-or-later + table above | yes | links libspeechd (LGPL-2.1+), onnxruntime (MIT), sherpa-onnx (Apache-2.0), pcre2 (BSD-3-Clause) |
| onnxruntime-tts | microsoft/onnxruntime v1.32.0 fab30a46 (+ mirrored CMake deps) | MIT + Apache-2.0, BSD, BSL-1.0, MPL-2.0 (eigen) | yes | private path (RUNPATH); ThirdPartyNotices.txt installed |
| onnxruntime (noble) | microsoft/onnxruntime 1.23.2 | same | superseded | |
| onnxruntime-ep-vulkan | automataIA/onnx-vulkan-rs 8625ef9 + vendored crates | MIT OR Apache-2.0 (crates: see copyright) | yes | upstream NOTICE must be installed |
| sherpa-onnx-tts | k2-fsa/sherpa-onnx v2.0.1 + deps | Apache-2.0, BSD, Expat, MPL-2.0 | yes | |
| sherpa-onnx (old source) | k2-fsa/sherpa-onnx 2.0.1 | Apache-2.0 | stale | no packaging in this repository |
| piper-tts | OHF-Voice/piper1-gpl v1.8.0 (639388b) + espeak-ng 212928b | GPL-3.0-only, espeak-ng GPL-3.0-or-later | yes | |
| piper-voices | rhasspy/piper-voices (sherpa-onnx conversions) | CC0 / CC-BY-4.0 / public domain as labelled | yes, **under review** | 9 of 12 voices are fine-tuned from lessac (Blizzard 2013, research only) |
| kokoro-tts-model-mul | hexgrad/Kokoro-82M v1.0 via onnx-community | Apache-2.0 | yes | training data not checked |
| speech-dispatcher-ru-normalizr | ru_normalizr 5f3a016 + PyPI deps | MIT, Apache-2.0; dictionary CC-BY-SA-3.0 (OpenCorpora); CMU dict BSD-2-Clause | yes | |
| speech-dispatcher (patched) | Ubuntu speech-dispatcher 0.12.1 / 0.12.0~rc2 | GPL-2+, LGPL-2.1+, BSD-2-Clause | yes | |
| rhvoice (patched) | Ubuntu multiverse rhvoice 1.14.0 / 1.8.0+dfsg | engine LGPL-2.1+/GPL; voices partly CC-BY-NC-ND and custom | yes, **orig tarball not clean** | binaries ship no voices; orig tarballs contain all voices |
| Vosk model vosk-model-tts-ru-0.9-multi | Alpha Cephei (ruBert-base, SberDevices) | Apache-2.0 | allowed (not uploaded) | |
| supertonic-3, TeraTTS v2, kokoro-ru | Supertone / TeraSpace / zaakirio | OpenRAIL-M (variants) | **no** → downloader | use restrictions are not free |
| sherpa-onnx pocket, kitten-nano 0.8 | Kyutai / KittenML | CC-BY-4.0 / Apache-2.0 (to confirm per model) | allowed after check | |
| mms-ru | facebook/mms-tts-rus | CC-BY-NC-4.0 | **no** → downloader | |
| Silero v5_ru | snakers4/silero-models | CC-BY-NC-SA-4.0 | **no** → downloader | executable model: load only by pinned sha256 |
| ZONOS3 | Zyphra | — | no (dropped) | |
| RHVoice voices | RHVoice project | per voice: some free, many CC-BY-NC-ND-4.0, custom | free ones only | downloader for the rest |
| Piper voices with NC or research-only lineage | rhasspy/piper-voices | per model card | **no** → downloader | |

## Licence files next to models

Every model or voice package (and every downloaded model) installs, in its own directory and
in `/usr/share/doc/<package>/`:

- the full upstream licence text (`LICENSE`, plus `NOTICE` for Apache-2.0);
- the model card or README with the training-data statement;
- for CC-BY / CC-BY-SA: the attribution (author, title, source URL, licence URL, changes);
- for downloads: `SOURCE` with URL, sha256, date and the licence the user accepted.

debian/copyright lists the model files with their licence and a `Comment:` on the training
data.

## Checklist: adding a dependency, model or voice

1. Record the exact origin: URL and commit, tag or Hugging Face revision; sha256 of what is
   downloaded.
2. Read the licence file itself (not only the badge or the model card summary). Note the
   SPDX id and the copyright holders.
3. For a model: find the training data and its licence, and the base model it was
   fine-tuned from (and that model's data). Research-only or NC anywhere in the chain makes
   the model non-free.
4. Decide the class with the distribution rule: PPA, downloader or nowhere.
5. Check compatibility with the place it goes: this repository (GPL-3.0-or-later) or
   xfce4-speech-settings (GPL-2.0-or-later: no Apache-2.0, no GPL-3 code).
6. Code that is copied or ported: keep the upstream notice in the file header, add a
   `Files:` stanza to debian/copyright, add a row to the inventory above.
7. Apache-2.0: ship the upstream NOTICE file; mark changed files.
8. Data under CC-BY / CC-BY-SA: keep it a separate file under its own licence; attribution
   in debian/copyright and next to the file.
9. Vendored libraries: private path only (RUNPATH), never Provides/Replaces/Conflicts
   against distro packages.
10. Repack orig tarballs that contain non-free files (`+ds`, `Files-Excluded:`).
11. Names: describe, do not imply endorsement ("for Piper voices", not "official Piper").
12. Voice cloning: only references with consent or a clearly free licence.

## Open problems (2026-11-22)

1. The rhvoice orig tarballs in the PPA (1.14.0 and 1.8.0+dfsg) contain all RHVoice voices,
   including CC-BY-NC-ND-4.0 and custom-licence voices.
2. Nine PPA Piper voices are fine-tuned from the lessac voice, whose data is licensed for
   research only.
3. debian/copyright lacks the textnorm ports (MIT, LGPL-2.1+, Apache-2.0 metadata, Unicode
   data), `sherpa/espeak-ru`, `firefox/` (MPL-2.0) and `common/num-words.c`.
4. Most source files have no SPDX line; textnorm files have no copyright or upstream notice.
5. debian/control recommends model packages that are non-free (OpenRAIL-M).
6. The local build-deb.sh scripts say libspeechd_module is LGPL-2.1+; it is BSD-2-Clause.
