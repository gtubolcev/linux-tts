/* shared helpers of the numerals passes (numerals/*.py): string helpers as static inline
 * functions, and rn-nx.c: select_parse, safe_inflect, inflect_unit_lemma,
 * render_unit_expression, inflect_numeral_string, render_ordinal with grammemes.
 *
 * Errors (out of memory, morphology) set rn_mx_failed; the passes check it at the end. */
#ifndef RN_NX_H
#define RN_NX_H

#include <stdlib.h>
#include <string.h>
#include <uchar.h>

#include "rn-mx.h"
#include "rn-ncase.h"
#include "rn-num.h"
#include "rn-pystr.h"
#include "rn-units.h"
#include "rn-util.h"

/* ---- strings ---- */

static inline void nx_fail(void)
{
	rn_mx_failed = 1;
}

static inline void nx_put(rn_str *o, const uint32_t *s, size_t n)
{
	if (rn_str_put(o, s, n) < 0)
		nx_fail();
}

static inline void nx_putc(rn_str *o, uint32_t c)
{
	if (rn_str_putc(o, c) < 0)
		nx_fail();
}

static inline void nx_put8(rn_str *o, const char *u)
{
	if (rn_str_utf8(o, u) < 0)
		nx_fail();
}

static inline void nx_lit(rn_str *o, const char32_t *s)
{
	if (rn_puts(o, (const uint32_t *)s) < 0)
		nx_fail();
}

static inline int nx_whole(const rn_match *m, rn_str *o)
{
	nx_put(o, m->s + m->start[0], m->end[0] - m->start[0]);
	return 0;
}

static inline int nx_eq(const uint32_t *s, size_t n, const char32_t *lit)
{
	size_t i = 0;
	for (; i < n && lit[i]; i++)
		if (s[i] != lit[i])
			return 0;
	return i == n && !lit[i];
}

static inline int nx_starts(const uint32_t *s, size_t n, const char32_t *lit)
{
	size_t i = 0;
	for (; lit[i]; i++)
		if (i >= n || s[i] != lit[i])
			return 0;
	return 1;
}

static inline int nx_ends(const uint32_t *s, size_t n, const char32_t *lit)
{
	size_t l = 0;
	while (lit[l])
		l++;
	return l <= n && nx_starts(s + n - l, l, lit);
}

static inline int nx_inset(uint32_t c, const char32_t *set)
{
	for (; *set; set++)
		if (c == *set)
			return 1;
	return 0;
}

/* s equals one of the NULL-terminated literals */
static inline int nx_oneof(const uint32_t *s, size_t n, const char32_t *const *lits)
{
	for (; *lits; lits++)
		if (nx_eq(s, n, *lits))
			return 1;
	return 0;
}

/* t = s.lower() */
static inline void nx_lower(const uint32_t *s, size_t n, rn_str *t)
{
	t->n = 0;
	if (rn_py_lower(s, n, t) < 0)
		nx_fail();
}

static inline const uint32_t *nx_grp(const rn_match *m, const rn_re *p, const char *name,
                                     size_t *n)
{
	return rn_group(m, rn_re_group(p, name), n);
}

/* m.group(name) appended to o ("" if unmatched) */
static inline void nx_putg(rn_str *o, const rn_match *m, const rn_re *p, const char *name)
{
	size_t n;
	const uint32_t *s = nx_grp(m, p, name, &n);
	if (s)
		nx_put(o, s, n);
}

static inline size_t nx_gstart(const rn_match *m, const rn_re *p, const char *name)
{
	return m->start[rn_re_group(p, name)];
}

static inline size_t nx_gend(const rn_match *m, const rn_re *p, const char *name)
{
	return m->end[rn_re_group(p, name)];
}

/* pattern.search(s[a:b]) is not None */
static inline int nx_found(const rn_re *re, const uint32_t *s, size_t a, size_t b)
{
	rn_match m;
	int r = rn_search(re, s + a, b - a, 0, &m);
	if (r < 0)
		nx_fail();
	return r > 0;
}

/* max(0, start - k) */
static inline size_t nx_left(size_t start, size_t k)
{
	return start > k ? start - k : 0;
}

/* in = pattern.sub(fn, in): cur and tmp swap */
static inline void nx_resub(const rn_re *re, rn_str *cur, rn_repl_fn fn, void *ctx)
{
	if (rn_resub(re, cur, fn, ctx) < 0)
		nx_fail();
}

/* ---- numbers ---- */

/* str(int(s)) with Python's int() of a str: whitespace around, a sign, digits of any script
 * with single underscores between them. Into buf; its length, or -1 (ValueError, or too
 * long for cap) */
int nx_pyint(const uint32_t *s, size_t n, char *buf, size_t cap);
/* int(s) as a long when it fits (value saturated at LONG_MAX); 0 on ValueError */
int nx_pyint_long(const uint32_t *s, size_t n, long *v);

/* inflect_numeral_string(num_str, case, gender, animate) of _helpers.py: case a grammeme,
 * gender a grammeme (masc/femn/neut or plur) or -1, animate -1 (None), 0, 1 */
void nx_numeral(rn_str *out, const uint32_t *num, size_t n, int gcase, int gender, int animate);
void nx_numeral_a(rn_str *out, const char *num, int gcase, int gender, int animate);
/* render_ordinal(number, case, gender, plural, inanimate) of ordinal_utils.py with
 * grammemes (gender -1 for None); number as an ASCII int string */
void nx_ordinal(rn_str *out, const char *num, int gcase, int gender, int plural, int inanimate);
void nx_ordinal_l(rn_str *out, long v, int gcase, int gender, int plural, int inanimate);

/* ---- morphology ---- */

/* a gset from grammeme names: nx_gs("gent plur") (cached by pointer: pass literals) */
cmorphy3_gset nx_gs(const char *names);
int nx_gram(const char *name);   /* a grammeme index, -1 */

/* select_parse keyword arguments; zero = no constraint except the -1 fields, which must be
 * set to -1 when unused (use NX_SELQ) */
typedef struct {
	int pos[4];                 /* allowed POS, -1 terminated; pos[0] == -1: any */
	const char *const *nforms;  /* allowed normal forms (lower case UTF-8), NULL-terminated */
	cmorphy3_gset req, forb, pref;
	int gender, number, gcase;  /* -1: any */
} nx_selq;
#define NX_SELQ ((nx_selq){{-1, -1, -1, -1}, NULL, {0, 0}, {0, 0}, {0, 0}, -1, -1, -1})

/* select_parse(parses, ...) */
const cmorphy3_form *nx_select(const cmorphy3_form *f, int count, const nx_selq *q);
/* select_parse(word, ...) */
const cmorphy3_form *nx_select_w(const uint32_t *s, size_t n, const nx_selq *q);
const cmorphy3_form *nx_select8(const char *word, const nx_selq *q);

/* safe_inflect(parsed, tags, fallback_word, pos_filter): fallback NULL = parsed.word;
 * pos_filter_adj = pos_filter {"ADJF", "PRTF"} (it never matters otherwise) */
void nx_safe_inflect(rn_str *out, const cmorphy3_form *f, cmorphy3_gset tags,
                     const char *fallback);
/* parsed.inflect(tags).word appended: 1, or 0 (nothing appended) */
int nx_inflect(rn_str *out, const cmorphy3_form *f, cmorphy3_gset tags);

/* inflect_unit_lemma(unit, tags, paucal) */
void nx_unit_lemma(rn_str *out, const rn_unit *u, cmorphy3_gset tags, int paucal);
/* the same for a plain lemma (UTF-8) */
void nx_unit_lemma8(rn_str *out, const char *lemma, cmorphy3_gset tags, int paucal);
/* render_unit_expression(unit, tags, paucal) */
void nx_render_unit(rn_str *out, const rn_unit *u, cmorphy3_gset tags, int paucal);

/* parse_word(s) has a parse with the POS */
int nx_has_pos(const uint32_t *s, size_t n, int pos);
int nx_has_pos8(const char *s, int pos);

/* get_numeral_case([*simple_tokenize(t[st-k:st]), num, *simple_tokenize(t[en:en+k])],
 * len(left tokens)) */
int nx_case_around(const uint32_t *t, size_t tn, size_t st, size_t en, const uint32_t *num,
                   size_t numn, size_t k);
/* the last token of simple_tokenize(t[st-k:st]) with a letter, lowered ("" if none) */
void nx_left_word(const uint32_t *t, size_t st, size_t k, rn_str *out);

#endif
