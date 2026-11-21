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
| N3 | Compound numerals ending in 2, 3, 4 in the animate accusative take the genitive form; the norm is the nominative form ("вижу двадцать два студента") | "двадцать двух" → "двадцать два"? | not in core | ask |
| N4 | 1000 and 1 000 000 at the start: always "одна тысяча", "один миллион" | "посадских одна тысяча сто шестьдесят девять" → "тысяча сто шестьдесят девять"? | core: 229 "одна тысяча", 48 "один миллион" | ask |
| N5 | Floats ignore case, gender and animacy (always nominative) | "с 2.5" → "две целых пять десятых" | no | later, if rn ever needs it |
| N6 | Plural cardinals are the collective numerals for 2..10 ("двое", "трое") | plural 2 → "двое" | no (rn passes plural only to ordinals) | kept, documented |

## ru_normalizr (to fix when the pass is ported)

| # | Bug | Example (core) | Status |
|---|-----|----------------|--------|
| R1 | Years after a comma read as accusative cardinals | "Гретри, 1742, 1813" → "одну тысячу семьсот сорок двух, одну тысячу восемьсот тринадцать" | later |
| R2 | Number after "равно" stays nominative | "равно 1024" → "равно одна тысяча двадцать четыре" (should be dative "тысяче двадцати четырём") | later |
| R3 | Date case after "сейчас" depends on обособление (memory: open case from the user) | "Сейчас 01.06.2026г." → "первого июня" | later, design first |

## pymorphy3 (`cmorphy3/`)

| # | Bug | Status |
|---|-----|--------|
| M1 | `tag.animacy` of the 4 Inmx tags carrying both anim and inan depends on PYTHONHASHSEED; C takes the first in the tag string | kept (deterministic), documented |
