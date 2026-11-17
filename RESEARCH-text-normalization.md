# Russian text normalization for TTS — research notes (2026-11-17)

Sources: 4pda topic 1110815 (local archive `../4pda`, 5542 posts), Habr, GitHub/PyPI/HF,
the T-Bank talk on FST normalization (YouTube 42UUahzVroc, 2024-05-24), papers, and our own
survey (`scripts/norm-test`, 15 sentences through every engine, checked with Vosk ASR).
Not yet searched: Telegram group «Распознавание и синтез речи» (the MCP was disconnected).

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

**Open questions.**
- Licences: dicrector (none), silero-stress models, LLMnorm-GUI, "Norma".
- Whether speechd's `SymbolsPreproc` should be off for our modules, since the normalizer
  can say symbols better in context.
- English text: NeMo has English TN too; the same module design applies.
