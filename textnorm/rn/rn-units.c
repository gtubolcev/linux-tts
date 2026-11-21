#include "rn-units.h"

#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-pat.h"
#include "rn-pystr.h"

typedef struct {
	const char *alias;
	int unit;
} rn_unit_alias;

typedef struct {
	const char *key;
	int unit, nsem;
} rn_unit_fold;

#include "rn-units-data.h"

#define N(a) (sizeof a / sizeof *a)

static int cmp_alias(const void *k, const void *e)
{
	return strcmp(k, ((const rn_unit_alias *)e)->alias);
}

static int cmp_fold(const void *k, const void *e)
{
	return strcmp(k, ((const rn_unit_fold *)e)->key);
}

static const rn_unit *lookup(const rn_unit_alias *t, size_t n, const char *key)
{
	const rn_unit_alias *a = bsearch(key, t, n, sizeof *t, cmp_alias);
	return a ? &RN_UNITS[a->unit] : NULL;
}

static _Thread_local char *B1, *B2;
static _Thread_local size_t C1, C2;

const rn_unit *rn_unit_resolve(const uint32_t *s, size_t n)
{
	size_t a, b;
	rn_py_strip(s, n, &a, &b);
	if (a == b)
		return NULL;
	const uint32_t *o = s + a;
	size_t on = b - a, cn = on;
	while (cn && o[cn - 1] == '.')
		cn--;
	if (!(B1 = rn_to_utf8(o, on, B1, &C1)))
		return NULL;
	const rn_unit *u = lookup(RN_UNITS_EXACT, N(RN_UNITS_EXACT), B1);
	if (u)
		return u;
	if (!(B2 = rn_to_utf8(o, cn, B2, &C2)))
		return NULL;
	if (cn != on && (u = lookup(RN_UNITS_EXACT, N(RN_UNITS_EXACT), B2)))
		return u;
	if ((u = lookup(RN_UNITS_FOLDED, N(RN_UNITS_FOLDED), B1)))
		return u;
	if (cn != on && (u = lookup(RN_UNITS_FOLDED, N(RN_UNITS_FOLDED), B2)))
		return u;
	rn_str f = {0};
	if (rn_py_casefold(o, cn, &f) < 0) {
		rn_str_free(&f);
		return NULL;
	}
	int same = f.n == cn && (!cn || !memcmp(f.s, o, cn * sizeof *o));
	char *key = rn_to_utf8(f.s, f.n, NULL, &(size_t){0});
	rn_str_free(&f);
	if (!key)
		return NULL;
	const rn_unit_fold *e = bsearch(key, RN_UNITS_FOLD, N(RN_UNITS_FOLD), sizeof *e, cmp_fold);
	free(key);
	if (!e)
		return NULL;
	if (cn == 1 && rn_py_isalpha(o[0]) && !same)
		return NULL;
	return e->nsem == 1 ? &RN_UNITS[e->unit] : NULL;
}

int rn_unit_is_exact(const uint32_t *s, size_t n)
{
	size_t a, b;
	rn_py_strip(s, n, &a, &b);
	while (b > a && s[b - 1] == '.')
		b--;
	if (!(B1 = rn_to_utf8(s + a, b - a, B1, &C1)))
		return 0;
	return lookup(RN_UNITS_EXACT, N(RN_UNITS_EXACT), B1) != NULL;
}

const rn_unit *rn_unit_resolve_expr(const uint32_t *s, size_t n, char *kind)
{
	size_t a, b;
	rn_py_strip(s, n, &a, &b);
	rn_str t = {0};
	if (rn_sub_tmpl(rn_pat(RN_NUMERALS_UNIT_EXPRESSIONS_SEPARATOR_SPACING), s + a, b - a,
	                (const uint32_t *)U"\1", 0, &t) < 0) {
		rn_str_free(&t);
		return NULL;
	}
	const rn_unit *u = rn_unit_resolve(t.s, t.n);
	if (u && kind) {
		*kind = 'a';
		for (size_t i = 0; i < t.n; i++)
			if (t.s[i] == '/') {
				*kind = 'd';
				break;
			}
		if (*kind == 'a')
			for (size_t i = 0; i < t.n; i++)
				if (t.s[i] == 0xB7)
					*kind = 'x';
		if (*kind == 'a' && t.n &&
		    (t.s[t.n - 1] == 0xB2 || t.s[t.n - 1] == 0xB3 ||
		     (t.n >= 2 && t.s[t.n - 2] == '^' && (t.s[t.n - 1] == '2' || t.s[t.n - 1] == '3'))))
			*kind = 'p';
	}
	rn_str_free(&t);
	return u;
}
