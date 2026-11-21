#!/usr/bin/python3 -I
"""Turn misaki's English dictionaries (misaki/data/{us,gb}_{gold,silver}.json)
into the sorted tab-separated files en-g2p.c reads:

    word<TAB>phonemes                       one reading
    word<TAB>default<TAB>TAG=ph<TAB>TAG=    readings by part of speech; an
                                            empty one is misaki's null

Lines are sorted bytewise.  The phonemes are checked against misaki's
vocabulary, as misaki's Lexicon does.

    misaki-lexicon.py <misaki-data-dir> <out-dir>
"""
import json
import sys

US_VOCAB = frozenset('AIOWYbdfhijklmnpstuvwzæðŋɑɔəɛɜɡɪɹɾʃʊʌʒʤʧˈˌθᵊᵻʔ')
GB_VOCAB = frozenset('AIQWYabdfhijklmnpstuvwzðŋɑɒɔəɛɜɡɪɹʃʊʌʒʤʧˈˌːθᵊ')


def check(word, ps, vocab):
    bad = [c for c in ps if c not in vocab]
    if bad:
        sys.exit(f'{word}: {ps!r} has {bad}')


def main():
    src, out = sys.argv[1], sys.argv[2]
    for loc, vocab in (('us', US_VOCAB), ('gb', GB_VOCAB)):
        for kind in ('gold', 'silver'):
            with open(f'{src}/{loc}_{kind}.json', encoding='utf-8') as f:
                d = json.load(f)
            lines = []
            for word, v in d.items():
                if any(c in word for c in '\t\n\r') or not word:
                    sys.exit(f'bad key {word!r}')
                if isinstance(v, str):
                    check(word, v, vocab)
                    lines.append(f'{word}\t{v}')
                    continue
                if not isinstance(v, dict) or not isinstance(v.get('DEFAULT'), str):
                    sys.exit(f'{word}: no DEFAULT')
                check(word, v['DEFAULT'], vocab)
                fields = [word, v['DEFAULT']]
                for tag in sorted(t for t in v if t != 'DEFAULT'):
                    ps = v[tag]
                    if ps is not None:
                        check(word, ps, vocab)
                    fields.append(f'{tag}={ps or ""}')
                lines.append('\t'.join(fields))
            lines.sort(key=lambda s: s.split('\t', 1)[0].encode())
            with open(f'{out}/{loc}-{kind}.tsv', 'w', encoding='utf-8', newline='\n') as f:
                f.write('\n'.join(lines) + '\n')


main()
