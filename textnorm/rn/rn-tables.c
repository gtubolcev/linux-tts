#include "rn-tables.h"

#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-pystr.h"
#include "rn-tables-data.h"

#define T(name) {RN_##name, sizeof RN_##name / sizeof *RN_##name}
static const struct {
	const rn_kv *kv;
	size_t n;
} TABLES[] = {
	T(RU_LETTER_NAMES),   T(EN_LETTER_NAMES),    T(RU_DOUBLED_VOWEL_LETTER_NAMES),
	T(EN_DOUBLED_VOWEL_LETTER_NAMES), T(NUMERIC_SUFFIXES), T(GLUED_PREPOSITIONS),
	T(LABEL_NOUN_LEMMAS),
};

static int cmp(const void *k, const void *e)
{
	return strcmp(k, ((const rn_kv *)e)->key);
}

const rn_kv *rn_table_get(enum rn_table t, const char *key)
{
	return bsearch(key, TABLES[t].kv, TABLES[t].n, sizeof(rn_kv), cmp);
}

const rn_kv *rn_table_get32(enum rn_table t, const uint32_t *s, size_t n)
{
	char small[256], *b = small;
	size_t cap = sizeof small;
	if (4 * n + 1 > cap) {
		b = NULL;
		cap = 0;
	}
	char *u = rn_to_utf8(s, n, b, &cap);
	if (!u)
		return NULL;
	const rn_kv *r = rn_table_get(t, u);
	if (u != small)
		free(u);
	return r;
}

const char *rn_letter_name(uint32_t letter, int double_vowels)
{
	rn_str up = {0};
	if (rn_py_upper(&letter, 1, &up) < 0)
		return NULL;
	const rn_kv *e = NULL;
	if (double_vowels) {
		e = rn_table_get32(RN_T_RU_DOUBLED_VOWEL_LETTER_NAMES, up.s, up.n);
		if (!e)
			e = rn_table_get32(RN_T_EN_DOUBLED_VOWEL_LETTER_NAMES, up.s, up.n);
	}
	/* RU.get(upper) or EN.get(upper): an empty name falls through as in Python */
	if (!e) {
		e = rn_table_get32(RN_T_RU_LETTER_NAMES, up.s, up.n);
		if (!e || !*e->value)
			e = rn_table_get32(RN_T_EN_LETTER_NAMES, up.s, up.n);
	}
	rn_str_free(&up);
	return e ? e->value : NULL;
}
