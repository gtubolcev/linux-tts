# Russian stress dictionary

`ru-stress.tsv`, read by `common/ru-stress.c` before the Vosk dictionary.
One line per word, sorted by bytes:

    key<TAB>variant[/flags][,variant[/flags]...]

- key: the word in lower case, ё written е;
- variant: the word with U+0301 after the stressed vowel (ё marks itself);
- flags: `n` proper name, `r` rare reading, `d` default reading.

The first variant is the one taken when the text says nothing more.

## Building

    python3 -I wikt2dict.py DL wikt-stress.tsv
    python3 -I build.py wikt-stress.tsv JN.txt dictionary.sorted verdicts.json \
        disagree.txt ru-stress.tsv ru-stress-jn.tsv freqrnc2011.csv

- `DL/en-ru.jsonl`, `DL/ruwikt.jsonl.gz`: kaikki.org extracts of the English
  and Russian Wiktionary (CC BY-SA 4.0); every form with a stress mark,
  with its lemma, part of speech, proper-name and rare flags.
- `verdicts.json`: our own checks of the words where the sources disagree
  (the review page): `v` the verdict per word, `r` default and rare
  readings. They win over Wiktionary.
- `freqrnc2011.csv`: the lemma frequency list of the Russian National
  Corpus (Lyashevskaya, Sharov 2009, dict.ruslang.ru), used only to order
  the readings of a word; not shipped.
- `dictionary.sorted` (Vosk TTS): its stress decides between ties, and an
  "o" sound on a stressed е drops a Wiktionary spelling without ё.
- JN.txt (Jugger-Naut list) and disagree.txt: local only. The list has no
  licence, so `ru-stress-jn.tsv` (its words missing from Wiktionary) is for
  local use and never packaged.

`ru-stress.tsv` itself is CC BY-SA 4.0, as Wiktionary.
