# Improvements over the originals

What the C port reads better than ru_normalizr 1.3.0 (our fork) and num2words 0.5.14. One
line per change, newest last; the details, flags and corpus counts are in QUIRKS.md under the
same number. All of them are on in the normal mode; flags 0 give the original output.

## Numbers (num2words)
- N1: "трёх тысячи лошадей" → "три тысячи лошадей"; "одного миллион" → "один миллион".
- N2: "-0.5" keeps its minus; "минус одна целых" → "минус одна целая".
- N3: "двадцать двух студентов" (accusative) → "двадцать два студента" (gramota.ru 310247).
- N4: "одна тысяча триста пять" → "тысяча триста пять"; "один миллион…" → "миллион…".

## Roman numerals
- R5: "вв. XIV–XV" → "четырнадцатого — пятнадцатого веков" (the abbreviation was left unread).

## Dates and time
- R6: "в 14:30" → "в четырнадцать тридцать" (no comma between hours and minutes).
- R7: "12:00:01" → "…одна секунда" (seconds are feminine).
- R8: "с 10:21" → "…двадцати одной минуты".
- R9: "с 9:00 до 18:00" → "с девяти часов до восемнадцати часов" (no "ноля минут").
- R10: "за 31 декабря" → "за тридцать первое"; "к 1 и 2 мая" → "к первому и второму".
- R11: "до мая 21 солдат" is not read as a date.

## Years
- R12: "12345 года" → "двенадцать тысяч триста сорок пятого" (not "двенадцати тысяч").
- R13: "в 1758 и 1761 гг. Конференция" keeps the sentence dot after "годах".
- R14: "в 2000гг." → "в двухтысячных годах"; "1999г." is read.
- R15: "с 1999 по 2001 годы" → "…по две тысячи первый год".
- R16: a year in brackets is an ordinal: "Михаил (1305)" → "(тысяча триста пятый)"; after
  этот/тот/следующий… it agrees and the brackets go: "в следующем (1207) году" → "в
  следующем тысяча двести седьмом году".
- R17: "страницы 5—7" → "пять — семь" (was the list "пять, семь").
- R19: "в следующем 1253" → "в следующем тысяча двести пятьдесят третьем".

## Prepositions
- R4: "с сто восьмой" → "со сто восьмой", "с второй" → "со второй".
