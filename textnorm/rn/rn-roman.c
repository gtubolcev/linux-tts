/* normalize_roman of roman_numerals.py: the 13 roman numeral steps.
 *
 * Errors (out of memory, a regex failure) inside the helpers set rn_mx_failed and let the
 * step go on with a harmless value; rn_run_roman reports them at the end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>

#include "rn-mx.h"
#include "rn-ncase.h"
#include "rn-num.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pystr.h"
#include "rn-tables.h"
#include "rn-units.h"
#include "rn-util.h"

#define P(x) rn_pat(RN_ROMAN_NUMERALS_##x)

static const char32_t PUNCT[] = U".,:;!\"«»()[]{}";   /* PUNCT_STRIP of text_context */

/* ---- strings ---- */

static int eq(const uint32_t *s, size_t n, const char32_t *lit)
{
	size_t i = 0;
	for (; i < n && lit[i]; i++)
		if (s[i] != lit[i])
			return 0;
	return i == n && !lit[i];
}

static int inset(uint32_t c, const char32_t *set)
{
	for (; *set; set++)
		if (c == *set)
			return 1;
	return 0;
}

static void fail(void)
{
	rn_mx_failed = 1;
}

static void put(rn_str *o, const uint32_t *s, size_t n)
{
	if (rn_str_put(o, s, n) < 0)
		fail();
}

static void put8(rn_str *o, const char *u)
{
	if (rn_str_utf8(o, u) < 0)
		fail();
}

static void putlit(rn_str *o, const char32_t *s)
{
	if (rn_puts(o, (const uint32_t *)s) < 0)
		fail();
}

static void putnum(rn_str *o, long v)
{
	char b[24];
	snprintf(b, sizeof b, "%ld", v);
	put8(o, b);
}

static void putlower(rn_str *o, const uint32_t *s, size_t n)
{
	if (rn_py_lower(s, n, o) < 0)
		fail();
}

/* out = s.lower().strip(PUNCT_STRIP): normalize_context_token */
static void norm_tok(const uint32_t *s, size_t n, rn_str *o)
{
	o->n = 0;
	putlower(o, s, n);
	size_t a = 0, b = o->n;
	while (a < b && inset(o->s[a], PUNCT))
		a++;
	while (b > a && inset(o->s[b - 1], PUNCT))
		b--;
	if (a && b > a)
		memmove(o->s, o->s + a, (b - a) * sizeof *o->s);
	o->n = b - a;
}

/* s.lower() == lit */
static int lower_eq(const uint32_t *s, size_t n, const char32_t *lit)
{
	rn_str t = {0};
	putlower(&t, s, n);
	int r = eq(t.s, t.n, lit);
	rn_str_free(&t);
	return r;
}

/* s == s.upper() */
static int is_upper_form(const uint32_t *s, size_t n)
{
	rn_str t = {0};
	if (rn_py_upper(s, n, &t) < 0)
		fail();
	int r = t.n == n && (!n || !memcmp(t.s, s, n * sizeof *s));
	rn_str_free(&t);
	return r;
}

static int is_lower_form(const uint32_t *s, size_t n)
{
	rn_str t = {0};
	putlower(&t, s, n);
	int r = t.n == n && (!n || !memcmp(t.s, s, n * sizeof *s));
	rn_str_free(&t);
	return r;
}

/* s.rstrip() length */
static size_t rstrip_len(const uint32_t *s, size_t n)
{
	while (n && rn_py_isspace(s[n - 1]))
		n--;
	return n;
}

static int search(const rn_re *re, const uint32_t *s, size_t n, rn_match *m)
{
	int r = rn_search(re, s, n, 0, m);
	if (r < 0) {
		fail();
		return 0;
	}
	return r;
}

static int match_at(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m)
{
	int r = rn_match_at(re, s, n, pos, m);
	if (r < 0) {
		fail();
		return 0;
	}
	return r;
}

/* named group of m (rn_group with a name) */
static const uint32_t *grp(const rn_re *re, const rn_match *m, const char *name, size_t *n)
{
	return rn_group(m, rn_re_group(re, name), n);
}

/* ---- morphology ---- */

static int has_name_marker(uint16_t tag)
{
	return rn_has(tag, rn_g.Name) || rn_has(tag, rn_g.Surn) || rn_has(tag, rn_g.Patr);
}

static int noun_case(const cmorphy3_form *p)   /* noun_parse_case */
{
	return rn_canon_case(rn_case(p->tag));
}

/* choose_noun_parse over the parses f[0..c) */
static const cmorphy3_form *choose_noun_in(const cmorphy3_form *f, int c, int prefer_inan)
{
	const cmorphy3_form *first = NULL;
	for (int i = 0; i < c; i++)
		if (rn_has(f[i].tag, rn_g.NOUN)) {
			if (!first)
				first = &f[i];
			if (!prefer_inan)
				return first;
			if (rn_has(f[i].tag, rn_g.inan))
				return &f[i];
		}
	return first;
}

static const cmorphy3_form *choose_noun(const uint32_t *s, size_t n, int prefer_inan)
{
	const cmorphy3_form *f;
	int c = rn_mparse(s, n, &f);
	return choose_noun_in(f, c, prefer_inan);
}

static const cmorphy3_form *choose_noun8(const char *lemma)
{
	const cmorphy3_form *f;
	int c = rn_mparse8(lemma, &f);
	return choose_noun_in(f, c, 1);
}

static int gender_n2w(int g)
{
	return g == rn_g.masc   ? N2W_MASC
	       : g == rn_g.femn ? N2W_FEMN
	       : g == rn_g.neut ? N2W_NEUT
	                        : RN_NO_GENDER;
}

/* render_ordinal(number, case, gender, plural, inanimate); gender a grammeme or -1 */
static void put_ord(rn_str *o, long v, int gcase, int gender, int plural, int inan)
{
	char b[24];
	snprintf(b, sizeof b, "%ld", v);
	if (rn_put_ordinal(o, b, rn_n2w_case(gcase), gender_n2w(gender), plural, inan) < 0)
		fail();
}

/* _render_ordinal_for_context_noun; gcase -1 = None */
static void put_ord_ctx(rn_str *o, long v, const cmorphy3_form *p, int gcase)
{
	int g = rn_gender(p->tag);
	put_ord(o, v, gcase >= 0 ? gcase : noun_case(p), g >= 0 ? g : rn_g.masc, 0,
	        rn_has(p->tag, rn_g.inan));
}

/* render_ordinal_from_noun_parse without hints */
static void put_ord_parse(rn_str *o, long v, const cmorphy3_form *p, int singularize)
{
	int g = rn_gender(p->tag);
	put_ord(o, v, noun_case(p), g >= 0 ? g : rn_g.masc,
	        rn_has(p->tag, rn_g.plur) && !singularize, rn_animacy(p->tag) == rn_g.inan);
}

/* ---- context nouns ---- */

/* _ROMAN_ABBREVIATION_TO_CONTEXT (gen-tables.py asserts it) */
static const struct {
	const char32_t *abbr;
	const char *lemma;
	int plural;
} ABBR[] = {{U"в", "век", 0}, {U"вв", "век", 1}, {U"кв", "квартал", 0}, {U"стол", "столетие", 0}};
#define ABBR_DOT_ONLY 3   /* _ROMAN_DOT_ONLY_ABBREVIATIONS = {стол} */

static int abbr_index(const rn_str *norm)
{
	for (int i = 0; i < (int)(sizeof ABBR / sizeof *ABBR); i++)
		if (eq(norm->s, norm->n, ABBR[i].abbr))
			return i;
	return -1;
}

/* _split_context_word_punctuation: the length of the base word (*dot: "." split off) */
static size_t split_punct(const uint32_t *w, size_t n, int *dot)
{
	*dot = 0;
	if (!n || w[n - 1] != '.')
		return n;
	rn_str t = {0};
	norm_tok(w, n, &t);
	int in = abbr_index(&t) >= 0;
	rn_str_free(&t);
	if (in)
		return n;
	*dot = 1;
	return n - 1;
}

typedef struct {
	const char *lemma;
	const cmorphy3_form *p;
	int number;   /* abbreviation_number: rn_g.sing / rn_g.plur, -1 = None */
} cnoun;

/* _resolve_roman_context_noun: 1 and *c, or 0 (None) */
static int resolve_noun(const uint32_t *w, size_t n, int allow_abbr, cnoun *c)
{
	rn_str t = {0};
	norm_tok(w, n, &t);
	int empty = !t.n, i = abbr_index(&t);
	rn_str_free(&t);
	if (empty)
		return 0;
	if (allow_abbr && i >= 0 && (i != ABBR_DOT_ONLY || w[n - 1] == '.')) {
		c->p = choose_noun8(ABBR[i].lemma);
		if (!c->p)
			return 0;
		c->lemma = ABBR[i].lemma;
		c->number = ABBR[i].plural ? rn_g.plur : rn_g.sing;
		return 1;
	}
	c->p = choose_noun(w, n, 1);
	if (!c->p || !rn_table_get(RN_T_ROMAN_CONTEXT_LEMMAS, c->p->normal_form))
		return 0;
	c->lemma = c->p->normal_form;
	c->number = -1;
	return 1;
}

/* _render_context_noun_word */
static void put_noun_word(rn_str *o, const uint32_t *w, size_t n, const cnoun *c, int gcase,
                          int number)
{
	if (number < 0) {
		put(o, w, n);
		return;
	}
	cmorphy3_form f;
	if (rn_minflect(c->p, rn_gset_or(rn_gset1(gcase), rn_gset1(number)), &f))
		put8(o, f.word);
	else
		put8(o, c->lemma);
}

/* the slice text[max(0, st - k):st] */
static const uint32_t *left_slice(const uint32_t *t, size_t st, size_t k, size_t *n)
{
	size_t a = st > k ? st - k : 0;
	*n = st - a;
	return t + a;
}

/* the word ([А-ЯЁа-яё-]+ or the like) ending the trimmed 40 characters left of st, when
 * whitespace separates it from st: 1 and the word */
static int left_word(const uint32_t *t, size_t st, const rn_re *re, const uint32_t **w,
                     size_t *wn)
{
	size_t ln;
	const uint32_t *l = left_slice(t, st, 40, &ln);
	size_t tn = rstrip_len(l, ln);
	if (tn == ln || !tn)
		return 0;
	rn_match m;
	if (!search(re, l, tn, &m))
		return 0;
	*w = rn_group(&m, 0, wn);
	return 1;
}

/* _governing_noun_precedes */
static int governing_noun_precedes(const uint32_t *t, size_t st)
{
	const uint32_t *w;
	size_t wn;
	if (!left_word(t, st, P(L234), &w, &wn))
		return 0;
	const cmorphy3_form *f;
	int c = rn_mparse(w, wn, &f);
	if (!rn_has(f[0].tag, rn_g.NOUN) || !rn_has(f[0].tag, rn_g.inan)) {
		int heading = 0;
		for (int i = 0; i < c && !heading; i++)
			heading = rn_has(f[i].tag, rn_g.NOUN) && rn_has(f[i].tag, rn_g.inan) &&
			          rn_table_get(RN_T_ROMAN_HEADING_LEMMAS, f[i].normal_form);
		if (!heading)
			return 0;
	}
	for (int i = 0; i < c; i++)
		if (has_name_marker(f[i].tag))
			return 0;
	return 1;
}

/* _animate_name_precedes */
static int animate_name_precedes(const uint32_t *t, size_t st)
{
	const uint32_t *w;
	size_t wn;
	if (!left_word(t, st, P(L266), &w, &wn))
		return 0;
	const cmorphy3_form *f;
	int c = rn_mparse(w, wn, &f);
	for (int i = 0; i < c; i++)
		if (rn_has(f[i].tag, rn_g.anim) && has_name_marker(f[i].tag))
			return 1;
	return 0;
}

/* _agreeing_adjective_precedes */
static int agreeing_adjective_precedes(const uint32_t *t, size_t st, const cmorphy3_form *p)
{
	const uint32_t *w;
	size_t wn;
	if (!left_word(t, st, P(L234), &w, &wn))
		return 0;
	int nc = noun_case(p), nn = rn_number(p->tag);
	const cmorphy3_form *f;
	int c = rn_mparse(w, wn, &f);
	for (int i = 0; i < c; i++) {
		int pos = rn_pos(f[i].tag);
		if ((pos == rn_g.ADJF || pos == rn_g.PRTF) && rn_canon_case(rn_case(f[i].tag)) == nc &&
		    rn_number(f[i].tag) == nn)
			return 1;
	}
	return 0;
}

/* _infer_roman_context_case; the noun as UTF-32 */
static int infer_case(const uint32_t *t, size_t tn, size_t st, size_t en, const uint32_t *w,
                      size_t wn)
{
	if (governing_noun_precedes(t, st))
		return rn_g.gent;
	size_t ln, rn = en + 60 < tn ? 60 : tn - en;
	const uint32_t *l = left_slice(t, st, 60, &ln);
	rn_tok *L = NULL, *R = NULL, *all = NULL;
	long nl = rn_simple_tokenize(l, ln, &L);
	long nr = nl < 0 ? -1 : rn_simple_tokenize(t + en, rn, &R);
	int r = -1;
	if (nr >= 0 && (all = malloc((size_t)(nl + nr + 2) * sizeof *all))) {
		memcpy(all, L, (size_t)nl * sizeof *all);
		all[nl] = (rn_tok){(const uint32_t *)U"1", 1};
		all[nl + 1] = (rn_tok){w, wn};
		memcpy(all + nl + 2, R, (size_t)nr * sizeof *all);
		rn_ncase *c = rn_ncase_new(all, (size_t)(nl + nr + 2));
		if (c) {
			r = rn_ncase_get(c, (size_t)nl, SIZE_MAX, NULL);
			rn_ncase_free(c);
		}
	}
	free(L);
	free(R);
	free(all);
	if (r < 0) {
		fail();
		return rn_g.nomn;
	}
	return r;
}

static int infer_case8(const uint32_t *t, size_t tn, size_t st, size_t en, const char *w)
{
	rn_str u = {0};
	put8(&u, w);
	int r = infer_case(t, tn, st, en, u.s, u.n);
	rn_str_free(&u);
	return r;
}

/* _resolve_full_noun_case_or_none: the case or -1 */
static int full_noun_case(const uint32_t *w, size_t n, const char *lemma)
{
	const cmorphy3_form *f;
	int c = rn_mparse(w, n, &f), found = -1;
	for (int i = 0; i < c; i++)
		if (rn_has(f[i].tag, rn_g.NOUN) && !strcmp(f[i].normal_form, lemma)) {
			int k = rn_canon_case(rn_case(f[i].tag));
			if (found >= 0 && found != k)
				return -1;
			found = k;
		}
	return found;
}

/* _preceding_word(...) in the table */
static int preceding_word_in(const uint32_t *t, size_t st, enum rn_table tab)
{
	size_t ln;
	const uint32_t *l = left_slice(t, st, 40, &ln);
	rn_tok *k = NULL;
	long nk = rn_simple_tokenize(l, ln, &k);
	if (nk < 0) {
		fail();
		return 0;
	}
	int r = 0;
	for (long i = nk - 1; i >= 0; i--) {
		int alpha = 0;
		for (size_t j = 0; j < k[i].n && !alpha; j++)
			alpha = rn_py_isalpha(k[i].s[j]);
		if (alpha) {
			rn_str w = {0};
			norm_tok(k[i].s, k[i].n, &w);
			r = rn_table_get32(tab, w.s ? w.s : (const uint32_t *)U"", w.n) != NULL;
			rn_str_free(&w);
			break;
		}
	}
	free(k);
	return r;
}

/* _infer_abbreviated_context_case */
static int abbreviated_case(const uint32_t *t, size_t tn, size_t st, size_t en,
                            const char *lemma, enum rn_table locs)
{
	int c = infer_case8(t, tn, st, en, lemma);
	if (c != rn_g.accs)
		return c;
	return preceding_word_in(t, st, locs) ? rn_g.loct : c;
}

/* _resolve_contextual_ordinal_case */
static int contextual_case(const uint32_t *t, size_t tn, size_t st, size_t en, const uint32_t *w,
                           size_t wn, const cnoun *c)
{
	if (c->number >= 0 && !strcmp(c->lemma, "век"))
		return abbreviated_case(t, tn, st, en, "век", RN_T_CENTURY_LOCATIVE_PREPOSITIONS);
	if (c->number >= 0 && !strcmp(c->lemma, "квартал"))
		return abbreviated_case(t, tn, st, en, "квартал", RN_T_HEAD_LOCATIVE_PREPOSITIONS);
	if (c->number >= 0)
		return infer_case8(t, tn, st, en, c->lemma);
	int k = full_noun_case(w, wn, c->lemma);
	return k >= 0 ? k : infer_case(t, tn, st, en, w, wn);
}

/* ---- renderers: 1 = appended, 0 = None (nothing appended) ---- */

static int render_single(rn_str *o, const rn_match *m, long v, const uint32_t *w, size_t wn)
{
	const uint32_t *t = m->s;
	int dot;
	size_t bn = split_punct(w, wn, &dot);
	cnoun c;
	if (!resolve_noun(w, bn, 1, &c))
		return 0;
	if (c.number < 0 && rn_has(c.p->tag, rn_g.plur) && noun_case(c.p) == rn_g.gent) {
		putnum(o, v);
		putlit(o, U" ");
		put(o, w, bn);
		if (dot)
			putlit(o, U".");
		return 1;
	}
	int k = c.number < 0 ? noun_case(c.p)
	                     : contextual_case(t, m->n, m->start[0], m->end[0], w, bn, &c);
	put_ord_ctx(o, v, c.p, k);
	putlit(o, U" ");
	put_noun_word(o, w, bn, &c, k, c.number);
	if (dot)
		putlit(o, U".");
	return 1;
}

/* the ordinals and the noun of a series or range, ordered as the noun_first branches of
 * _render_roman_series_with_context / _render_roman_hyphen_range_with_context */
static void put_ordered(rn_str *o, rn_str *nums, const uint32_t *w, size_t wn, size_t bn,
                        int dot, const cnoun *c, int k, int noun_first, int keep, int range)
{
	(void)wn;
	rn_str noun = {0};
	put_noun_word(&noun, w, bn, c, k, c->number);
	if (noun_first && keep) {
		put(o, noun.s, noun.n);
		putlit(o, U" ");
		put(o, nums->s, nums->n);
	} else if (noun_first) {
		if (rn_py_isupper(w[0]) && nums->n) {
			if (rn_py_upper(nums->s, 1, o) < 0)
				fail();
			put(o, nums->s + 1, nums->n - 1);
		} else {
			put(o, nums->s, nums->n);
		}
		putlit(o, U" ");
		/* R5: the range lowers base_word ("вв." stays unread), the series the noun */
		if (range && !(rn_fixes & RN_FIX_ROMAN_RANGE_NOUN))
			putlower(o, w, bn);
		else
			putlower(o, noun.s, noun.n);
	} else {
		put(o, nums->s, nums->n);
		putlit(o, U" ");
		put(o, noun.s, noun.n);
	}
	if (dot)
		putlit(o, U".");
	rn_str_free(&noun);
}

static int render_series(rn_str *o, const rn_match *m, const uint32_t *ser, size_t sn,
                         const uint32_t *w, size_t wn, int noun_first)
{
	const uint32_t *t = m->s;
	int dot;
	size_t bn = split_punct(w, wn, &dot);
	cnoun c;
	if (!resolve_noun(w, bn, 1, &c))
		return 0;
	int keep = noun_first && agreeing_adjective_precedes(t, m->start[0], c.p);
	int k = keep ? noun_case(c.p) : contextual_case(t, m->n, m->start[0], m->end[0], w, bn, &c);
	rn_str nums = {0};
	const rn_re *sep = rn_pat(RN_ROMAN_NUMERALS_ROMAN_SHARED_SEPARATOR_PATTERN);
	rn_iter it = rn_iter_init(sep, ser, sn, 0);
	rn_match sm;
	size_t pos = 0;
	int r, ok = 1;
	for (;;) {
		r = rn_iter_next(&it, &sm);
		if (r < 0)
			fail();
		size_t end = r == 1 ? sm.start[0] : sn;
		if (end > pos) {
			long v = rn_roman_value(ser + pos, end - pos);
			if (v < 0) {
				ok = 0;
				break;
			}
			put_ord_ctx(&nums, v, c.p, k);
		}
		if (r != 1)
			break;
		size_t gn;
		const uint32_t *g = rn_group(&sm, 1, &gn);
		put(&nums, g, gn);
		pos = sm.end[0];
	}
	if (ok)
		put_ordered(o, &nums, w, wn, bn, dot, &c, k, noun_first, keep, 0);
	rn_str_free(&nums);
	return ok;
}

static int render_range(rn_str *o, const rn_match *m, long lv, long rv, const uint32_t *w,
                        size_t wn, int noun_first)
{
	const uint32_t *t = m->s;
	int dot;
	size_t bn = split_punct(w, wn, &dot);
	cnoun c;
	if (!resolve_noun(w, bn, 1, &c))
		return 0;
	int keep = noun_first && agreeing_adjective_precedes(t, m->start[0], c.p);
	int k = keep ? noun_case(c.p) : contextual_case(t, m->n, m->start[0], m->end[0], w, bn, &c);
	rn_str nums = {0};
	put_ord_ctx(&nums, lv, c.p, k);
	putlit(&nums, U" — ");
	put_ord_ctx(&nums, rv, c.p, k);
	put_ordered(o, &nums, w, wn, bn, dot, &c, k, noun_first, keep, 1);
	rn_str_free(&nums);
	return 1;
}

/* ---- the steps ---- */

#define GROUP(name)                                                                      \
	size_t name##_n;                                                                     \
	const uint32_t *name = grp(re, m, #name, &name##_n)

static int whole(const rn_match *m, rn_str *o)
{
	put(o, m->s + m->start[0], m->end[0] - m->start[0]);
	return 0;
}

/* normalize_cyrillic_roman */
static uint32_t cyr_to_lat(uint32_t c)
{
	switch (c) {
	case 0x425: case 0x445: return 'X';   /* Хх */
	case 0x421: case 0x441: return 'C';   /* Сс */
	case 0x406: case 0x456: return 'I';   /* Іі */
	case 0x41C: case 0x43C: return 'M';   /* Мм */
	}
	return c;
}

static int is_lookalike(uint32_t c)
{
	return cyr_to_lat(c) != c;
}

static int is_safe_numeric_hyphen_unit(const uint32_t *w, size_t n)
{
	rn_str key = {0};
	putlower(&key, w, n);
	size_t a = 0, b = key.n;
	while (a < b && key.s[a] == '.')
		a++;
	while (b > a && key.s[b - 1] == '.')
		b--;
	int r = 1;
	if (rn_table_get32(RN_T_NUMERIC_SUFFIXES, key.s ? key.s + a : (const uint32_t *)U"", b - a) ||
	    !rn_unit_resolve(w, n))
		r = 0;
	else if (b - a == 1 && inset(key.s[a], U"глмнрстфч"))
		r = 0;
	rn_str_free(&key);
	return r;
}

static int r_cyrillic(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *w = rn_group(m, 1, &n), *t = m->s;
	if (is_lower_form(w, n) || !is_upper_form(w, n))
		return whole(m, o);
	size_t ndot = 0, nlook = 0, nlat = 0;
	for (size_t i = 0; i < n; i++) {
		ndot += w[i] == '.';
		nlook += is_lookalike(w[i]);
		nlat += w[i] < 0x80 && w[i] != '.';
	}
	rn_str lat = {0};
	if (ndot) {   /* _join_dotted_roman */
		for (size_t i = 0; i < n; i++)
			if (w[i] != '.')
				if (rn_str_putc(&lat, cyr_to_lat(w[i])) < 0)
					fail();
		if (rn_roman_value(lat.s, lat.n) >= 0)
			put(o, lat.s, lat.n);
		else
			put(o, w, n);
		rn_str_free(&lat);
		return 0;
	}
	if (is_safe_numeric_hyphen_unit(w, n)) {
		size_t ln;
		const uint32_t *l = left_slice(t, m->start[0], 32, &ln);
		rn_match lm;
		if (search(P(L1034), l, ln, &lm))
			return whole(m, o);
	}
	if (!nlook)
		return whole(m, o);
	rn_match am;
	if (!nlat && !match_at(P(CENTURY_WORD_AFTER_PATTERN), t, m->n, m->end[0], &am))
		return whole(m, o);
	for (size_t i = 0; i < n; i++)
		if (rn_str_putc(&lat, cyr_to_lat(w[i])) < 0)
			fail();
	if (rn_roman_value(lat.s, lat.n) >= 0)
		put(o, lat.s, lat.n);
	else
		put(o, w, n);
	rn_str_free(&lat);
	return 0;
}

/* convert_shared_roman_century_ranges */
static int r_shared_century(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *re = P(ROMAN_SHARED_CENTURY_RANGE_PATTERN);
	GROUP(left_head);
	GROUP(left);
	GROUP(mid);
	GROUP(right_head);
	GROUP(right);
	GROUP(word);
	long lv = rn_roman_value(left, left_n), rv = rn_roman_value(right, right_n);
	if (lv < 0 || rv < 0)
		return whole(m, o);
	put(o, left_head, left_head_n);
	putlit(o, U" ");
	put_ord(o, lv, rn_g.gent, rn_g.masc, 0, 1);
	int dash = 0;
	for (size_t i = 0; i < mid_n; i++)
		dash |= inset(mid[i], U"—–-");
	if (dash) {
		putlit(o, U" — ");
	} else {
		size_t a, b;
		rn_py_strip(mid, mid_n, &a, &b);
		putlit(o, U" ");
		put(o, mid + a, b - a);
		putlit(o, U" ");
	}
	put(o, right_head, right_head_n);
	putlit(o, U" ");
	put_ord(o, rv, rn_g.gent, rn_g.masc, 0, 1);
	putlit(o, U" ");
	if (lower_eq(word, word_n, U"в.") || lower_eq(word, word_n, U"вв."))
		putlit(o, U"веков");
	else
		put(o, word, word_n);
	return 0;
}

/* convert_roman_century_ranges */
static int r_century(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *re = P(L668);
	GROUP(prep);
	GROUP(left);
	GROUP(mid);
	GROUP(right);
	GROUP(word);
	long lv = rn_roman_value(left, left_n), rv = rn_roman_value(right, right_n);
	if (lv < 0 || rv < 0)
		return whole(m, o);
	int rcase = lower_eq(mid, mid_n, U"до") ? rn_g.gent : rn_g.accs;
	/* _resolve_explicit_roman_context_form(word, case_override, number_override="sing") */
	int dot;
	size_t bn = split_punct(word, word_n, &dot);
	cnoun c;
	if (!resolve_noun(word, bn, 1, &c))
		return whole(m, o);
	put(o, prep, prep_n);
	putlit(o, U" ");
	put_ord_ctx(o, lv, c.p, rn_g.gent);
	putlit(o, U" ");
	put(o, mid, mid_n);
	putlit(o, U" ");
	put_ord_ctx(o, rv, c.p, rcase);
	putlit(o, U" ");
	put_noun_word(o, word, bn, &c, rcase, rn_g.sing);
	if (dot)
		putlit(o, U".");
	return 0;
}

static int range_step(const rn_match *m, rn_str *o, const rn_re *re, int noun_first)
{
	GROUP(left);
	GROUP(right);
	GROUP(word);
	long lv = rn_roman_value(left, left_n), rv = rn_roman_value(right, right_n);
	if (lv < 0 || rv < 0)
		return whole(m, o);
	size_t mark = o->n;
	if (!render_range(o, m, lv, rv, word, word_n, noun_first)) {
		o->n = mark;
		return whole(m, o);
	}
	return 0;
}

static int r_hyphenated(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	return range_step(m, o, P(L725), 0);
}

static int r_left_hyphenated(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	return range_step(m, o, P(L748), 1);
}

static int series_step(const rn_match *m, rn_str *o, const rn_re *re, int noun_first)
{
	GROUP(series);
	GROUP(suffix);
	size_t mark = o->n;
	if (!render_series(o, m, series, series_n, suffix, suffix_n, noun_first)) {
		o->n = mark;
		return whole(m, o);
	}
	return 0;
}

static int r_shared_words(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	return series_step(m, o, P(L579), 0);
}

static int r_left_shared_words(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	return series_step(m, o, P(L596), 1);
}

/* convert_roman_words */
static int r_words(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *re = P(L615);
	const uint32_t *t = m->s;
	size_t st = m->start[0];
	GROUP(roman);
	GROUP(spacing);
	GROUP(word);
	(void)spacing;
	long v = rn_roman_value(roman, roman_n);
	if (v < 0)
		return whole(m, o);
	size_t ln;
	const uint32_t *l = left_slice(t, st, 16, &ln);
	rn_match lm;
	if (search(P(L626), l, ln, &lm))
		return whole(m, o);
	if (!spacing_n) {
		rn_str nt = {0};
		norm_tok(word, word_n, &nt);
		int isv = eq(nt.s, nt.n, U"в");
		rn_str_free(&nt);
		if (!isv)
			return whole(m, o);
	}
	if (spacing_n && (lower_eq(word, word_n, U"в") || lower_eq(word, word_n, U"вв"))) {
		int skip = !rn_py_str_isupper(roman, roman_n);
		if (!skip && governing_noun_precedes(t, st)) {
			l = left_slice(t, st, 40, &ln);
			skip = !search(P(CENTURY_HEAD_BEFORE_PATTERN), l, ln, &lm);
		}
		if (skip)
			return whole(m, o);
	}
	if ((eq(word, word_n, U"в") || eq(word, word_n, U"В")) && animate_name_precedes(t, st))
		return whole(m, o);
	size_t mark = o->n;
	if (!render_single(o, m, v, word, word_n)) {
		o->n = mark;
		return whole(m, o);
	}
	return 0;
}

/* convert_suffixed_roman_regnal_names */
static int r_regnal(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *re = P(L837);
	GROUP(name);
	GROUP(roman);
	long v = rn_roman_value(roman, roman_n);
	if (v < 0)
		return whole(m, o);
	const cmorphy3_form *p = choose_noun(name, name_n, 0);
	if (!p || !rn_has(p->tag, rn_g.anim))
		return whole(m, o);
	const cmorphy3_form *f;
	int c = rn_mparse(name, name_n, &f), named = 0;
	for (int i = 0; i < c && !named; i++)
		named = has_name_marker(f[i].tag);
	if (!named)
		return whole(m, o);
	put(o, name, name_n);
	putlit(o, U" ");
	put_ord_parse(o, v, p, 1);
	return 0;
}

/* convert_suffixed_roman_ordinals */
static int r_suffixed(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *g = rn_group(m, 1, &n);
	long v = rn_roman_value(g, n);
	if (v < 0)
		return whole(m, o);
	putnum(o, v);
	return 0;
}

/* convert_roman_names */
static int name_parse_ok(const cmorphy3_form *f)
{
	return rn_has(f->tag, rn_g.NOUN) && rn_has(f->tag, rn_g.anim) && has_name_marker(f->tag);
}

static int r_names(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *re = P(L833);
	const uint32_t *t = m->s;
	GROUP(name);
	GROUP(roman);
	long v = rn_roman_value(roman, roman_n);
	if (v < 0)
		return whole(m, o);
	/* pick_name_parse */
	const cmorphy3_form *f, *first = NULL;
	int c = rn_mparse(name, name_n, &f);
	for (int i = 0; i < c && !first; i++)
		if (name_parse_ok(&f[i]))
			first = &f[i];
	if (!first && name_n && (name[name_n - 1] == 0x44A || name[name_n - 1] == 0x42A)) {
		c = rn_mparse(name, name_n - 1, &f);
		for (int i = 0; i < c && !first; i++)
			if (name_parse_ok(&f[i]))
				first = &f[i];
	}
	if (!first)
		return whole(m, o);
	const cmorphy3_form *p = first;
	size_t ln;
	const uint32_t *l = left_slice(t, m->start[0], 40, &ln);
	rn_tok *k = NULL;
	long nk = rn_simple_tokenize(l, ln, &k);
	if (nk < 0)
		fail();
	rn_str lw = {0};
	for (long i = nk - 1; i >= 0; i--) {
		int alpha = 0;
		for (size_t j = 0; j < k[i].n && !alpha; j++)
			alpha = rn_py_isalpha(k[i].s[j]);
		if (alpha) {
			norm_tok(k[i].s, k[i].n, &lw);
			break;
		}
	}
	free(k);
	int target = -1;
	const rn_kv *kv = rn_table_get32(RN_T_REGNAL_CASE_CONTEXT,
	                                 lw.s ? lw.s : (const uint32_t *)U"", lw.n);
	if (kv)
		target = cmorphy3_grammeme(rn_morph, kv->value);
	else if (lw.n) {
		const cmorphy3_form *lf;
		rn_mparse(lw.s, lw.n, &lf);
		if (rn_has(lf[0].tag, rn_g.NOUN))
			target = rn_g.gent;
	}
	rn_str_free(&lw);
	if (target >= 0)
		for (int i = 0; i < c; i++)
			if (name_parse_ok(&f[i]) && rn_canon_case(rn_case(f[i].tag)) == target) {
				p = &f[i];
				break;
			}
	int g = rn_gender(p->tag);
	put(o, name, name_n);
	putlit(o, U" ");
	put_ord(o, v, noun_case(p), g >= 0 ? g : rn_g.masc, 0, 0);
	return 0;
}

/* _render_abbreviated_heading */
static int abbreviated_heading(const rn_match *m, rn_str *o, const rn_re *re)
{
	const uint32_t *t = m->s;
	size_t tn = m->n, en = m->end[0];
	GROUP(word);
	GROUP(roman);
	if (!word_n || word[word_n - 1] != '.')
		return whole(m, o);
	rn_str nt = {0};
	norm_tok(word, word_n - 1, &nt);
	const rn_kv *kv = rn_table_get32(RN_T_ROMAN_HEADING_ABBREVIATION_LEMMAS,
	                                 nt.s ? nt.s : (const uint32_t *)U"", nt.n);
	rn_str_free(&nt);
	if (!kv)
		return whole(m, o);
	rn_match pm;
	if (word_n == 2 && roman_n == 1 && en < tn && t[en] == '.' &&
	    !match_at(P(L884), t + en, tn - en < 16 ? tn - en : 16, 0, &pm))
		return whole(m, o);
	const cmorphy3_form *p = choose_noun8(kv->value);
	if (!p)
		return whole(m, o);
	long v = rn_roman_value(roman, roman_n);
	if (v < 0)
		return whole(m, o);
	int k = abbreviated_case(t, tn, m->start[0], en, kv->value, RN_T_HEAD_LOCATIVE_PREPOSITIONS);
	put(o, word, word_n);
	putlit(o, U" ");
	put_ord_ctx(o, v, p, k);
	return 0;
}

/* convert_heading_roman_numerals */
static int r_heading(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *re = P(L906);
	GROUP(word);
	GROUP(roman);
	if (!is_upper_form(roman, roman_n))
		return whole(m, o);
	cnoun c;
	if (!resolve_noun(word, word_n, 0, &c) ||
	    !rn_table_get(RN_T_ROMAN_HEADING_LEMMAS, c.lemma))
		return abbreviated_heading(m, o, re);
	long v = rn_roman_value(roman, roman_n);
	if (v < 0)
		return whole(m, o);
	const cmorphy3_form *p = choose_noun(word, word_n, 1);   /* render_ordinal_from_noun_word */
	put(o, word, word_n);
	putlit(o, U" ");
	if (p)
		put_ord_parse(o, v, p, 0);
	else
		putnum(o, v);
	return 0;
}

/* convert_other_roman_numerals */
static int r_other(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const uint32_t *t = m->s;
	size_t n, tn = m->n, en = m->end[0];
	const uint32_t *w = rn_group(m, 1, &n);
	if (rn_unit_is_exact(w, n)) {
		put(o, w, n);
		return 0;
	}
	size_t ln, gn;
	const uint32_t *l = left_slice(t, m->start[0], 16, &ln), *g;
	rn_match hm;
	if (search(P(L962), l, ln, &hm)) {
		g = rn_group(&hm, 1, &gn);
		if (rn_roman_value(g, gn) < 0)
			return whole(m, o);
	}
	if (match_at(P(L970), t + en, tn - en < 16 ? tn - en : 16, 0, &hm)) {
		g = rn_group(&hm, 1, &gn);
		if (rn_roman_value(g, gn) < 0)
			return whole(m, o);
	}
	rn_str up = {0};
	if (rn_py_upper(w, n, &up) < 0)
		fail();
	long v = -1;
	if (!rn_table_get32(RN_T_ROMAN_ABBREVIATION_EXCEPTIONS, up.s ? up.s : (const uint32_t *)U"",
	                    up.n))
		v = rn_roman_value(up.s, up.n);
	rn_str_free(&up);
	if (v < 0)
		return whole(m, o);
	putnum(o, v);
	return 0;
}

int rn_run_roman(const rn_str *in, rn_str *out)
{
	static const struct {
		enum rn_pattern pat;
		rn_repl_fn fn;
	} STEPS[] = {
		{RN_ROMAN_NUMERALS_L1019, r_cyrillic},
		{RN_ROMAN_NUMERALS_ROMAN_SHARED_CENTURY_RANGE_PATTERN, r_shared_century},
		{RN_ROMAN_NUMERALS_L668, r_century},
		{RN_ROMAN_NUMERALS_L725, r_hyphenated},
		{RN_ROMAN_NUMERALS_L579, r_shared_words},
		{RN_ROMAN_NUMERALS_L748, r_left_hyphenated},
		{RN_ROMAN_NUMERALS_L596, r_left_shared_words},
		{RN_ROMAN_NUMERALS_L615, r_words},
		{RN_ROMAN_NUMERALS_L837, r_regnal},
		{RN_ROMAN_NUMERALS_L941, r_suffixed},
		{RN_ROMAN_NUMERALS_L833, r_names},
		{RN_ROMAN_NUMERALS_L906, r_heading},
		{RN_ROMAN_NUMERALS_L985, r_other},
	};
	rn_mx_trim();
	rn_mx_failed = 0;
	rn_str cur = {0};
	int r = -1;
	if (rn_str_copy(&cur, in) < 0)
		goto done;
	for (size_t i = 0; i < sizeof STEPS / sizeof *STEPS; i++)
		if (rn_resub(rn_pat(STEPS[i].pat), &cur, STEPS[i].fn, NULL) < 0 || rn_mx_failed)
			goto done;
	r = rn_str_put(out, cur.s, cur.n);
done:
	rn_str_free(&cur);
	return r;
}
