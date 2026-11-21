# Quirks of the originals and our fixes

The C port reproduces the originals exactly in compat mode (used against the goldens). Every
bug found in the originals is listed here, shown to the user and fixed behind a flag; the
normal mode has the fixes on. Status: **fixed** (flag, in C), **ask** (needs the user's
judgement), **later** (fix belongs to a pass not ported yet).

Examples marked "core" are from the oracle output `tests/golden/core.jsonl.zst`.

## num2words lang_RU (`num2words/`, flags `N2W_FIX_*`)

| # | Bug | Example (num2words → fixed) | Seen in rn | Status |
|---|-----|------------------------------|------------|--------|
| N1 | The numeral before тысяча/миллион takes the caller's gender, animacy and number instead of agreeing with the noun тысяча (f) / миллион (m), inanimate, singular | "взяли трёх тысячи лошадей" → "три тысячи"; "дать четырёх тысячи семьсот пять рублев" → "четыре тысячи"; "двести девяносто одна миллион" → "один миллион"; "одного миллион" → "один миллион"; plural "двадцать одни тысяча" → "одна тысяча" | core: 11 × accusative, 3 × "одна миллион" | fixed, `N2W_FIX_MAGNITUDE` |
| N2 | Floats: "-0.5" loses the minus; negative numbers get "целых" after "одна" (Python's % of a negative number) | "-0.5" "ноль целых пять десятых" → "минус ноль целых…"; "-1.5" "минус одна целых" → "минус одна целая" | no (rn reads decimals itself) | fixed, `N2W_FIX_FLOAT_SIGN` |
| N3 | Compound numerals ending in 2, 3, 4 in the animate accusative take the genitive form; the norm keeps the nominative form (gramota.ru 310247: "оправдали двадцать два российских биатлониста"; "убили тридцать двух человек" is colloquial) | "двадцать двух" → "двадцать два", "сто двух" → "сто два"; "двух" alone stays | not in core | fixed, `N2W_FIX_COMPOUND_ACCS` |
| N4 | 1000 and 1 000 000 at the start: always "одна тысяча", "один миллион" | "Михаил (одна тысяча триста пять)" → "(тысяча триста пять)"; "милостыни одна тысяча рублей" → "тысяча рублей"; "один миллион восемьдесят девять тысяч" → "миллион…" (ordinals already say "тысяча двухсотый") | core: 229 "одна тысяча", 48 "один миллион" | fixed, `N2W_FIX_LEADING_ONE` |
| N5 | Floats ignore case, gender and animacy (always nominative) | "с 2.5" → "две целых пять десятых" | no | later, if rn ever needs it |
| N6 | Plural cardinals are the collective numerals for 2..10 ("двое", "трое") | plural 2 → "двое" | no (rn passes plural only to ordinals) | kept, documented |

## ru_normalizr (to fix when the pass is ported)

| # | Bug | Example (core) | Status |
|---|-----|----------------|--------|
| R1 | Years after a comma read as accusative cardinals | "Гретри, 1742, 1813" → "одну тысячу семьсот сорок двух, одну тысячу восемьсот тринадцать" | later |
| R2 | Number after "равно" stays nominative | "равно 1024" → "равно одна тысяча двадцать четыре" (should be dative "тысяче двадцати четырём") | later |
| R3 | Date case after "сейчас" depends on обособление (memory: open case from the user) | "Сейчас 01.06.2026г." → "первого июня" | later, design first |
| R4 | "с" stays "с" before numerals that need "со" | "на с. 108–113" → "на страницах с сто восьмой по сто тринадцатую" → "со сто восьмой"; "с второй" → "со второй", "с ста" → "со ста". Own pass `rn_run_so` (rn-so.c) at the end, only before сто/ста/втор…/стотысячн…; "с стороны" of old texts stays | core 2 | fixed, `RN_FIX_SO` |
| R5 | Roman ranges with the noun first ("вв. XIV–XV"): the output lowers the written word instead of the inflected noun, so the abbreviation stays unread; the series path reads it | "Памятники вв. XIV–XV" → "четырнадцатого — пятнадцатого вв." → fixed "…пятнадцатого веков"; "в. XIV–XV" → "века" (not in core/big) | fixed, `RN_FIX_ROMAN_RANGE_NOUN` (`rn_fixes`) |
| R6 | Time "H:MM" without a governing preposition is read with a comma, as two numbers | "в 14:30" → "в четырнадцать, тридцать" → fixed "в четырнадцать тридцать"; ":00" → "девять ноль-ноль"; "21.12.25, 17:49" (forum stamps, ~5000 in big) | fixed, `RN_FIX_TIME_NO_COMMA` |
| R7 | Seconds are read masculine | "12:00:01" → "…один секунда", "двадцать два секунды" → "одна секунда", "двадцать две секунды" | fixed, `RN_FIX_SECONDS_FEMN` |
| R8 | After с/от/до/после the minutes are genitive but masculine | "с 10:21" → "с десяти часов двадцати одного минуты" → "двадцати одной минуты" | fixed, `RN_FIX_TIME_GENT_FEMN` |
| R9 | After с/от/до/после zero minutes are read out | "с 9:00 до 18:00" (core) → "с девяти часов ноля минут…" → "с девяти часов до восемнадцати часов" | fixed, `RN_FIX_TIME_GENT_ZERO` |
| R10 | A date takes its case only from a preposition inside its own pattern (на/к/ко/перед); за, по and a preposition before a list or a numeric date are ignored → genitive | "за 31 декабря" → "за тридцать первого" → "за тридцать первое"; "по 1 марта" → "по первое"; "к 1 и 2 мая" → "к первому и второму"; "к 01.06.2026" → "к первому июня" (core 11, big 34 changes) | fixed, `RN_FIX_DATE_PREP_CASE` |
| R11 | "Месяц + число" is read as a date before a counted noun when the number ends in 1 (the check gives up for 1, 21 …) | "до мая 21 солдат" → "до мая двадцать первого солдат" → kept "21 солдат" for the numerals stage. Only when a preposition governs the month: "ноября 1 генерал-прокурор предложил" (big) stays a date | fixed, `RN_FIX_COUNTED_ONE` |
| R12 | Years ≥ 10000: the thousands part of the ordinal is inflected too (only the last word of a Russian ordinal changes) | "12345 года" → "двенадцати тысяч триста сорок пятого" → "двенадцать тысяч триста сорок пятого" (probe; not in core/big) | fixed, `RN_FIX_YEAR_HIGH` |
| R13 | A list of years with "гг." at the end of a sentence loses the sentence dot | "в 1758 и 1761 гг. Конференция…" → "…годах Конференция" → "годах. Конференция" (big 9) | fixed, `RN_FIX_YEAR_LIST_DOT` |
| R14 | "гг." glued to the number is read alone and the number stays; "1999г." is not read at all | "в 2000гг. эпоха" → "в 2000годы" → "в двухтысячных годах"; "1999г." → "тысяча девятьсот девяносто девятого года". A space is put between digits and г/гг before the year patterns, so the gram guards (v < 1000, mass context) apply as for "300 г." | fixed, `RN_FIX_YEAR_GLUED` |
| R15 | "с X по Y годы": the singular ordinal keeps the plural word | "с 1999 по 2001 годы" → "по две тысячи первый годы" → "по две тысячи первый год" (core 3) | fixed, `RN_FIX_YEAR_RANGE_WORD` |
| R16 | A year in brackets is read as a cardinal number, unless "под/при" stands two tokens before | "Михаил (1305)" → "(одна тысяча триста пять)", with N4 "(тысяча триста пять)"; ordinal would be "(тысяча триста пятый)"; "(1020)" in a log line is a number | ask |
| R17 | Two small numbers with a dash and no noun after are read as a list | "страницы 5—7", "выпуск 5—7." → "5, 7" (comma = pause); a range would be "с пятой по седьмую" / "пять — семь" | ask |
| R18 | "гг." in a text without any digit is never touched by this pass (the oracle returns early) | "Уважаемые гг. депутаты" stays | later (abbreviations pass) |

## pymorphy3 (`cmorphy3/`)

| # | Bug | Status |
|---|-----|--------|
| M1 | `tag.animacy` of the 4 Inmx tags carrying both anim and inan depends on PYTHONHASHSEED; C takes the first in the tag string | kept (deterministic), documented |
