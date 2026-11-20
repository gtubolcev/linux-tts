# Russian text normalization for TTS — research notes (2026-11-17)

Sources: 4pda topic 1110815 (local archive `../4pda`, 5542 posts), Habr, GitHub/PyPI/HF,
the T-Bank talk on FST normalization (YouTube 42UUahzVroc, 2024-05-24), papers, and our own
survey (`scripts/norm-test`, 15 sentences through every engine, checked with Vosk ASR).
Telegram group «Распознавание и синтез речи» searched 2026-11-17 (section 3a).

## 1. Terms and the problem

- **TN (text normalization)**: the written form becomes the spoken form, for TTS.
  "В 2026 году" → "в две тысячи двадцать шестом году".
- **ITN (inverse normalization)**: the spoken form becomes the written one, for STT output.
  Yandex SpeechKit's page (aistudio.yandex.ru/…/stt/normalization) covers only ITN: numbers,
  dates/time, phones, profanity, punctuation.
- **Spelling/social-media normalization**: "чё → что", "мб → может быть", typos. This is a
  third task, needed before morphological tagging. Feoktistov & Morozov (NSU, IJOIT 13(12),
  2025) published a 2000-sentence corpus (huggingface.co/datasets/ruscorpora/normalization).
  Their finding: MoNoise, the BERT hybrid and mBART/ruT5 all do poorly, and the authors expect
  LLMs to do better. For us this matters only for reading chats and forums aloud, and it is
  out of scope for the first version.
- The Google/Kaggle token classes (Sproat & Jaitly 2016; Kaggle "Text Normalization
  Challenge – Russian", 2017, `ru_train.csv`, 10.6M tokens): PLAIN, PUNCT, CARDINAL,
  ORDINAL, DATE, TIME, DECIMAL, FRACTION, MONEY, MEASURE, TELEPHONE, ELECTRONIC (URL/e-mail),
  LETTERS (acronyms spelt), VERBATIM (symbols). NeMo's Russian grammars have the same classes.

What makes Russian hard:
1. **Case and gender agreement of numbers.** "с 10 по 14", "у 3 берёз", "21 письмо". The
   case comes from the preposition, verb or noun, sometimes several words away.
2. **Cardinal vs ordinal vs year.** "в 1999" versus "1999 человек", "XXI век", "Петр I",
   "глава IV".
3. **Ambiguous abbreviations.** "г." means год or город; "т." means том or тонна; "стр."
   means страница or строение; "кв." means квартира or квадратный.
4. **Acronyms.** Some are read as words (ЛОМО, ООН, НАТО), some spelt (КГБ, МВД, США →
   "сэ-шэ-а"), some are mixed or English (API, HTTP, JSON).
5. **Latin-script words.** Brands and terms need a transcription to Cyrillic: Linux →
   "линукс", GitHub → "гитхаб". The vocabulary is open-ended.
6. **Technical tokens.** Versions (0.12.1 is not a decimal), URLs, e-mails, paths, code,
   ranges, "100%-ный", "1999-м", "№", "°C", "км/ч".
7. **Next stages that depend on TN output.** ё-restoration, stress, homographs
   (замо́к/за́мок, verb homographs are mostly unsolvable). They need the final words, so
   they run *after* TN. Stress marks ("+", U+0301) break normalizers.
8. **CAPS text.** Some engines (F5 per 4pda) read upper-case words as acronyms.

## 2. Approaches

| Approach | Strengths | Weaknesses |
|---|---|---|
| Regex + dictionaries (Demagog/Balabolka .dic/.rex, rutextnorm) | fast, predictable, user-editable | no context, so case agreement breaks |
| Rules + morphology (pymorphy3, Natasha; ru-normalizr, dicrector) | case agreement through `make_agree_with_number` and parses; ~2 s per book | Python dependency; still heuristic |
| WFST grammars (pynini/OpenFst; NeMo, Sparrowhawk, RHVoice's own *.fst, T-Bank) | fast at runtime, declarative, can return several candidates | grammars need linguists; slow builds if written loosely |
| Seq2seq neural (snakers4 2020, saarus72 FRED-T5, RUNorm T5) | context-aware | slow (RUNorm: 140–700 s per book), hallucinates and "eats words" (4pda: StUser, azimutspb) |
| LLM prompts (LLMnorm-GUI with gemma-4-26B, PolyNorm arXiv 2511.03080) | best context handling for books | GPU or cloud, minutes per chapter, needs a fallback; not for a screen reader |
| Hybrid: rules/FST produce candidates, a model picks one (T-Bank; rutextnorm's `flag_uncertain` router) | combines both | more parts |

The consensus everywhere is a hybrid: the T-Bank talk, Habr 1004144, Habr 679838 and SberDevices
in Habr 850118 all say "rules plus ML". Tinkoff VoiceKit (Habr 474782, 2019) used a neural
front-end in production.

### T-Bank FST architecture (talk by Vladimir Markov, channel «Желтый AI», 2024)
1. Tokenize into non-overlapping tagged tokens: time, integer, text, with POS and the
   required case.
2. A number gets a *superposition* of all its readings (every case and gender, cardinal or
   ordinal).
3. Agreement rules prune the candidates: preposition → case, number ↔ noun.
4. Verbalize, then strip the tags.
5. If more than one candidate is left, a small NN or LM picks one.
Tools: pynini (`cross`, `union`, `cdrewrite` with sigma-star, `compose`, `optimize`).
Lesson from the talk: their grammars matched too broadly and the build took 10 h and 50 GB.
Rules need context boundaries.

## 3. Solutions, by date

| Year | What | Who | Licence | Notes / usable for us? |
|---|---|---|---|---|
| 2005– | Balabolka + Demagog dictionaries (.dic, .rex) | Ilya Morozov; Demagog community | freeware, closed | De facto standard on 4pda (~370 posts each). Windows only. **The file formats are worth supporting**: users already have large dictionaries. |
| 2016 | Sparrowhawk (Kestrel's open runtime) | Google | Apache-2.0 | Runs FAR grammars from C++. Dormant since 2018. |
| 2017 | Kaggle RU TN dataset + top solutions (fairseq conv seq2seq, rules) | Google / Kaggle; Habr 679838 (Unistar, 2022) describes one | data CC-BY-SA-like | A test set. Many tokens are trivially PLAIN. |
| 2018– | RHVoice built-in Russian TN (`numbers.fst`, `untranslit.fst`, `english_phone_mapping.fst`, `rulex_*.fst`) | Olga Yakovleva | GPL-2 (LGPL lib) | Best Latin reading in our survey. Not reusable outside RHVoice. |
| 2020 | snakers4/russian_stt_text_normalization (Habr 491260): rules + tiny seq2seq, TorchScript | Silero (Islanna) | **GPL-3.0** | Built for STT data. Weak per 4pda; last commit 2021. Licence not compatible with our MIT code, so at most a separate process. **Skip.** |
| 2021– | NeMo-text-processing, `text_normalization/ru` (cardinal, ordinal, date, time, decimal, money, measure, telephone, electronic, whitelist) | NVIDIA | Apache-2.0 | The best-structured open FST grammars for Russian; active. Export to FAR, then run from C++ (Sparrowhawk-like) with no Python at runtime. **Candidate for an FST core.** |
| 2022–23 | saarus72 russian_text_normalizer (FRED-T5-large) + ficbook_text_norm dataset | saarus72 | Apache-2.0 | Too slow, and it drops words (ru-normalizr README, StUser). Useful only for its data. |
| 2023 | dicrector: Demagog-compatible dic/rex corrector with `@function` replacements, pymorphy agreement, plugins (ruaccent) | Lecron (4pda) | **no licence** | Best ideas: agreement rules, Roman numerals, acronym → sounds, Natasha for homographs. The code can't be used until the author adds a licence; we could ask. |
| 2023–26 | RUAccent (stress + homographs; COLING 2025) | Den4ikAI | MIT (code and models) | The 4pda standard after normalization. Next stage, not TN. |
| 2024 | RUNorm (FRED-T5-95M / ruT5-base / FRED-T5-large + tagger) | Den4ikAI | Apache-2.0 | 4pda: slow, "портит текст". Skip as the main path. |
| 2024 | T-Bank FST talk (above) | T-Bank TTS team | slides/colab on GitHub | Architecture to copy. |
| 2025–26 | silero-stress | Silero | MIT per repo (check the model terms) | Stress and homographs. |
| 2025–26 | rutextnorm 2.1 (formerly russian-tts-normalization): one file, regex only, 0.17 ms per sentence, `flag_uncertain()` router | shigabeev | MIT | 93.7% exact on cleaned Kaggle ru, 98.2% on the non-flagged 91%. Weak on bare-number case, ordinals, measures, money. **Good fast tier.** |
| 2026 | ru-normalizr 1.3 (pymorphy3; modes `safe` / `tts`; Windows GUI) | NickZaitsev (Zaycevv on 4pda) | MIT | Best non-LLM result in our survey and on 4pda. Our gaps: versions read as decimals, "14:30", poor Latin ("линэкс"), "/". The same author published proper-noun stress dictionaries from Wikipedia. **Main candidate today.** |
| 2026 | LLMnorm-GUI: YAML pipeline, regex picks sentences, prompt, batching, fallback to the original | azimutspb (4pda) | ? | For audiobooks with a local gemma-4. No CLI. Its author says rules are a dead end. |
| 2026 | "Norma" | alex_dubov (4pda #3054) | ? | Not checked yet. The same author's test text (#2897) is a good test case. |

Other Habr articles:
- 1004144 (2026): a DIY assistant on RK3588 that uses rules only for speed.
- 767560 (2023): compares stress quality of Yandex, Silero, SberDevices and RUAccent.
- 992892: ruaccent-predictor.
- 660565, comments: Silero drops numbers ("18 мая" → "мая").

There is also a Habr Q&A question, 513019, asking for the same TN material.

Academic work (search: cyberleninka.ru, its JSON API `POST /api/search`
`{"mode":"articles","q":…,"size":…}`):
- Khomitsevich, Rybin et al., "Использование лингвистического анализа для нормализации текста
  и снятия омонимии в системе синтеза русской речи" (Изв. вузов. Приборостроение, 2013;
  STC/ITMO, VitalVoice). Morphological and syntactic analysis resolves TN ambiguity and
  homonymy. This is the classic description of an industrial Russian TTS front-end.
- Cherepanova, "…использование англо-русской практической транскрипции в системе
  русскоязычного синтеза" (2017). English insertions read by practical-transcription rules.
  **Directly relevant to our Latin → Cyrillic stage.**
- Pikalyov, "Разработка автоматической системы трансформации английских вставок в русских
  текстах с применением глубокого обучения" (Донецк, 2019). The same task with a neural model.
- Pikalyov, "Разработка системы нормализации текстовых корпусов" (Проблемы искусственного
  интеллекта 2022 № 2). A hybrid system for ASR language-model corpora:
  - procedural and declarative rules;
  - a neural syntactic parser that supplies the agreement for numbers (it finds the
    syntactically linked noun, preposition or adjective and inflects the numeral);
  - patterns for "5 кг", "5-ый", "10-летний", ranges with "–", decimals by the length of
    the fraction;
  - alphanumeric complexes and abbreviations;
  - ё-fication and й-fication.
  No code is published. The design matches the "parse, then agree" idea.
- Grashchenkov, "Системы автоматической транскрипции русского текста: общая организация и
  решённые и нерешённые проблемы" (2022). An overview of the front-end after TN.
- Solomennik et al., "Оценка качества синтезированной речи" (2013). How to test front-end
  errors.
- Feoktistov & Morozov (NSU, 2025): social-media normalization corpus. See §1; this is not
  TTS TN.
- MSU Lomonosov-2022 abstract on neural ITN.

## 3a. Telegram group «Распознавание и синтез речи» and other runtimes (2026-11-17)

- The group's default advice for Russian is "text → RUNorm → RUAccent → engine" (nshmyrev,
  Den4ik, many). Reported faults of RUNorm: "Улан-Батор" → "Улан- инкубатор", "21 крупнейшей"
  → "двадцать первой", reads НАТО/СМИ letter by letter, phone numbers fail. Den4ik asked for
  contributors in 2025-08; the project needs data cleaning and stability work.
- NeMo ru TN: "с окончаниями там беда" (akbarkhuja, 2026-02). Agrees with our view that its
  grammars need case agreement on top.
- ru-normalizr was reposted by the group channel (2026-03-16) with no reports since.
- LLMnorm-GUI (zelenukhin = azimutspb on 4pda): regex picks the spots, gemma-4 31B rewrites
  only those; pipeline LLMnorm → RuVoice-Stress-Converter → LLMnorm (все/всё). Book mode only.
- Others seen: num2t4ru (numbers to words), rus2num and yargy (ITN, not TN),
  saarus72/russian_text_normalizer (used for numbers and Latin by one vosk-tts user).
- Multilingual rule sets: NeMo-text-processing TN covers ar, de, en, es, fr, hi, hu, hy, it,
  ja, ko, pt, ru, rw, sv, vi, zh (Apache-2.0). WeTextProcessing (wenet, Apache-2.0): zh, en,
  ja, with a C++ runtime (`processor_main`, tagger + verbalizer FSTs on OpenFst); wetext-rs is
  a Rust port. voxnorm (Python): zh, en, ja, ko. num2words (LGPL): numbers only, ~50 languages.
- **sherpa-onnx has a TN hook**: `OfflineTtsConfig.rule_fsts` / `rule_fars` (comma lists),
  applied in order by `kaldifst::TextNormalizer` before the frontend. Shipped only for Chinese
  (matcha-zh: phone.fst, date.fst, number.fst). Any rewrite FST compiled with pynini works,
  so NeMo-derived grammars could be fed to our sherpa module without new code, and the same
  kaldifst runtime (Apache-2.0, small, header-mostly) can serve the other modules in common/.

### saarus72 in detail (2026-11-18)

- Model `saarus72/russian_text_normalizer`: FRED-T5-large (820M, F32), Apache-2.0. Input
  marks every number or Latin span: `<SC1>Было у отца [3]<extra_id_0> сына … за [142 990]<extra_id_1> руб.`;
  the model outputs only the spoken form per sentinel, so the rest of the text can't be
  "eaten". It inflects numbers by context; Latin was added from the Kaggle Russian TN data
  and made numbers worse (the number-only version is commit 8c2476b). Own examples of
  faults: "одни [5]" → "пятые", phone numbers read as one big number.
- How the data was made — the useful part: take a big Russian corpus (ficbook, librusec,
  pikabu), find sentences with spelled-out numbers by regex, run ITN (a fixed
  flockentanz/word_to_number_ru) to get the digit form, clean by hand. Every pair is then a
  real sentence with the correct case of the number, written by a human. LLM labelling
  was tried and dropped (too many silent errors).
- Dataset `saarus72/ficbook_text_norm` (HF, card says Apache-2.0; the texts themselves are
  ficbook authors'): 3.93 M sentences, 1.9 GB JSONL, each a list of
  `{text_from, text_to}` spans ("5" → "пять"). **Use: a large reference set for numbers in
  context** (case agreement), scored per span; far beyond our hand-written tests. Not for
  redistribution in our repo; download in the test container only.
- Repo github.com/saarus72/text_normalization (Apache-2.0): notebooks for the pipeline
  above and an ITN comparison (NeMo, Oknolaz, SergeyShk, averkij, flockentanz). No metrics.
- For us: the model itself is too slow for the screen-reader path (820M seq2seq), but the
  "mark spans, let a model only inflect them" design is the right shape for an optional
  book mode, and the dataset can train or test a small case-agreement component.

### Hugging Face search "normalization/normalisation/normalizer" (2026-11-18)

Most hits are unrelated (audio loudness, legal data, spelling of social-media text). TTS-relevant:

| item | what | licence | note |
|---|---|---|---|
| intx82/byt5-textnorm-ru (2026-09) | ByT5-small (~300M) TN for TTS: numbers, dates, time, units, %, fractions, math | Apache-2.0 | newest; 78% exact / 89.6% "semantic valid" on own test; 92% on numeric-heavy math texts; data rule-generated + model-reviewed; byte-level, so no tokenizer gaps for symbols |
| saarus72/russian_text_normalizer | FRED-T5-large, see above | Apache-2.0 | 12 likes, most known |
| maximxls/text-normalization-ru-terrible | tiny T5 from scratch on Kaggle RU TN | MIT | author: "very much unreliable"; useful only as a size floor |
| alexue4/text-normalization-ru-new | rut5-small fine-tune, Kaggle + custom | MIT | no card details |
| CrabInHoney/t5-ru-text-normalization-v1 | spelling/spacing fixes | Apache-2.0 | not TN for speech |
| saarus72/ficbook_text_norm, pikabu_text_norm | 3.9 M / 1–10 M sentence pairs (ITN-made), spans `text_from→text_to` | Apache-2.0 card | best large test set for numbers in context |
| kenenbek/gemma-russian-normalization-dataset | 597 k chat-format pairs, no card | none stated | licence unknown — skip |
| ruscorpora/normalization | 1930 social-media spelling pairs | CC-BY-NC-SA | not TN for speech |
| skypro1111/uk-text-normalization | **Ukrainian** TTS TN pairs: numbers, dates, money, units, abbreviations, phones, IBAN, domains, Roman, Latin; separate test sets for agreement and transliteration | CC-BY-4.0 | ready test set for uk; Space speech-uk/text-normalization and Yehor/normalize-text-uk are uk normalizers |
| Folx/byt5-small-pl-text-normalization, Folx/qwen3-0.6b-pl-… | Polish TN | gated | — |
| others | Turkish, Kyrgyz, Bambara, Hindi, Malay, Khmer, Vietnamese datasets/models | various | not our languages now |

Conclusion: no rule-based Russian TN on HF; the neural ones are all T5-family. intx82/byt5 is the
best candidate to add to the benchmark next to RUNorm (similar size, TTS-oriented, Apache).
For Ukrainian there is a real dataset and a normalizer to look at later.

### Benchmark on our test set (2026-11-18, scripts/normbench)

47 auto-scored rows (+8 judged by hand: acronyms, Latin, versions, web, phone), on mikhail in
docker with 3 CPUs. Exact = after lowercase/ё/punctuation folding; any listed variant counts.

| class | rn | nemo | runorm | byt5 | saarus |
|---|---|---|---|---|---|
| abbrev | 3/4 | 0/4 | 1/4 | 0/4 | 0/4 |
| acronyms | 1/1 | 1/1 | 0/1 | 1/1 | 1/1 |
| cases | 9/13 | 0/13 | 12/13 | 6/13 | 11/13 |
| dates | 3/3 | 0/3 | 3/3 | 1/3 | 2/3 |
| money | 4/5 | 0/5 | 2/5 | 5/5 | 0/5 |
| names | 1/1 | 1/1 | 1/1 | 1/1 | 1/1 |
| numbers | 4/4 | 0/4 | 3/4 | 1/4 | 1/4 |
| ordinals | 3/3 | 0/3 | 3/3 | 1/3 | 0/3 |
| roman | 3/3 | 0/3 | 0/3 | 0/3 | 3/3 |
| time | 1/2 | 0/2 | 2/2 | 2/2 | 1/2 |
| units | 5/5 | 1/5 | 2/5 | 4/5 | 0/5 |
| years | 3/3 | 0/3 | 2/3 | 1/3 | 2/3 |
| **total exact** | **40/47** | **3/47** | **31/47** | **23/47** | **22/47** |
| best of alternatives | — | 27/47 | — | — | — |
| WER | 6.2% | 44.4% | 18.0% | 20.6% | 21.8% |
| ms/phrase median (max) | 1.7 (178) | 36.1 (325) | 671.8 (2102) | 3593.9 (15734) | 2225.8 (13526) |
| load s | 0.2 | 74.0 | 48.0 | 106.1 | 280.6 |
| RSS MB | 71 | 1814 | 728 | 1524 | 3488 |


Notes:
- NeMo-ru only has a non-deterministic mode (meant to pick by audio); "first" option is
  arbitrary, the best of up to 50 options hits 27/47; many cases stay as digits. Not usable as is.
- RUNorm crashes on Latin/Roman with transformers 5 (`token_type_ids`), so its Latin is untested;
  otherwise best on case agreement (12/13), weak on %, units, abbreviations, acronyms letter by letter.
- byt5-textnorm-ru: 3.6 s median per phrase, loops ("данные ООН и минус данные ООН…"), garbage
  on Latin/URLs, wrong Roman numerals. Rejected.
- saarus72 was given only number/Latin spans by our wrapper, so units/abbreviations/% stay
  unexpanded by design and ordinal suffixes leak ("втором-м") — a wrapper limit. Its Latin
  transliteration is the best of all (линакс, файерфокс, джитхаб); 3.5 GB RAM, 2.2 s per phrase.
- ru-normalizr: best overall (40/47, WER 6%), 1.7 ms per phrase, 71 MB. Its faults are
  case agreement after "нет/около/представители" (genitive), time "ноля минут", "Цена 99 руб."
  → genitive, and the Latin reader (линэкс, джисзюб). Plan: keep ru-normalizr as the base,
  fix genitive agreement, take Latin transliteration from a dictionary; neural models only
  for an optional book mode.

### ru-normalizr vs RUNorm row by row; combining them (idea, later)

RUNorm better (5): genitive agreement after нет/около/представители ("двадцати двух писем",
"двадцати одной крупнейшей"), "около тысячи" (rn: "одной тысячи"), time "9:00" (rn: "часов
ноля минут"), "Цена 99 руб." (rn wrongly genitive). ru-normalizr better (14): Roman numerals,
%, currency symbols, units (ГБ, °C, "1 км"), abbreviations (т. е., д.), formulas, year
ranges, acronyms read as words (НАТО). 26 rows both right, 2 both wrong.

So RUNorm wins only on inflection, ru-normalizr on everything rule-shaped. Combination
(user's idea, 2026-11-18, for later): ru-normalizr does all expansion and marks number spans;
only those spans plus their neighbour words go to a small model that picks the case form
(RUNorm/saarus72 design, or a light classifier trained on saarus72/ficbook_text_norm).
Cheaper alternative first: fix rn's agreement rules (governing prepositions/verbs + pymorphy
government of the next noun) and measure on the ficbook pairs.

### Telegram group, 2026-11-16..18

- Debate rules vs LLM. Zelenukhin (LLMnorm-GUI author): LLM normalizes best; uses gemma-4 31B
  (or 26B MoE on 6 GB VRAM) through llama.cpp; LLMnorm-GUI picks only matching sentences by
  regex and sends just those to the LLM (yaml pipelines). saarus72: rules are faster,
  deterministic, easy to change; LLMs still hallucinate in TN (confirmed by another member);
  TTS + LLM on one GPU means swapping models. Agreed middle ground (AigizK): rules cover ~95%;
  write a golden set and let a coding agent grow/optimize the rules against it.
  For us: rules in the screen-reader path, LLM never on the default path (size, GPU, determinism).
- RuVoice-Stress-Converter 0.3 (Windows portable, from kost-t-human/ruvoice-tts) contains its
  own normalizer. ruvoice-tts `text/` (Kotlin): Normalizer, Abbrev, Latin, Uk/Bel layers;
  case agreement by preposition + noun from a morph table built from the AOT dictionary
  (`tools/aot_morph.py` → `assets/morph.bin`: gender/number/case of nouns and adjectives).
  "около 500 рублей" → "пятисот", "с 500 рублями" → "пятьюстами", "2 ч. л." → "две чайные ложки".
  Uses ru-normalizr's tests as its test corpus. **Code licence CC BY-NC-SA 4.0 — not usable
  in our free PPA**; take only the idea (compact morph table instead of full pymorphy) and
  the AOT data (LGPL). Also a candidate for the benchmark (runs on JVM; later).
- Inverse-TN question (rus2num/yargy turning "на втором месте" into digits) — not our task.

## 4. Our engines today (norm-test, 2026-11-17)

No engine handles TN fully:
- **Numbers** are read without case agreement ("в две тысячи двадцать шесть году", "у три
  берёз").
- **Dates and percents** come out wrong: "17.11.2026" → "семнадцать одиннадцать…", "3,5%"
  → "три пять".
- **Units and abbreviations** break everywhere. Silero expands some, and some wrongly
  ("г." → "год", "стр." → "строение").
- **Roman numerals** are read only by RHVoice.
- **URLs and e-mails** are read badly by every engine.

| Engine | Numbers | Latin | Notes for the profile |
|---|---|---|---|
| RHVoice | own TN | best ("линакс", "блутуз") | send almost raw text; only user dictionaries and agreement fixes |
| espeak-ng | own rules | English rules | as RHVoice |
| Piper (via espeak-ng phonemes) | espeak-ng rules, no case | garbage | needs full TN and Latin → Cyrillic |
| Kokoro | espeak-ng | partly | full TN; stress from `common/ru-stress.c` |
| Supertonic, MMS | none | none | full TN; MMS: lower case, letters only |
| TeraTTS | `num2words` style (`common/num-words.c`, nominative only) | ? (listen) | full TN; "+" stress marks |
| Vosk | `vosk.c` drops digits | dropped | full TN, Latin → Cyrillic |
| Silero | partial, own abbreviations | dropped | full TN; its own stress model; "+" marks accepted |

ru-normalizr `--mode tts` fixed most of the list: cases, millions, dates ("семнадцатого
ноября"), percents, currencies, ordinals, abbreviations, units, Roman numerals.

speech-dispatcher also has a symbol preprocessor. `SymbolsPreproc` in speechd.conf with
`locale/ru/symbols.dic`, `emojis.dic` and others names punctuation and emoji per punctuation
level. It runs *before* the module, so our normalizer must not double-handle symbols that
speechd has already spoken.

### End-to-end with the normalizer in the modules (2026-11-19, mikhail)

All five modules (resolute debs of 4f995b0 + speech-dispatcher-ru-normalizr) in a private
speechd in docker, texts-ru.txt with `TextNormalization yes` and `no`, ASR with
vosk-model-small-ru-0.22 (scripts/norm-test/docker-e2e.sh). Every message ended; with the
normalizer all five read numbers, dates, cases, ordinals, Roman numerals and units in full
(«двадцать одно письмо», «у трёх берёз», «семнадцатого ноября две тысячи двадцать шестого
года», «в двадцать первом веке»). Without it vosk drops all digits; the others say bare
cardinals («у три берёз», «один и место»).

**speechd damages the text before the module** (sd_vosk.proto.log, punctuation none):
`3,5%` → `3 ,5 `, `руб.,` → `руб ,`, `€20` → ` евро 20`, `км/ч` → `км ч`, `°C` → ` C`,
`−5` → ` минус 5`, `№` → `No`, and `+ = * >` vanish. So percent and the formula's
operators are lost and «евро двадцать» comes out in the wrong order, although the
normalizer handles the raw text right («три целых пять десятых процента», «двадцать евро»,
«два плюс два равно четыре»). Needs: speechd's symbol preprocessing off (or minimal) for
modules that normalize — check how SymbolsPreproc can be set per module/client.

ru-normalizr weaknesses seen here: «14:30» → «четырнадцать, тридцать»; English names
(«линэкс», «фээфокс», «джисзюб», «убуту», «пайсан»); «ООН» → «оо он», «США» → «сэ шэ аа»;
versions read as decimals («семь целых ноль десятых», «три целых десять сотых»);
«GNOME 50» → «пятидесяти»; «/» kept between numbers; «https» → «хтпс».

## 5. Proposal: one normalization module with engine profiles

The user's idea: every engine gets text from one normalizer, and the normalizer knows which
engine receives it.

**Placement.** A library in `common/` (C API), called by every module before synthesis:
`tts_norm(ctx, text, lang, profile)`. Inside the module it sees the final text after
speechd's own symbol pass and before our stress and g2p.

**Switch (user, 2026-11-17).** The settings dialog gets an "Enable text normalization"
check box, because some users care more about speed and memory. When it is off:
- the helper is not started;
- no dictionaries are loaded;
- the text goes to the engine as now: only the engine's own minimal preparation, such as
  the stress marks it needs to work.

It is stored as a module option in the module's .conf, so speechd modules read it without
the dialog. It may later grow into off / fast (C rules only) / full (the morphology helper);
the check box comes first.

**Pipeline (each stage switchable by profile):**
1. Segmentation: sentences and tokens; keep SSML and index marks intact.
2. User dictionaries in Demagog/Balabolka `.dic`/`.rex` format (global, per language, per
   voice). Runs first so the user can always override.
3. TN core: numbers with case agreement, dates/time, money, measures, Roman numerals,
   ranges, versions, phone numbers, URL and e-mail, symbols.
4. Abbreviations and acronyms: an expansion list plus "spell or read as a word" rules.
5. Latin → Cyrillic: dictionary first (brands, IT terms), then an English g2p → Russian
   phoneme mapping (CMUdict or espeak-ng en, the way RHVoice's `english_phone_mapping` does
   it), letter names for unknown short tokens.
6. CAPS normalization for engines that read capitals as acronyms.
7. ё-restoration and stress/homographs (`common/ru-stress.c` now; RUAccent or silero-stress
   later), in the mark format the engine wants.
8. Output formatting for the engine.

**Profile fields (per engine/voice):** which classes the engine already handles (numbers,
Latin, Roman, abbreviations); stress mark format (`+` before the vowel, U+0301, none);
whether it needs ё; case (MMS: lower only); the alphabet the engine accepts (drop or spell
the rest); max chunk length; whether SSML passes through. Examples:
- **RHVoice/espeak:** stages 2 and 3-agreement only.
- **Piper/Kokoro/Vosk/TeraTTS:** everything.
- **Silero:** everything but stress.

**Engine of stage 3, in steps:**
- **Step 1, now.** Run ru-normalizr (MIT) as a resident helper process in Python
  (`python3 -I`, JSON lines over a pipe; ~1 ms per sentence once loaded). Fix its gaps with
  our pre- and post-rules: versions, time, Latin and "/" are handled before it sees the text.
  The PPA needs `pymorphy3` and its dictionaries (OpenCorpora data). Check noble and resolute
  packaging.
- **Step 2.** Make a C core for the latency path: screen readers need a few ms per
  utterance and no Python. Either rutextnorm-style rules ported to C plus our
  `num-words.c` grown to cases and genders, or NeMo's ru grammars compiled to FAR and
  run with OpenFst (Apache-2.0, packaged in Debian). FST is the stronger long-term choice,
  matching the T-Bank design: candidates plus agreement pruning.
- **Step 3, optional.** Add a "book mode" for long texts with an LLM or neural pass
  (LLMnorm-style, local GGUF), uncertain sentences only, falling back to the rules. This
  is never on the screen reader path.

**Test set.** Use our `scripts/norm-test/texts-ru.txt`, alex_dubov's text (4pda #2897), a
cleaned Kaggle ru subset (as rutextnorm cleaned it), and the reference outputs of
ru-normalizr and rutextnorm. Run text-only comparisons first (exact match per class), then
audio with Vosk ASR through `norm-test`.
Add the supertonic-android sets (§8): `formats.txt` (61 dates/times/counts in context),
`punctuation.txt`, and for stage 7 `hard.txt` (71 stress cases with a key), «Идиот» ch. 1
(41 scored words) and `yo_orlovy.txt` (69 ё cases). They are GPL-3.0: keep them in `_tmp/`
and record only scores here.

**Rules to add, seen in supertonic-android (§8; ideas only, their code is GPL-3.0).**
Check each against ru-normalizr first; add only what it lacks:
- stage 3: case of a number from the preposition before it (без/до/от → gen., к/по → dat.,
  с/между/перед → ins., о/при → prep.; "с 5 лет" gen.);
- stage 3: gender from the following noun ("21 книга", "1001 имя");
- stage 3: Roman numerals agreeing with the label noun, including lists and spans ("главы с
  III по V", "тома I и II", "XIX, XX века"); Arabic chapter numbers read the same way;
- stage 3: ranges "10-15 человек" → "от десяти до пятнадцати", only when the second number
  is larger (not dates or codes); year spans to the dates rule;
- stage 3: phone numbers by their punctuation; digit groups with spaces, NBSP or U+202F
  joined into one number;
- stage 1: footnote marks dropped ("слово¹", "слово.*", "[1]"), but "км²" and the "* * *"
  separator kept; words hyphenated across a line break joined;
- stage 4: acronyms spelt by letter names when they have no vowel or are on the list, else
  read as a word; with stress (ФСБ → эфэсбэ́, США → сэшэа́);
- stage 5: a short list of Latin words read in Russian before language routing (Wi-Fi,
  SIM, PIN, GIF, MIDI);
- stage 8 (per profile): an ellipsis gives a longer pause than a full stop; brackets read
  as words, as a pause, as a separate phrase, or skipped.

**Open questions.**
- Licences: dicrector (none), silero-stress models, LLMnorm-GUI, "Norma".
- Whether speechd's `SymbolsPreproc` should be off for our modules, since the normalizer
  can say symbols better in context.
- English text: NeMo has English TN too; the same module design applies.

## 6. Reference material to read

Sources of knowledge on TTS markup and pronunciation (user, 2026-11-18), not read yet
except the Azure page:
- Hannes Pirker, ESSLLI 2003 course excerpt (D9c):
  https://www.ofai.at/~hannes.pirker/esslli03/D9c_excerpt.pdf
- Google Assistant SSML (beta), broad vs narrow transcriptions:
  https://developers.google.com/assistant/conversational/ssml-beta#broad_vs_narrow_transcriptions
- ElevenLabs, SSML tags explained: https://elevenlabs.io/blog/ssml-tags-explained

- Microsoft Azure Speech, SSML pronunciation
  (https://learn.microsoft.com/azure/ai-services/speech-service/speech-synthesis-markup-pronunciation;
  user, 2026-11-18). The controls a mature commercial engine gives authors:
  - `<phoneme>` with IPA/SAPI/UPS alphabets;
  - custom lexicons (PLS XML: grapheme → phoneme or alias), also for homographs and
    abbreviations;
  - `<say-as interpret-as=…>`: cardinal, ordinal, date with format, time, telephone,
    currency, address, characters/spell-out, fraction, …;
  - `<sub alias=…>` substitution.
  Use as a checklist for which SSML our modules should honour, how per-voice user
  dictionaries could look (PLS as an exchange format besides Balabolka `.dic`), and which
  say-as types the normalizer needs as entry points.

## 7. Stress stage: dictionary design and plan (2026-11-19)

How it is usually done:
- **Lemma + paradigm** is the authoring form. AOT (Sokirko, LGPL) gives each lemma an
  inflection model and an *accent model* (in effect a Zaliznyak stress scheme); Wiktionary
  (CC-BY-SA) has Zaliznyak indices whose templates generate all stressed forms. The
  Zaliznyak dictionary itself is copyrighted. OpenCorpora (CC-BY-SA) has tags, no stress.
- **All forms in a minimal automaton** is the lookup form: the input is a surface form, so
  paradigms are compiled into a DAWG/FSA over all forms with paradigm/form ids or directly
  stress + tags in the leaves (pymorphy2/3: OpenCorpora paradigms → DAWG). Millions of forms
  fit in a few MB.
- **FST** (HFST/foma): GiellaLT lang-rus and `udar` (Robert Reynolds) put stress with an FST
  and resolve homographs with Constraint Grammar. Licence to check.
- **Neural models** (RUAccent, silero-stress) for out-of-dictionary words and homographs,
  after the dictionary, not instead of it.

Plan for us:
1. Runtime stays a plain form lookup in C (mmap'd sorted array or DAWG), no paradigm engine
   in the modules. Store stress **and** grammatical tags: homographs need the tags
   (замка: genitive of за́мок and of замо́к).
2. Paradigms live in the dictionary build pipeline: complete the forms of lemmas that the
   sources list only partly, guess unknown words by ending (as pymorphy does), give tags.
   Free sources: AOT (stress + paradigms), Wiktionary (Zaliznyak indices), OpenCorpora (tags).
3. Jugger-Naut (`_tmp/4pda-jugger/`, no licence: local only unless the authors agree): the
   user checks `disagree.txt` (2279 JN/Vosk disagreements) to decide the dictionary order;
   then an optional user stress dictionary looked up before Vosk, converted from the `.dic`.
4. Fix: Vosk marks secondary stress with 1 too, ru-stress writes two marks
   (ме́жконфессиона́льными) — keep one.
5. JN `.rex` rules (553, ~300 homographs) as seed pairs "homograph + context" for our own
   homograph stage, not run as regexes.
6. Vote of two sources (from supertonic-android, §8): when two independent sources agree, the
   stress is final. When they disagree, or the word is on a homograph list, use the homograph
   stage, else a fixed order. Their result without an LLM: 61 of 71 hard cases. Homograph and
   ambiguity lists to compare with ours: `ruaccent_omographs.tsv.gz` (19 732 forms),
   `tera_ambiguous_stress.txt` (19 741), `tera_ambiguous_yo.txt` (637). Put ё only where a
   dictionary confirms it.
7. Score every change on their `hard.txt`, «Идиот» ch. 1 and `yo_orlovy.txt` (§8).

### 7a. Stress sources and cross-checking (2026-11-19)

Free sources with stress on all word forms, checked:

| Source | Licence | What | Get it |
|---|---|---|---|
| English Wiktionary via kaikki.org | CC-BY-SA | full stressed inflection tables with tags, accent class (a, b, …) | `kaikki.org/dictionary/Russian/kaikki.org-dictionary-Russian.jsonl` (950 MB) |
| Russian Wiktionary via kaikki.org | CC-BY-SA | full stressed tables, IPA, homograph categories | `kaikki.org/ruwiktionary/raw-wiktextract-data.jsonl.gz` (285 MB, all languages; filter lang_code ru) |
| Koziev NLP_Datasets `Stress/all_accents.zip` | CC0 | 2.9M forms from Wikipedia/Wiktionary, forms generated assuming fixed stress: wrong for mobile stress (ре́ки/реки́) | GitHub |
| OpenRussian (Badestrand/russian-dictionary) | CC-BY-SA-4.0 | ~30k lemmas, paradigms with `'` after the stressed vowel | GitHub CSV |
| AOT (sokirko74/aot) | LGPL-2.1 | Zaliznyak-based paradigms + accent models; 4pda users trust it over neural accentuators | repo; dictionary files to locate |
| udar, GiellaLT lang-rus | GPL-3.0 | FST with stress + CG homographs | GitHub |
| rulex (poretsky) | GPL-2.0 | ru_tts pronunciation dictionary, ~60k entries + rules | GitHub |
| RUAccent, silero-stress | MIT | neural; RUAccent dictionaries on HF (`ruaccent/accentuator`) | HF/PyPI |

Not usable: Zaliznyak / Hagen orthoepic full-form lists (copyright; circulated on TTS forums),
НКРЯ accent subcorpus (no download), gramota.ru / accentonline (lookup only).

Cross-check of the 2279 JN/Vosk disagreements (`_tmp/4pda-jugger/xcheck.py`, output
`xcheck.tsv`, run on mikhail, data in ~/claude-builds/stressdict): Wiktionary (en or ru) sides
with JN in 1300, with Vosk in 208, has both variants in 280 (homographs, names, variants),
en and ru disagree in 11, missing in 480 (names, slang). With Koziev and OpenRussian added:
JN 1401, Vosk 316, conflict 261. The user's own marks (38) agree: 37 JN, 1 Vosk.
→ JN before Vosk; Wiktionary as the referee and as the main free full-form source.

## 8. davnozdu/supertonic-android ("MyTTS", read 2026-11-20)

github.com/davnozdu/supertonic-android: an Android TTS engine for Russian. Engines: Supertonic 3,
TeraTTS v2, Silero, Kokoro-RU, Shtorm PocketTTS RU. Code: Kotlin plus Rust on ONNX Runtime.
**The code is GPL-3.0, so we take ideas, rules and test cases, not code.**
What was taken and where it went: normalization rules and test sets → §5; stress vote, lists
and test sets → §7 items 6–7; voices and engine settings → xfce4-speech-settings/PLAN.md
("Ideas from supertonic-android"). The rest of this section is the source summary. A local clone of
commit 7d55869 is in `_tmp/web/supertonic-android/src`.

Normalization (offline path, `app/src/main/java/.../tts/utils/`):
- `RussianBookNormalizer.kt`:
  - case from the preceding preposition (без/до/от → gen., к/по → dat., с/между → ins., о/при →
    prep.), e.g. "без 25 записей", "к 234", "с 5 кг";
  - gender agreement with a list of following nouns: "21 книга", "1001 имя";
  - Roman numerals agreeing with the label noun ("главы с III по V" → "с третьей по пятую");
  - number ranges "10-15 человек" → "от десяти до пятнадцати", while year spans go to the dates
    stage;
  - digit groups joined ("1 500 км");
  - phone numbers read in groups;
  - footnote marks removed ("молоко¹", "молоко.*", "[1]"), but "км²" and the "* * *" separator
    kept;
  - words hyphenated across a line break joined;
  - acronyms spelled by letter names unless they are in a "read as a word" list.
  Quality is mixed. Their own test expects "Я прошёл одна тысяча пятьсот километров" (should be
  "тысячу пятьсот"). ru-normalizr is ahead on morphology.
- `TextNormalizer.kt`, `CurrencyNormalizer.kt`, `RussianNumberNormalizer.kt` cover currency,
  measurements, percentages, decimals ("3,14" → "три целых четырнадцать сотых") and fractions
  ("1/3" → "одна третья").
- Prosody tweaks (release v5.0.5): an ellipsis gives a longer pause than a full stop; brackets
  are read as words, as a pause, as a separate phrase, or skipped; Latin words such as SIM,
  PIN and Wi-Fi are transliterated before language routing.

Stress (`docs/stress-vote.md`):
- Two opinions per word: the RUAccent dictionary (3.26 M forms) and Silero Stress. If they agree,
  that stress is final. If they disagree, or the word is a RUAccent homograph (19 732 forms,
  `assets/ruaccent_omographs.tsv.gz`), an LLM decides. Each question is asked twice with the
  options swapped, and the answer counts only if both agree.
- Without an LLM: RUAccent decides a disagreement, and Silero decides a homograph. This scored
  61/71 on their hard set (70/71 with a cloud LLM; a local Gemma did worse than no LLM).
- ё is taken only from a dictionary confirmation, because LLMs add false ё ("телёфон").
- Assets: `tera_ambiguous_stress.txt` (19 741 forms) and `tera_ambiguous_yo.txt` (637) list
  where stress or ё is ambiguous. Useful for the stress-review page and for marking forms where
  the user's decision is needed.

**Test corpora** (`tools/stress_tests/`; for local evaluation only, not to commit here):
- `hard.txt`: 71 hard stress cases, with a key in `hard_stress.py`;
- `idiot_ch1.txt`: 41 scored words from chapter 1 of «Идиот», with a key in `ch1_key.py`;
- `formats.txt`: 61 lines of dates, times, counts and ranges in context;
- `punctuation.txt`;
- `yo_orlovy.txt`: ё restoration, 69 cases, with a key.

These go into the norm-test set for the profile normalizer (stage 3 of the work order).

Other:
- Supertonic voices can be imported as `style_ttl.npy` + `style_dp.npy`, or as a `.json`.
- The release shtorm-designed-voices-v1 has 206 synthetic Russian reference voices for Shtorm
  PocketTTS RU (Qwen3-TTS VoiceDesign, 24 kHz MP3). They could serve as Pocket TTS voices in
  sd_sherpa once the RU fine-tune runs through sherpa's Pocket path. That needs a check: the
  model export (`tools/export_shtorm.py`) and the licence of the reference packs.
- A Russian int8 model with 6 flow steps instead of 8 (about a third of the CPU load). The idea
  to try for Supertonic in sherpa: a NumSteps setting.
- Stress dictionary releases (v3.1.1): russian_accents_{full,compact}.json, in the form
  `{"москва": "москва́"}`, built from RUAccent (MIT, but derived from Zaliznyak's dictionary).

## 9. Other languages: GitHub and web survey (2026-11-20)

Searched GitHub (repos and topics: text normalization/normalisation, inverse TN, num2words,
number-to-words, tts frontend, names in ru/pl/de/fr/es) and the web. Stars and dates as of
today. Our licence is GPL-3.0-or-later now, so GPL/LGPL/Apache/MIT sources can all be used.

### Multilingual, written direction (what TTS needs)

| Project | Licence | Languages | What it is |
|---|---|---|---|
| NVIDIA/NeMo-text-processing (518★, active) | Apache-2.0 | TN: ar de en es fr hi hu hy it ja ko pt ru rw sv vi zh; ITN also he mr | Pynini WFST grammars, tagger + verbalizer; compiled to .far. Python at runtime, or C++ through Sparrowhawk (google/sparrowhawk, Apache, 2018). The largest free rule set for EU languages. No pl, cs, uk, nl. |
| wenet-e2e/WeTextProcessing (842★) | Apache-2.0 | zh, en, ja | Same idea, own C++ runtime (OpenFst). sherpa-onnx loads such FSTs through `rule_fsts`/`rule_fars` (used by its Chinese models). |
| savoirfairelinux/num2words (973★) | LGPL-2.1 | ~60: am ar az be bn ca cs cy da de en eo es eu fa fi fr he hi hu hy id is it ja kn ko kz lt lv nl no pl pt ro ru sk sl sr sv te tg th tr uk vi zh … | Numbers only: cardinal, ordinal, year, currency. Python. Case/gender for Slavic languages is partial. |
| forzagreen/n2words (113★, active) | MIT | 50 (72 variants) | Same in JS, no deps; easy to port tables to C. |
| rhasspy/gruut (332★, 2024) | MIT | ar ca cs de en es fa fr it lb nl pt ru sv sw | Tokenizer + "text cleaner": numbers, dates, currency via num2words/babel, then G2P. Good reference for the token classes. |
| espeak-ng (in our stack) | GPL-3.0 | 100+ | Numbers, ordinals, some abbreviations in `dictsource/*_list`/`*_rules` for every language it speaks. Now licence-compatible: usable as the fallback number reader for any language without own rules. |
| piercarlo62/fida-normalizer (PyPI, 0.1.x) | MIT | it en fr de es pt nl sv | Young rule-based TTS normalizer: numbers, dates, times, URLs. Not read yet. |
| google/TextNormalizationCoveringGrammars | Apache-2.0 | en, ru | Thrax covering grammars (paper material, not a full normalizer). |
| google/language-resources (2021) | Apache-2.0 | af bn es hi is jv km lo my ne si su ta xh zu … | Per-language verbalizer grammars (Festus/Thrax) from Google's low-resource TTS work. |

### Per language (EU, GB, North America first)

- **en**: NeMo en (the most complete), tomaarsen/TTSTextNormalization (MIT, 2022), PolyNorm
  benchmark (below) for tests.
- **de, fr, it, es, pt, sv, hu**: NeMo; fida-normalizer for de/fr/it/es/pt/nl/sv; num2words.
- **nl, da, no, fi, pl, cs, sk, sl, ro, lt, lv, el, bg, hr**: no TN grammars found; numbers
  only (num2words, n2words, espeak-ng). Polish: `pl_itn` (PyPI) is ITN only (NeMo port);
  paper W19-3703 on Polish number forms. Czech: a Charles University thesis on number
  normalization (dspace.cuni.cz 20.500.11956/183077), no package.
- **uk**: lang-uk/ukrainian-tts-preprocessing (Apache-2.0, 2025; benchmark + data, UNLP 2025
  paper), robinhad/ukrainian-tts `formatter.py` (MIT, numbers and currency), Yehor's
  radtts-uk `numerical.py`; `ukr-itn` is ITN only.
- **ru**: sections 3–7. Also shigabeev/russian_tts_normalization (MIT, regex only, one file),
  snakers4/russian_stt_text_normalization (GPL-3, seq2seq, 2021).
- **ga, gd**: kscanne/caighdean (GPL-3), standardisation rather than TN.
- Outside Europe, for later lists: tr — ozguradmin/ahenk-turkish-tts `trnorm` (Apache, active);
  vi — soe-vinorm (MIT), VietNormalizer (MIT, arXiv 2603.04145); km — seanghay/tha (Apache,
  WFST, arXiv 2609.30984); zh — speechio/chinese_text_normalization (MIT), WeText; ja —
  neologdn (Apache); Indic — indic-num2words (Apache), csebuetnlp/normalizer (bn).

### LLM-based TN

- Apple **PolyNorm** (EMNLP 2025 industry, arXiv 2511.03080): few-shot LLM TN for several
  languages incl. Polish; releases **PolyNorm-Benchmark** with hard classes (version numbers,
  serials, coordinates, tickers, addresses). Useful as a test set for our rules.
- arXiv 2309.13426: GPT few-shot TN ~40 % fewer errors than production rules (large models).
- No results found for sub-1B models; matches our own finding that RUNorm-class small
  seq2seq models are the practical neural option (section 3).

### Conclusions for the normalizer (section 5)

1. For de, en, es, fr, it, pt, sv, hu (and ru as a cross-check) the NeMo grammars are the
   base: compile them to .far once, run them in C++ (Sparrowhawk-style tagger → verbalizer)
   inside our TN module; no Python at runtime. To check: whether sherpa-onnx `rule_fars` can
   take NeMo's two-stage grammars directly (probably not: it applies one rewrite FST).
2. For EU languages without grammars: a small common layer (dates, times, units, currency,
   abbreviations per language) plus number words from num2words/n2words tables or
   espeak-ng's `*_list` data. Polish/Czech/Ukrainian need case agreement like Russian; our
   ru design (section 5) is the template.
3. uk: start from lang-uk/ukrainian-tts-preprocessing and its benchmark.
4. Tests: PolyNorm-Benchmark for en/de/fr/es/it/pt/pl.

## 10. Zyphra/zyphra-textnorm 0.3.1 (tested 2026-11-20)

MIT, Rust; Rust ports of the NeMo r1.2.0 grammars plus the ZONOS3 locale grammars, 43 languages
(en de es fr it pt nl pl uk cs ru …). No FST runtime: the tables are compiled in. Python
(abi3 wheel) and a C ABI (`zyphra_textnorm_new/normalize/free`, static lib, header
`include/zyphra_textnorm.h`). A pre-pass (on by default, `fancy-regex`+`regex`) handles
markdown, emoji, agreement, dates, units. "Never fails": a sentence the grammar fails on comes
back as typed.

normbench (ref-ru.tsv, 47 rows; scripts/normbench, `bench.py zt|ztraw`):

| | ru_normalizr | zt (pre-pass) | zt raw (NeMo parity) |
|---|---|---|---|
| exact | 40/47 | 37/47 | 25/47 |
| WER | 6.2% | 7.1% | 15.2% |
| ms/phrase median (max) | 1.7 (311) | 0.2 (4) | 0.0 |
| load | 1.0 s | 0.01 s | 0 |
| RSS (Python process) | 71 MB | 39 MB | 19 MB |

zt is better at: "С 9:00 до 18:00" (с девяти часов до восемнадцати часов), "99 руб." (nominative),
"14:30" as "четырнадцать часов тридцать минут", acronyms that are words (ООН stays, rn says
"оо он"), and "Дата релиза: 17.11.2026" in the nominative (cf. the обособление case in
memory). zt is worse at:
- Latin words inside Russian are spelled letter by letter ("smartphone" → "эс эм эй ар …", even
  a stray "и эн джи эл ай эс эйч" from a label); rn transliterates. Known words (Linux, GitHub)
  stay Latin, which is fine for engines with their own Latin handling.
- "см." → "сантиметр", "При Петре I" → "ай", "Python 3.10" → "третье октября", "Ubuntu 26.04" →
  "двадцать шестое апреля", "Счёт 3:1" → "одна минута четвертого", "1,5 книги", "К 3-й главе".
- Case after prepositions still fails in the same places as rn (нет 5 рублей, около 1000 км).

Conclusion:
- **Not a replacement for ru_normalizr** in Russian: slightly worse, with worse failure modes
  (letter spelling, dates out of versions).
- **Worth it for the other languages.** We have nothing for de/en/uk/fr/es/pl/cs…, and zt
  covers them in-process from C: no Python helper, ~0.2 ms, no load time. Plan:
  1. Build the static lib in the speech-dispatcher-modules source package. This needs cargo,
     the crates vendored in the orig tarball (the PPA builds offline), and rustc ≥ 1.88:
     rustc-1.88 exists in noble and resolute. Link it into libtts-common.
  2. In tts-norm.c, pick by voice language. ru: ru_normalizr helper as now. Other languages:
     zt in-process. Unknown language: unchanged. Same "enable normalization" switch.
  3. Wrap engine markup (SSML pieces we pass on) as markup tags or verbatim spans.
- Borrow zt's ru rules where they beat rn (time ranges, "руб.", acronym lexicon, nominative
  dates after a colon) as rn fixes. Run them through the native-speaker check first.
- Watch the repo along with ZONOS3-Mini (memory zonos3-watch).

**Decision (user, 2026-11-21):** no zt fallback for Russian. ru_normalizr is to be rewritten
in C instead, which removes the Python helper.

ru_normalizr 1.3.0 (MIT) is 19 135 lines of Python in about 40 files, with roughly 100 regex
calls (60 files use lookbehind or named groups). Its dependencies are pymorphy3 and num2words.
It uses only part of pymorphy3: parse → normal_form and tag.case, gender, number, animacy, plus
inflect(). The pymorphy3-dicts-ru data (OpenCorpora, CC BY-SA) is 16 MB.

Port plan:
1. Oracle and corpus. Python ru_normalizr is the golden generator. The corpus is normbench,
   norm-test, the 4pda topic text, book texts and the rule tables, about 100k sentences. The C
   output must match byte for byte, except for a reviewed list of our deliberate fixes. Like
   zt's goldens, the corpus is .zst in tests and checked in CI.
2. Morphology in C. A build-time converter turns the dictionary into a compact read-only file
   that is mmapped, so the modules share the page cache. Its API is parse(word) → [(lemma,
   tag)] and inflect(word, grammemes), with pymorphy's unknown-word prediction only where rn
   needs it.
3. num2words-ru in C (cardinals and ordinals in every case, gender and number). It is small.
4. Port the rn passes one at a time with PCRE2, in rn's order, keeping the goldens green
   after each pass.
5. tts-norm.c calls it in-process. tts-normalize.py and the helper protocol go once parity
   holds. The "enable normalization" switch stays.

**Per-language normalizers (user, 2026-11-21):** tts-norm.c chooses a normalizer for each voice
language from a table (language → backend). Russian uses the C port of ru_normalizr. The other
languages use zt until something better is found. Candidates are WeTextProcessing for zh and
our own fixes for uk. Each language can be switched, measured and replaced on its own. A
language with no backend gets the text unchanged.
