"""Writes rn-tables-data.h: word tables of ru_normalizr as sorted C arrays.

Usage (oracle Python, docker image claude-ru-normalizr): python3 -I gen-tables.py RN_SRC OUT.h
(writes OUT.h and, next to it, rn-tables-list.h: the X-macro list of the table names)

Maps are {key, value} arrays, sets {key, NULL}; UTF-8, sorted by byte order, so rn-tables.c
looks them up with bsearch/strcmp.
"""
import sys

sys.path.insert(0, sys.argv[1])
from ru_normalizr import constants as K  # noqa: E402
from ru_normalizr import identifiers as I  # noqa: E402
from ru_normalizr.numerals import _helpers as H  # noqa: E402
from ru_normalizr.numerals import numeric_suffixes as NS  # noqa: E402
from ru_normalizr.numerals import _constants as NC  # noqa: E402
from ru_normalizr.numerals import _rules as NR  # noqa: E402
from ru_normalizr import roman_numerals as R  # noqa: E402
from ru_normalizr import abbreviation_rules as AR  # noqa: E402


def cstr(s) -> str:
    if s is None:
        return "NULL"
    out = '"'
    for b in s.encode():
        ch = chr(b)
        if ch in '"\\':
            out += "\\" + ch
        elif 32 <= b < 127:
            out += ch
        else:
            out += "\\%03o" % b
    return out + '"'


TABLES = {
    "RU_LETTER_NAMES": dict(K.RU_LETTER_NAMES),
    "EN_LETTER_NAMES": dict(K.EN_LETTER_NAMES),
    "RU_DOUBLED_VOWEL_LETTER_NAMES": dict(K.RU_DOUBLED_VOWEL_LETTER_NAMES),
    "EN_DOUBLED_VOWEL_LETTER_NAMES": dict(K.EN_DOUBLED_VOWEL_LETTER_NAMES),
    "NUMERIC_SUFFIXES": dict.fromkeys(NS.NUMERIC_SUFFIXES),
    "GLUED_PREPOSITIONS": dict.fromkeys(I.GLUED_PREPOSITIONS),
    "LABEL_NOUN_LEMMAS": dict.fromkeys(H.LABEL_NOUN_LEMMAS),
    "KNOWN_ABBREVIATIONS": dict(K.KNOWN_ABBREVIATIONS),
}

# numerals/_helpers.py (get_numeral_case and its rules)
for name in ("_TIME_CONTEXT_LEMMAS", "_PARTITIVE_BOUNDARY_LEMMAS", "NUMERAL_CONTEXT_BARRIERS",
             "SENTENCE_BOUNDARY_TOKENS", "GENITIVE_QUANTITY_GOVERNORS",
             "NEGATED_GENITIVE_PREDICATIVES", "NEGATED_GENITIVE_VERB_LEMMAS",
             "MEASURE_PARAMETER_HEADS", "LOCATIVE_GENITIVE_QUANTITY_HEADS",
             "_ARITHMETIC_OPERATOR_VERBS", "_MULTIPLICATION_MARKERS", "_DIMENSION_HEADS",
             "_TEMPERATURE_SCALE_SYMBOLS", "_COMPARISON_PHRASES", "_ANGLE_AND_DEGREE_UNITS",
             "_EQUALITY_PREDICATES", "_ERA_MARKER_WORDS", "_LANDMARK_MARKERS", "_SCORE_VERBS",
             "_SCORE_NOUNS", "_LOCATIVE_PREPOSITIONS", "_LABEL_PREFIX_TOKENS",
             "_NUMBERING_HEAD_TOKENS", "_TOTALIZING_PARTICLES", "_SERIES_COORDINATORS",
             "_VALUE_HEAD_LEMMAS", "_TIME_COUNTED_LEMMAS", "_GROUP_END_MARKERS", "_AGO_WORDS",
             "_ELAPSE_VERB_LEMMAS"):
    TABLES[name.lstrip("_")] = dict.fromkeys(getattr(H, name))
TABLES["DASH_TOKENS"] = dict.fromkeys(NR.DASH_TOKENS)
TABLES["PREP_CASE"] = dict(NC.PREP_CASE)
TABLES["VERB_CASE"] = dict(NC.VERB_CASE)
TABLES["TIME_WORDS"] = dict(NC.TIME_WORDS)
TABLES["ALL_UNITS"] = dict.fromkeys(NC.ALL_UNITS)
# entity-keyword-default returns ENTITY_DEFAULT_CASE of the first type whose keywords
# hold the word; all of them are nomn, so the union is enough
assert set(NC.ENTITY_DEFAULT_CASE.values()) == {"nomn"}, NC.ENTITY_DEFAULT_CASE
TABLES["ENTITY_KEYWORDS"] = dict.fromkeys(set().union(*NC.ENTITY_KEYWORDS.values()))
# roman_numerals.py
TABLES["REGNAL_CASE_CONTEXT"] = dict(R._REGNAL_CASE_CONTEXT)
TABLES["ROMAN_CONTEXT_LEMMAS"] = dict.fromkeys(R._ROMAN_CONTEXT_LEMMAS)
TABLES["ROMAN_HEADING_LEMMAS"] = dict.fromkeys(R._ROMAN_HEADING_LEMMAS)
TABLES["ROMAN_HEADING_ABBREVIATION_LEMMAS"] = dict(R._ROMAN_HEADING_ABBREVIATION_LEMMAS)
TABLES["ROMAN_ABBREVIATION_EXCEPTIONS"] = dict.fromkeys(R._ROMAN_ABBREVIATION_EXCEPTIONS)
TABLES["CENTURY_LOCATIVE_PREPOSITIONS"] = dict.fromkeys(R._CENTURY_LOCATIVE_PREPOSITIONS)
TABLES["HEAD_LOCATIVE_PREPOSITIONS"] = dict.fromkeys(R._HEAD_LOCATIVE_PREPOSITIONS)
# abbreviation_rules.py
TABLES["ADJECTIVE_ABBREVIATION_EXPANSIONS"] = dict(AR.ADJECTIVE_ABBREVIATION_EXPANSIONS)
TABLES["GEO_CLASSIFIER_ABBREVIATIONS"] = dict(AR.GEO_CLASSIFIER_ABBREVIATIONS)
TABLES["GEO_NAME_REQUIRED_ABBREVIATIONS"] = dict.fromkeys(AR.GEO_NAME_REQUIRED_ABBREVIATIONS)
TABLES["RELIEF_CONTEXT_LEMMAS"] = dict.fromkeys(AR.RELIEF_CONTEXT_LEMMAS)
# value "lemma number"
TABLES["DECLINABLE_NOUN_ABBREVIATIONS"] = {
    k: "%s %s" % v for k, v in AR.DECLINABLE_NOUN_ABBREVIATIONS.items()}
# ordered lists, as {key, value} in their order: ABBREVIATION_PATTERNS as {literal hint or
# NULL, replacement} (rn-patterns.h holds the patterns as ABBREVIATION_PATTERNS_<i>_0)
ORDERED = {
    "ABBREVIATION_RULES": list(zip(AR.ABBREVIATION_LITERAL_HINTS,
                                   (r for _p, r in AR.ABBREVIATION_PATTERNS))),
    "MOTION_VERB_ROOT_SUFFIXES": [(x, None) for x in AR.MOTION_VERB_ROOT_SUFFIXES],
}
for _k, _r in ORDERED["ABBREVIATION_RULES"]:
    assert "\\" not in _r, "match.expand() would treat the backslash"
assert R._ROMAN_ABBREVIATION_TO_CONTEXT == {
    "в": ("век", "sing"), "вв": ("век", "plur"), "кв": ("квартал", "sing"),
    "стол": ("столетие", "sing")}, "rn-roman.c hard-codes _ROMAN_ABBREVIATION_TO_CONTEXT"
assert R._ROMAN_DOT_ONLY_ABBREVIATIONS == {"стол"}

with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write("/* generated by gen-tables.py from ru_normalizr; do not edit */\n")
    for name, table in TABLES.items():
        for k, v in table.items():
            assert isinstance(k, str) and (v is None or isinstance(v, str)), (name, k, v)
        f.write(f"static const rn_kv RN_{name}[] = {{\n")
        for k in sorted(table, key=lambda s: s.encode()):
            f.write(f"\t{{{cstr(k)}, {cstr(table[k])}}},\n")
        f.write("};\n")
    for name, items in ORDERED.items():
        f.write(f"static const rn_kv RN_O_{name}[] = {{\n")
        for k, v in items:
            f.write(f"\t{{{cstr(k)}, {cstr(v)}}},\n")
        f.write("};\n")

import os  # noqa: E402

with open(os.path.join(os.path.dirname(sys.argv[2]) or ".", "rn-tables-list.h"), "w") as f:
    f.write("/* generated by gen-tables.py; do not edit */\n#define RN_TABLE_LIST(X)")
    for name in TABLES:
        f.write(" \\\n\tX(%s)" % name)
    f.write("\n")
