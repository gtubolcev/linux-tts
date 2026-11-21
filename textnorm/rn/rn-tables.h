/* word tables of ru_normalizr (gen-tables.py → rn-tables-data.h) */
#ifndef RN_TABLES_H
#define RN_TABLES_H

#include "rn-re.h"

typedef struct {
	const char *key, *value;
} rn_kv;

enum rn_table {
	RN_T_RU_LETTER_NAMES,
	RN_T_EN_LETTER_NAMES,
	RN_T_RU_DOUBLED_VOWEL_LETTER_NAMES,
	RN_T_EN_DOUBLED_VOWEL_LETTER_NAMES,
	RN_T_NUMERIC_SUFFIXES,
	RN_T_GLUED_PREPOSITIONS,
	RN_T_LABEL_NOUN_LEMMAS,
};

/* the entry for key (UTF-8), NULL if absent */
const rn_kv *rn_table_get(enum rn_table t, const char *key);
/* the same for a UTF-32 key */
const rn_kv *rn_table_get32(enum rn_table t, const uint32_t *s, size_t n);

/* resolve_letter_name(letter, vowel_mode) of constants.py: NULL for None */
const char *rn_letter_name(uint32_t letter, int double_vowels);

#endif
