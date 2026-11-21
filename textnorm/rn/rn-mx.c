#include "rn-mx.h"

#include <stdlib.h>
#include <string.h>

#include "../num2words/n2w-ru.h"
#include "rn-pystr.h"

rn_gram rn_g;
_Thread_local int rn_mx_failed;

static int G(const char *name, int *ok)
{
	int i = cmorphy3_grammeme(rn_morph, name);
	if (i < 0)
		*ok = 0;
	return i;
}

int rn_mx_init(void)
{
	if (!rn_morph)
		return -1;
	int ok = 1;
	rn_gram g = {
		G("NOUN", &ok), G("VERB", &ok), G("INFN", &ok), G("GRND", &ok), G("PREP", &ok),
		G("CONJ", &ok), G("NPRO", &ok), G("ADJF", &ok), G("PRTF", &ok), G("PRTS", &ok),
		G("NUMR", &ok), G("nomn", &ok), G("gent", &ok), G("datv", &ok), G("accs", &ok),
		G("ablt", &ok), G("loct", &ok), G("loc2", &ok), G("voct", &ok), G("sing", &ok),
		G("plur", &ok), G("masc", &ok), G("femn", &ok), G("neut", &ok), G("ms-f", &ok),
		G("anim", &ok), G("inan", &ok), G("Fixd", &ok), G("Name", &ok), G("Surn", &ok),
		G("Patr", &ok), G("Geox", &ok), G("Orgn", &ok), G("Abbr", &ok), G("pssv", &ok),
		G("tran", &ok), G("past", &ok), G("COMP", &ok), G("PRCL", &ok), G("INTJ", &ok),
		G("impr", &ok), G("Anum", &ok),
	};
	rn_g = g;
	return ok ? 0 : -1;
}

/* the cache: open addressing on the UTF-8 key */
typedef struct {
	char *key;
	uint32_t hash;
	int count;
	cmorphy3_form *forms;
} entry;

static _Thread_local cmorphy3_arena *A;
static _Thread_local entry *T;
static _Thread_local size_t TCAP, TN;
static _Thread_local cmorphy3_form *STANDIN;
static _Thread_local char *KBUF;
static _Thread_local size_t KCAP;

#define TRIM_AT 200000

void rn_mx_trim(void)
{
	if (TN < TRIM_AT)
		return;
	free(T);
	T = NULL;
	TCAP = TN = 0;
	STANDIN = NULL;
	cmorphy3_arena_reset(A);
}

static uint32_t hash(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

static int grow(void)
{
	size_t cap = TCAP ? TCAP * 2 : 4096;
	entry *t = calloc(cap, sizeof *t);
	if (!t)
		return -1;
	for (size_t i = 0; i < TCAP; i++)
		if (T[i].key) {
			size_t j = T[i].hash & (cap - 1);
			while (t[j].key)
				j = (j + 1) & (cap - 1);
			t[j] = T[i];
		}
	free(T);
	T = t;
	TCAP = cap;
	return 0;
}

static const cmorphy3_form *standin(int *count)
{
	*count = 1;
	if (!STANDIN && A) {
		cmorphy3_form *f;
		if (cmorphy3_parse(rn_morph, A, "", &f) >= 1)
			STANDIN = f;
	}
	if (!STANDIN) {
		static const cmorphy3_form zero;
		return &zero;
	}
	return STANDIN;
}

int rn_mparse8(const char *u, const cmorphy3_form **out)
{
	int count;
	if (!A && !(A = cmorphy3_arena_new()))
		goto fail;
	if (TN * 2 >= TCAP && grow() < 0)
		goto fail;
	uint32_t h = hash(u);
	size_t j = h & (TCAP - 1);
	for (; T[j].key; j = (j + 1) & (TCAP - 1))
		if (T[j].hash == h && !strcmp(T[j].key, u)) {
			*out = T[j].forms;
			return T[j].count;
		}
	cmorphy3_form *f;
	int n = cmorphy3_parse(rn_morph, A, u, &f);
	char *k = n >= 1 ? cmorphy3_arena_strdup(A, u) : NULL;
	if (!k)
		goto fail;
	T[j] = (entry){k, h, n, f};
	TN++;
	*out = f;
	return n;
fail:
	rn_mx_failed = 1;
	*out = standin(&count);
	return count;
}

/* parse_word lowercases with str.lower() before pymorphy sees the word */
static _Thread_local rn_str LBUF;

int rn_mparse(const uint32_t *s, size_t n, const cmorphy3_form **out)
{
	LBUF.n = 0;
	int ok = rn_py_lower(s, n, &LBUF) == 0;
	if (ok) {
		KBUF = rn_to_utf8(LBUF.s, LBUF.n, KBUF, &KCAP);   /* frees KBUF on failure */
		ok = KBUF != NULL;
	}
	if (!ok) {
		int count;
		rn_mx_failed = 1;
		*out = standin(&count);
		return count;
	}
	return rn_mparse8(KBUF, out);
}

const cmorphy3_form *rn_mfirst(const uint32_t *s, size_t n)
{
	const cmorphy3_form *f;
	rn_mparse(s, n, &f);
	return f;
}

int rn_minflect(const cmorphy3_form *f, cmorphy3_gset g, cmorphy3_form *out)
{
	if (!A && !(A = cmorphy3_arena_new())) {
		rn_mx_failed = 1;
		return 0;
	}
	int r = cmorphy3_inflect(rn_morph, A, f, g, out);
	if (r < 0) {
		rn_mx_failed = 1;
		return 0;
	}
	return r == 1;
}

int rn_canon_case(int c)
{
	if (c < 0)
		return rn_g.nomn;
	cmorphy3_gset s = rn_canon(rn_gset1(c));
	/* next(iter(...)): the set has one element */
	for (int i = 0; i < 128; i++)
		if (cmorphy3_gset_has(s, i))
			return i;
	return rn_g.nomn;
}

int rn_n2w_case(int c)
{
	c = rn_canon_case(c);
	return c == rn_g.gent   ? N2W_GENT
	       : c == rn_g.datv ? N2W_DATV
	       : c == rn_g.accs ? N2W_ACCS
	       : c == rn_g.ablt ? N2W_ABLT
	       : c == rn_g.loct ? N2W_LOCT
	                        : N2W_NOMN;
}

const char *rn_gname(int g)
{
	return g < 0 ? "" : cmorphy3_grammeme_name(rn_morph, g);
}
