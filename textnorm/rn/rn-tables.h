/* word tables of ru_normalizr (gen-tables.py → rn-tables-data.h) */
#ifndef RN_TABLES_H
#define RN_TABLES_H

#include "rn-re.h"

typedef struct {
	const char *key, *value;
} rn_kv;

#include "rn-tables-list.h"

enum rn_table {
#define RN_TABLE_ENUM(name) RN_T_##name,
	RN_TABLE_LIST(RN_TABLE_ENUM)
#undef RN_TABLE_ENUM
};

/* the entry for key (UTF-8), NULL if absent */
const rn_kv *rn_table_get(enum rn_table t, const char *key);
/* the same for a UTF-32 key */
const rn_kv *rn_table_get32(enum rn_table t, const uint32_t *s, size_t n);
/* key presence, UTF-8 / UTF-32 */
#define rn_in(t, key) (rn_table_get(RN_T_##t, key) != NULL)
#define rn_in32(t, s, n) (rn_table_get32(RN_T_##t, s, n) != NULL)

/* resolve_letter_name(letter, vowel_mode) of constants.py: NULL for None */
const char *rn_letter_name(uint32_t letter, int double_vowels);

#endif
