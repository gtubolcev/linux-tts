/* run_years of normalizer.py: normalize_years and normalize_numeric_ranges of years.py
 * (years_context.py and the parts of abbreviation_context.py it uses).
 *
 * Each replacement sees the input of its own sub as the text, as the oracle's closures do:
 * m->s, m->n. Years are kept as str(int(...)) digit strings, so long numbers behave as in
 * Python (num2words raises above 10**33 and the digits stay). Errors set rn_mx_failed. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>

#include "rn-mx.h"
#include "rn-ncase.h"
#include "rn-num.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-units.h"
#include "rn-util.h"

#define P(x) rn_pat(RN_YEARS_##x)

static const char32_t PUNCT[] = U".,:;!\"«»()[]{}";   /* PUNCT_STRIP of text_context */

/* ---- strings ---- */

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

static int whole(const rn_match *m, rn_str *o)
{
	put(o, m->s + m->start[0], m->end[0] - m->start[0]);
	return 0;
}

static int eq(const uint32_t *s, size_t n, const char32_t *lit)
{
	size_t i = 0;
	for (; i < n && lit[i]; i++)
		if (s[i] != lit[i])
			return 0;
	return i == n && !lit[i];
}

static int starts(const uint32_t *s, size_t n, const char32_t *lit)
{
	size_t i = 0;
	for (; lit[i]; i++)
		if (i >= n || s[i] != lit[i])
			return 0;
	return 1;
}

static int inset(uint32_t c, const char32_t *set)
{
	for (; *set; set++)
		if (c == *set)
			return 1;
	return 0;
}

/* s equals one of the NULL-terminated literals */
static int oneof(const uint32_t *s, size_t n, const char32_t *const *lits)
{
	for (; *lits; lits++)
		if (eq(s, n, *lits))
			return 1;
	return 0;
}

static int ends_dot(const rn_str *w)
{
	return w->n && w->s[w->n - 1] == '.';
}

/* t = s.lower() */
static void lower(const uint32_t *s, size_t n, rn_str *t)
{
	t->n = 0;
	if (rn_py_lower(s, n, t) < 0)
		fail();
}

/* o = s.lower().strip(PUNCT_STRIP): normalize_context_token */
static void norm_tok(const uint32_t *s, size_t n, rn_str *o)
{
	lower(s, n, o);
	size_t a = 0, b = o->n;
	while (a < b && inset(o->s[a], PUNCT))
		a++;
	while (b > a && inset(o->s[b - 1], PUNCT))
		b--;
	if (a && b > a)
		memmove(o->s, o->s + a, (b - a) * sizeof *o->s);
	o->n = b - a;
}

static const uint32_t *grp(const rn_match *m, const rn_re *p, const char *name, size_t *n)
{
	return rn_group(m, rn_re_group(p, name), n);
}

static size_t gstart(const rn_match *m, const rn_re *p, const char *name)
{
	return m->start[rn_re_group(p, name)];
}

/* pattern.search(s[a:b]); the match offsets are relative to s + a */
static int sl_search(const rn_re *re, const uint32_t *s, size_t a, size_t b, rn_match *m)
{
	int r = rn_search(re, s + a, b - a, 0, m);
	if (r < 0) {
		fail();
		return 0;
	}
	return r;
}

static int sl_match(const rn_re *re, const uint32_t *s, size_t a, size_t b, rn_match *m)
{
	int r = rn_match_at(re, s + a, b - a, 0, m);
	if (r < 0) {
		fail();
		return 0;
	}
	return r;
}

/* pattern.match(s, pos) */
static int match_at(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m)
{
	int r = rn_match_at(re, s, n, pos, m);
	if (r < 0) {
		fail();
		return 0;
	}
	return r;
}

static size_t left(size_t start, size_t k)
{
	return start > k ? start - k : 0;
}

static size_t rightof(size_t end, size_t k, size_t n)
{
	return end + k < n ? end + k : n;
}

/* ---- numbers ---- */

/* str(int(s)) */
typedef struct {
	char b[64];
	char *d;
} ynum;

static void ynum_init(ynum *y, const uint32_t *s, size_t n)
{
	y->d = y->b;
	if (rn_int_str(s, n, y->b, sizeof y->b) >= 0)
		return;
	char *d = malloc(n + 2);
	if (!d || rn_int_str(s, n, d, n + 2) < 0) {
		free(d);
		fail();
		strcpy(y->b, "0");
		return;
	}
	y->d = d;
}

static void ynum_free(ynum *y)
{
	if (y->d != y->b)
		free(y->d);
}

/* the value, LONG_MAX when it is larger */
static long yval(const char *d)
{
	return strlen(d) > 18 ? LONG_MAX : strtol(d, NULL, 10);
}

static long ymod(const char *d, long k)   /* int(d) % k for k = 10, 100, 1000 */
{
	size_t len = strlen(d), w = k == 10 ? 1 : k == 100 ? 2 : 3;
	return strtol(d + (len > w ? len - w : 0), NULL, 10);
}

/* num2words(d, lang="ru", to=..., case=c, plural=pl); 0, -1 where it raises */
static int n2w(rn_str *o, int ordinal, const char *d, enum n2w_case c, int plural)
{
	char buf[4096];
	n2w_opts op = N2W_DEFAULTS;
	op.fixes = rn_n2w_fixes;
	op.gcase = c;
	op.plural = plural;
	int len = ordinal ? n2w_ru_ordinal(d, op, buf, sizeof buf)
	                  : n2w_ru_cardinal(d, op, buf, sizeof buf);
	if (len < 0)
		return -1;
	if ((size_t)len >= sizeof buf) {
		fail();
		return -1;
	}
	put8(o, buf);
	return 0;
}

/* year_to_ordinal_words(year, case, plural) */
static void yow(rn_str *o, const char *d, enum n2w_case c, int plural)
{
	enum n2w_case nc = c == N2W_ACCS && !plural ? N2W_NOMN : c;
	size_t len = strlen(d);
	if (yval(d) >= 10000 && !plural && ymod(d, 1000)) {
		char *high = malloc(len + 1);
		if (!high) {
			fail();
			return;
		}
		memcpy(high, d, len + 1);
		memcpy(high + len - 3, "000", 3);
		/* R12: only the last word of an ordinal is inflected */
		int r = n2w(o, 0, high, rn_fixes & RN_FIX_YEAR_HIGH ? N2W_NOMN : nc, 0);
		free(high);
		if (r == 0) {
			char tail[8];
			snprintf(tail, sizeof tail, "%ld", ymod(d, 1000));
			putlit(o, U" ");
			yow(o, tail, nc, 0);
			return;
		}
	}
	if (n2w(o, 1, d, nc, plural) == 0)
		return;
	/* the oracle then tries the nominative ordinal and inflects its last word; num2words
	 * raises for that too wherever it raised above (only for numbers past 10**33) */
	if (n2w(o, 1, d, N2W_NOMN, 0) == 0)
		return;
	put8(o, d);
}

/* yow of the digits s[0..n) */
static void yow_s(rn_str *o, const uint32_t *s, size_t n, enum n2w_case c, int plural)
{
	ynum y;
	ynum_init(&y, s, n);
	yow(o, y.d, c, plural);
	ynum_free(&y);
}

static long yval_s(const uint32_t *s, size_t n)
{
	ynum y;
	ynum_init(&y, s, n);
	long v = yval(y.d);
	ynum_free(&y);
	return v;
}

static int plausible(long v)
{
	return v >= 1000 && v <= 2100;
}

/* ---- tables ---- */

/* YEAR_SUFFIX_TO_CASE.get(suffix (lower case), "nomn") */
static enum n2w_case suffix_case(const uint32_t *s, size_t n)
{
	static const char32_t *const gent[] = {U"ого", U"го", U"ых", U"х", NULL};
	static const char32_t *const datv[] = {U"ому", U"му", U"м", NULL};
	static const char32_t *const ablt[] = {U"ым", U"ыми", U"ми", NULL};
	return oneof(s, n, gent)   ? N2W_GENT
	       : oneof(s, n, datv) ? N2W_DATV
	       : oneof(s, n, ablt) ? N2W_ABLT
	       : eq(s, n, U"ом")   ? N2W_LOCT
	                           : N2W_NOMN;
}

/* PREPOSITIONS_TO_CASE.get(prep.lower(), dflt) */
static enum n2w_case prep_case(const uint32_t *p, size_t n, enum n2w_case dflt)
{
	static const char32_t *const loct[] = {U"в", U"во", U"о", U"об", NULL};
	static const char32_t *const gent[] = {U"с", U"со", U"до", U"от", U"около", NULL};
	static const char32_t *const accs[] = {U"за", U"на", U"по", NULL};
	rn_str t = {0};
	lower(p, n, &t);
	enum n2w_case c = oneof(t.s, t.n, loct)                             ? N2W_LOCT
	                  : eq(t.s, t.n, U"к") || eq(t.s, t.n, U"ко")       ? N2W_DATV
	                  : oneof(t.s, t.n, gent)                           ? N2W_GENT
	                  : oneof(t.s, t.n, accs)                           ? N2W_ACCS
	                  : eq(t.s, t.n, U"между") || eq(t.s, t.n, U"под") ? N2W_ABLT
	                                                                    : dflt;
	rn_str_free(&t);
	return c;
}

/* YEAR_WORD_TO_CASE.get(wl, "nomn") */
static enum n2w_case word_case(const rn_str *wl)
{
	static const char32_t *const gent[] = {U"года", U"годов", NULL};
	static const char32_t *const loct[] = {U"году", U"годе", U"годах", NULL};
	static const char32_t *const ablt[] = {U"годом", U"года́ми", U"годами", NULL};
	return oneof(wl->s, wl->n, gent)   ? N2W_GENT
	       : oneof(wl->s, wl->n, loct) ? N2W_LOCT
	       : oneof(wl->s, wl->n, ablt) ? N2W_ABLT
	       : eq(wl->s, wl->n, U"годам") ? N2W_DATV
	                                     : N2W_NOMN;
}

/* YEAR_WORD_FORMS[("годы" if plural else "год", case)] */
static void put_form(rn_str *o, int plural, enum n2w_case c)
{
	static const char32_t *const sg[] = {U"год", U"года", U"году", U"год", U"годом", U"году"};
	static const char32_t *const pl[] = {U"годы", U"годов", U"годам", U"годы", U"годами",
	                                     U"годах"};
	putlit(o, plural ? pl[c] : sg[c]);
}

/* the year word: w = word.replace("+", ""), wl = w.lower() */
typedef struct {
	rn_str w, wl;
} yword;

static void yword_set(yword *y, const uint32_t *s, size_t n)
{
	y->w.n = 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] != '+' && rn_str_putc(&y->w, s[i]) < 0)
			fail();
	lower(y->w.s ? y->w.s : RN_U(""), y->w.n, &y->wl);
}

static void yword_free(yword *y)
{
	rn_str_free(&y->w);
	rn_str_free(&y->wl);
}

static int is_g(const yword *y)   /* word_lower in ("г.", "г") */
{
	return eq(y->wl.s, y->wl.n, U"г.") || eq(y->wl.s, y->wl.n, U"г");
}

static int is_plural_abbrev(const yword *y)   /* re.fullmatch(YEAR_PLURAL_ABBREV_REGEX, wl) */
{
	rn_match m;
	int r = rn_fullmatch(P(L880), y->wl.s ? y->wl.s : RN_U(""), y->wl.n, 0, &m);
	if (r < 0)
		fail();
	return r == 1;
}

/* _resolve_explicit_year_word_case(prep, word) */
static enum n2w_case explicit_case(const uint32_t *prep, size_t pn, const yword *y)
{
	if (eq(y->wl.s, y->wl.n, U"году") && prep && pn) {
		rn_str t = {0};
		lower(prep, pn, &t);
		int d = eq(t.s, t.n, U"к") || eq(t.s, t.n, U"ко") || eq(t.s, t.n, U"по");
		rn_str_free(&t);
		return d ? N2W_DATV : N2W_LOCT;
	}
	return word_case(&y->wl);
}

/* ---- context ---- */

static int keep_dot(const rn_match *m)
{
	return rn_keep_terminal_abbr_dot(m->s, m->n, m->end[0]);
}

/* _normalize_era_marker(raw); NULL for None */
static const char32_t *era_text(const uint32_t *raw, size_t n)
{
	rn_str c = {0}, cu = {0}, t = {0};
	const char32_t *r = NULL;
	for (size_t i = 0; i < n; i++)
		if (raw[i] != '.' && !rn_is_space(raw[i]) && rn_str_putc(&c, raw[i]) < 0)
			fail();
	if (rn_py_upper(c.s ? c.s : RN_U(""), c.n, &cu) < 0)
		fail();
	int same = cu.n == c.n && (!c.n || !memcmp(cu.s, c.s, c.n * sizeof *c.s));
	if (same && (eq(cu.s, cu.n, U"BC") || eq(cu.s, cu.n, U"BCE"))) {
		r = U"до нашей эры";
		goto done;
	}
	if (same && (eq(cu.s, cu.n, U"AD") || eq(cu.s, cu.n, U"CE"))) {
		r = U"нашей эры";
		goto done;
	}
	lower(raw, n, &t);
	size_t a, b;
	rn_py_strip(t.s ? t.s : RN_U(""), t.n, &a, &b);
	const uint32_t *l = t.s ? t.s + a : RN_U("");
	size_t ln = b - a;
	rn_match m;
	int k = rn_fullmatch(P(L139), l, ln, 0, &m);
	if (k < 0)
		fail();
	if (k == 1) {
		r = U"до нашей эры";
		goto done;
	}
	k = rn_fullmatch(P(L141), l, ln, 0, &m);
	if (k < 0)
		fail();
	if (k == 1)
		r = U"нашей эры";
	else if (starts(l, ln, U"до нашей эры"))
		r = U"до нашей эры";
	else if (starts(l, ln, U"нашей эры"))
		r = U"нашей эры";
done:
	rn_str_free(&c);
	rn_str_free(&cu);
	rn_str_free(&t);
	return r;
}

/* _should_keep_era_terminal_dot */
static int era_keep_dot(const rn_match *m, const rn_re *p)
{
	size_t en;
	const uint32_t *e = grp(m, p, "era", &en);
	if (!e)
		return 0;
	while (en && rn_py_isspace(e[en - 1]))
		en--;
	if (!en || e[en - 1] != '.')
		return 0;
	int nl = 0;
	size_t i = rn_first_non_ws(m->s, m->n, m->end[0], &nl);
	if (i >= m->n || nl)
		return 1;
	return inset(m->s[i], U".!?…)\"»”]}") || rn_py_isupper(m->s[i]);
}

/* " " + era text and its dot */
static void put_era(rn_str *o, const char32_t *era, const rn_match *m, const rn_re *p)
{
	putlit(o, U" ");
	putlit(o, era);
	if (era_keep_dot(m, p))
		putlit(o, U".");
}

/* the era group: 1 and *era (NULL for None) if absent or known, 0 if unknown */
static int era_of(const rn_match *m, const rn_re *p, const char32_t **era)
{
	size_t en;
	const uint32_t *e = grp(m, p, "era", &en);
	*era = e ? era_text(e, en) : NULL;
	return !e || *era;
}

/* the word just before start: re.search(r"([а-яё]+)\s+$", s[start-k:start], re.I) */
static int word_before(const uint32_t *s, size_t start, size_t k, size_t *wa, size_t *wb)
{
	rn_match m;
	size_t a = left(start, k);
	if (!sl_search(P(L200), s, a, start, &m))
		return 0;
	*wa = a + m.start[1];
	*wb = a + m.end[1];
	return 1;
}

/* select_parse(word, pos=..., required_grammemes=..., case=...): pos2, req*, ccase -1 for
 * none; pos -1 for any */
static const cmorphy3_form *sel(const uint32_t *s, size_t n, int pos, int pos2, int req1,
                                int req2, int ccase)
{
	const cmorphy3_form *f;
	int count = rn_mparse(s, n, &f);
	for (int i = 0; i < count; i++) {
		int ps = rn_pos(f[i].tag);
		if (pos >= 0 && ps != pos && (pos2 < 0 || ps != pos2))
			continue;
		cmorphy3_gset g = rn_canon(rn_gs(f[i].tag));
		if ((req1 >= 0 && !cmorphy3_gset_has(g, req1)) || (req2 >= 0 && !cmorphy3_gset_has(g, req2)))
			continue;
		if (ccase >= 0 && rn_canon_case(rn_case(f[i].tag)) != ccase)
			continue;
		return &f[i];
	}
	return NULL;
}

static int nf_is(const cmorphy3_form *f, const char *const *names)
{
	for (; *names; names++)
		if (!strcmp(f->normal_form, *names))
			return 1;
	return 0;
}

/* _follows_month_name */
static int follows_month_name(const uint32_t *s, size_t start)
{
	static const char32_t *const months[] = {U"января",   U"февраля", U"марта",  U"апреля",
	                                         U"мая",      U"июня",    U"июля",   U"августа",
	                                         U"сентября", U"октября", U"ноября", U"декабря",
	                                         NULL};
	size_t a, b;
	if (!word_before(s, start, 12, &a, &b))
		return 0;
	rn_str t = {0};
	lower(s + a, b - a, &t);
	int r = oneof(t.s, t.n, months);
	rn_str_free(&t);
	return r;
}

/* _month_name_precedes; *mstart = where the month starts */
static int month_name_precedes(const uint32_t *s, size_t start, size_t *mstart)
{
	static const char *const lemmas[] = {"январь", "февраль", "март",     "апрель",
	                                     "май",    "июнь",    "июль",     "август",
	                                     "сентябрь", "октябрь", "ноябрь", "декабрь", NULL};
	size_t a, b;
	if (!word_before(s, start, 14, &a, &b))
		return 0;
	rn_str t = {0};
	lower(s + a, b - a, &t);
	const cmorphy3_form *f = sel(t.s, t.n, rn_g.NOUN, -1, -1, -1, -1);
	rn_str_free(&t);
	*mstart = a;
	return f && nf_is(f, lemmas);
}

/* is_non_year_following_token (token lower case). Its unit loop repeats resolve_unit_form
 * on the same surface, so only the exception lemmas remain of it. */
static int non_year_token(const rn_str *t)
{
	if (rn_unit_resolve_form(t->s, t->n))
		return 1;
	if (eq(t->s, t->n, U"лет"))
		return 1;
	const cmorphy3_form *f;
	int count = rn_mparse(t->s, t->n, &f);
	for (int i = 0; i < count; i++)
		if (rn_pos(f[i].tag) == rn_g.COMP)
			return 1;
	static const char *const exc[] = {"раз", "место", NULL};
	for (int i = 0; i < count; i++)
		if (nf_is(&f[i], exc))
			return 1;
	return 0;
}

/* should_treat_as_implicit_year(text, start, ...) */
static int implicit_year(const uint32_t *s, size_t n, size_t start)
{
	rn_match m;
	if (match_at(P(L489), s, n, start, &m) || match_at(P(L485), s, n, start, &m))
		return 0;
	/* _leading_context_tokens(text, start, limit=4) */
	rn_str tok[4] = {{0}};
	int k = 0;
	rn_iter it = rn_iter_init(rn_pat(RN_TEXT_CONTEXT_TOKEN_PATTERN), s, n, start);
	int r;
	rn_str t = {0};
	while (k < 4 && (r = rn_iter_next(&it, &m)) == 1) {
		const uint32_t *ts = s + m.start[0];
		size_t tn = m.end[0] - m.start[0];
		norm_tok(ts, tn, &t);
		if (!t.n) {   /* token.strip(), then .lower() */
			size_t a, b;
			rn_py_strip(ts, tn, &a, &b);
			lower(ts + a, b - a, &t);
		}
		static const char32_t TRIVIA[] = U",;:.!?…»\"”)]}";
		if (!t.n || (t.n == 1 && inset(t.s[0], TRIVIA)))
			continue;
		rn_str_copy(&tok[k], &t);
		if (!tok[k].s)
			rn_str_put(&tok[k], NULL, 0);
		k++;
	}
	if (k < 4 && r < 0)
		fail();
	rn_str_free(&t);
	int res;
	if (!k) {
		res = 1;
		goto done;
	}
	if (k >= 2 && (eq(tok[0].s, tok[0].n, U"до") || eq(tok[0].s, tok[0].n, U"по")) &&
	    rn_py_str_isdigit(tok[1].s, tok[1].n)) {
		/* _consume_following_number */
		size_t len = tok[1].n;
		int idx = 2;
		while (idx < k && rn_py_str_isdigit(tok[idx].s, tok[idx].n) && tok[idx].n == 3)
			len += tok[idx++].n;
		if (len >= 3 || (idx < k && non_year_token(&tok[idx]))) {
			res = 0;
			goto done;
		}
	}
	res = !non_year_token(&tok[0]);
done:
	for (int i = 0; i < 4; i++)
		rn_str_free(&tok[i]);
	return res;
}

/* the normalized non-empty tokens of s[a:b] (simple_tokenize); their count, -1 on error.
 * out[i] must be freed. */
static long left_tokens(const uint32_t *s, size_t a, size_t b, rn_str **out)
{
	rn_tok *t;
	long k = rn_simple_tokenize(s + a, b - a, &t);
	*out = NULL;
	if (k < 0) {
		fail();
		return -1;
	}
	rn_str *o = calloc(k ? (size_t)k : 1, sizeof *o);
	if (!o) {
		free(t);
		fail();
		return -1;
	}
	long j = 0;
	for (long i = 0; i < k; i++) {
		norm_tok(t[i].s, t[i].n, &o[j]);
		if (o[j].n)
			j++;
	}
	free(t);
	*out = o;
	return j;   /* entries past j may hold buffers too: free k of them */
}

static void free_tokens(rn_str *t, long k)
{
	if (!t)
		return;
	for (long i = 0; i < k; i++)
		rn_str_free(&t[i]);
	free(t);
}

/* has_mass_measurement_context */
static int mass_context(const uint32_t *s, size_t start)
{
	rn_str *t;
	size_t a = left(start, 40);
	long all = 0;
	{
		rn_tok *tt;
		all = rn_simple_tokenize(s + a, start - a, &tt);
		if (all >= 0)
			free(tt);
	}
	long k = left_tokens(s, a, start, &t);
	int r = 0;
	static const char *const mass[] = {"вес", "масса", NULL};
	for (long i = k - 1; i >= 0 && i >= k - 3; i--) {
		const cmorphy3_form *f;
		int count = rn_mparse(t[i].s, t[i].n, &f);
		const cmorphy3_form *c = &f[0];
		for (int j = 0; j < count; j++)
			if (rn_has(f[j].tag, rn_g.NOUN)) {
				c = &f[j];
				break;
			}
		if (nf_is(c, mass)) {
			r = 1;
			break;
		}
	}
	free_tokens(t, all > 0 ? all : 0);
	return r;
}

static int word_char(uint32_t c)   /* _is_word_character */
{
	return rn_py_isalnum(c) || c == '-' || c == '\'' || c == 0x2019;
}

/* measuring_instruction_precedes */
static int measuring_instruction(const uint32_t *s, size_t start)
{
	size_t a = left(start, 60);
	if (a > 0 && word_char(s[a - 1]))
		while (a < start && word_char(s[a]))
			a++;
	for (size_t i = start; i > a; i--)
		if (inset(s[i - 1], U".!?;:")) {
			a = i;
			break;
		}
	rn_tok *t;
	long k = rn_simple_tokenize(s + a, start - a, &t);
	if (k < 0) {
		fail();
		return 0;
	}
	int r = 0;
	rn_str w = {0};
	for (long i = 0; i < k && !r; i++) {
		norm_tok(t[i].s, t[i].n, &w);
		if (!w.n)
			continue;
		const cmorphy3_form *f;
		int count = rn_mparse(w.s, w.n, &f);
		for (int j = 0; j < count; j++)
			if (rn_has(f[j].tag, rn_g.impr))
				r = 1;
	}
	rn_str_free(&w);
	free(t);
	return r;
}

/* allows_short_abbreviated_year */
static int short_year_ok(const uint32_t *s, size_t n, size_t end, const uint32_t *prep, size_t pn)
{
	if (prep && pn)
		return 1;
	size_t i = rn_first_non_ws(s, n, end, NULL);
	if (i >= n)
		return 0;
	rn_match m;
	if (!match_at(rn_pat(RN_ABBREVIATION_CONTEXT_SHORT_ABBREVIATED_YEAR_TOKEN_PATTERN), s, n, i, &m))
		return 0;
	rn_str t = {0};
	lower(s + m.start[0], m.end[0] - m.start[0], &t);
	int r = starts(t.s, t.n, U"рожд");
	rn_str_free(&t);
	return r;
}

/* the next word of s[end:end+k] by L244 (r"\s+([а-яё]+)") */
static int next_word(const uint32_t *s, size_t n, size_t end, size_t k, const uint32_t **w,
                     size_t *wn)
{
	rn_match m;
	if (!sl_match(P(L244), s, end, rightof(end, k, n), &m))
		return 0;
	*w = rn_group(&m, 1, wn);
	return 1;
}

/* _genitive_mass_noun_follows */
static int genitive_mass_noun(const uint32_t *s, size_t n, size_t end)
{
	const uint32_t *w;
	size_t wn;
	if (!next_word(s, n, end, 20, &w, &wn))
		return 0;
	const cmorphy3_form *f;
	int count = rn_mparse(w, wn, &f);
	for (int i = 0; i < count; i++) {
		int ps = rn_pos(f[i].tag);
		if (ps == rn_g.PREP || ps == rn_g.CONJ || ps == rn_g.PRCL || ps == rn_g.INTJ)
			return 0;
	}
	const cmorphy3_form *p = sel(w, wn, rn_g.NOUN, -1, -1, -1, -1);
	return p && rn_case(p->tag) == rn_g.gent && !rn_has(p->tag, rn_g.anim);
}

/* _mass_construction_wins */
static int mass_wins(const uint32_t *s, size_t n, size_t start, size_t end, const uint32_t *prep,
                     size_t pn)
{
	if (!genitive_mass_noun(s, n, end))
		return 0;
	if (prep && pn) {
		rn_str t = {0};
		lower(prep, pn, &t);
		int pod = eq(t.s, t.n, U"под");
		rn_str_free(&t);
		if (pod)
			return 1;
	}
	return measuring_instruction(s, start);
}

/* _surname_subject_follows */
static int surname_follows(const uint32_t *s, size_t n, size_t end)
{
	rn_match m;
	if (!match_at(P(SURNAME_SUBJECT_PATTERN), s, n, end, &m))
		return 0;
	size_t wn;
	const uint32_t *w = rn_group(&m, 1, &wn);
	const cmorphy3_form *f;
	int count = rn_mparse(w, wn, &f);
	for (int i = 0; i < count; i++)
		if (rn_has(f[i].tag, rn_g.Surn) && rn_has(f[i].tag, rn_g.nomn) &&
		    rn_has(f[i].tag, rn_g.sing))
			return 1;
	return 0;
}

/* _counted_word_follows */
static int counted_word_follows(const uint32_t *s, size_t n, size_t end)
{
	rn_match m;
	if (!match_at(P(NEXT_WORD), s, n, end, &m))
		return 0;
	size_t wn;
	const uint32_t *w = rn_group(&m, 1, &wn);
	const cmorphy3_form *f = rn_mfirst(w, wn);
	int ps = rn_pos(f->tag);
	return (ps == rn_g.NOUN || ps == rn_g.ADJF) && rn_case(f->tag) == rn_g.gent &&
	       rn_number(f->tag) == rn_g.plur;
}

/* ---- the replacements ---- */

/* " " + the year word as YEAR_WORD_FORMS gives it for an abbreviation, else as written;
 * with the abbreviation's dot when it stays */
static void put_prep(rn_str *o, const uint32_t *prep, size_t pn)
{
	if (prep && pn) {
		put(o, prep, pn);
		putlit(o, U" ");
	}
}

static int r_range_decade(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L443);
	size_t pn, y1n, sn, y2n, xn, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *y1 = grp(m, p, "year1", &y1n);
	const uint32_t *sep = grp(m, p, "separator", &sn), *y2 = grp(m, p, "year2", &y2n);
	const uint32_t *suf = grp(m, p, "suffix", &xn), *w = grp(m, p, "word", &wn);
	if (eq(sep, sn, U",") && !(w && wn))
		return whole(m, o);
	rn_str t = {0};
	lower(suf, xn, &t);
	enum n2w_case c = suffix_case(t.s, t.n);
	rn_str_free(&t);
	put_prep(o, prep, pn);
	yow_s(o, y1, y1n, c, 1);
	putlit(o, U" — ");
	yow_s(o, y2, y2n, c, 1);
	if (w && wn) {
		yword y = {0};
		yword_set(&y, w, wn);
		putlit(o, U" ");
		put(o, y.w.s, y.w.n);
		yword_free(&y);
	}
	return 0;
}

static int r_s_po(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L447);
	size_t pn, mn, y1n, y2n, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *mid = grp(m, p, "mid", &mn);
	const uint32_t *y1 = grp(m, p, "year1", &y1n), *y2 = grp(m, p, "year2", &y2n);
	const uint32_t *w = grp(m, p, "word", &wn);
	const char32_t *era;
	if (!era_of(m, p, &era))
		return whole(m, o);
	int has_word = w && wn;
	if (!era && !has_word && !(plausible(yval_s(y1, y1n)) && plausible(yval_s(y2, y2n))))
		return whole(m, o);
	if (!era && !has_word && !implicit_year(m->s, m->n, m->end[0]))
		return whole(m, o);
	rn_str t = {0};
	lower(mid, mn, &t);
	enum n2w_case c2 = eq(t.s, t.n, U"по") ? N2W_ACCS : N2W_GENT;
	rn_str_free(&t);
	put(o, prep, pn);
	putlit(o, U" ");
	yow_s(o, y1, y1n, N2W_GENT, 0);
	putlit(o, U" ");
	put(o, mid, mn);
	putlit(o, U" ");
	yow_s(o, y2, y2n, c2, 0);
	yword y = {0};
	if (has_word)
		yword_set(&y, w, wn);
	if (has_word && y.w.n) {
		rn_str inf = {0};
		static const char32_t *const pls[] = {U"годы", U"годов", U"годам", U"годами", U"годах",
		                                      NULL};
		if (is_g(&y))
			put_form(&inf, 0, c2);
		else if (is_plural_abbrev(&y))
			put_form(&inf, 1, c2);
		else if (rn_fixes & RN_FIX_YEAR_RANGE_WORD && oneof(y.wl.s, y.wl.n, pls))
			put_form(&inf, 0, c2);   /* R15: "по 2001 год", not "годы" */
		else
			put(&inf, y.w.s, y.w.n);
		if (keep_dot(m) && ends_dot(&y.w) && !ends_dot(&inf))
			putlit(&inf, U".");
		putlit(o, U" ");
		put(o, inf.s, inf.n);
		rn_str_free(&inf);
	} else if (era) {
		putlit(o, U" ");
		put_form(o, 0, c2);
	}
	yword_free(&y);
	if (era)
		put_era(o, era, m, p);
	return 0;
}

static int r_range(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L451);
	size_t pn, y1n, y2n, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *y1 = grp(m, p, "year1", &y1n);
	const uint32_t *y2 = grp(m, p, "year2", &y2n), *w = grp(m, p, "word", &wn);
	const char32_t *era;
	if (!era_of(m, p, &era))
		return whole(m, o);
	yword y = {0};
	yword_set(&y, w, wn);
	int pl = is_plural_abbrev(&y);
	int abbrev = eq(y.wl.s, y.wl.n, U"г.") || pl;
	if (!prep && pl && plausible(yval_s(y1, y1n)) && plausible(yval_s(y2, y2n))) {
		putlit(o, U"с ");
		yow_s(o, y1, y1n, N2W_GENT, 0);
		putlit(o, U" по ");
		yow_s(o, y2, y2n, N2W_ACCS, 0);
		putlit(o, U" год");
		if (era)
			put_era(o, era, m, p);
		else if (ends_dot(&y.w) && keep_dot(m))
			putlit(o, U".");
		yword_free(&y);
		return 0;
	}
	enum n2w_case c = y.w.n && !abbrev ? word_case(&y.wl)
	                  : prep           ? prep_case(prep, pn, N2W_NOMN)
	                                   : N2W_NOMN;
	put_prep(o, prep, pn);
	yow_s(o, y1, y1n, c, 0);
	putlit(o, U" — ");
	yow_s(o, y2, y2n, c, 0);
	if (y.w.n) {
		putlit(o, U" ");
		if (abbrev) {
			put_form(o, 1, c);
			/* the forms never end with a dot */
			if (!era && ends_dot(&y.w) && keep_dot(m))
				putlit(o, U".");
		} else {
			put(o, y.w.s, y.w.n);
		}
	}
	if (era)
		put_era(o, era, m, p);
	yword_free(&y);
	return 0;
}

static int r_multiple(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L455);
	size_t pn, yn, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *ys = grp(m, p, "years", &yn);
	const uint32_t *w = grp(m, p, "word", &wn);
	yword y = {0};
	yword_set(&y, w, wn);
	enum n2w_case c;
	int plural_word = 1;   /* word_norm "годы"; else the word itself */
	if (is_plural_abbrev(&y)) {
		c = prep ? prep_case(prep, pn, N2W_NOMN) : N2W_NOMN;
	} else {
		c = word_case(&y.wl);
		if (prep && c == N2W_NOMN)
			c = prep_case(prep, pn, c);
		plural_word = starts(y.wl.s, y.wl.n, U"год");
	}
	put_prep(o, prep, pn);
	/* re.split(r"(\s*,\s*|\s+и\s+)", years, flags=re.I): numbers between the separators */
	rn_iter it = rn_iter_init(P(L734), ys, yn, 0);
	rn_match sm;
	size_t pos = 0;
	int more = 1;
	while (more) {
		int k = rn_iter_next(&it, &sm);
		if (k < 0) {
			fail();
			k = 0;
		}
		more = k == 1;
		size_t a = pos, b = more ? sm.start[0] : yn;
		size_t sa, sb;
		rn_py_strip(ys + a, b - a, &sa, &sb);
		if (sb > sa) {
			if (rn_py_str_isdigit(ys + a + sa, sb - sa))
				yow_s(o, ys + a + sa, sb - sa, c, 0);
			else
				put(o, ys + a, b - a);
		}
		if (more) {
			size_t ss, se;
			rn_py_strip(ys + sm.start[0], sm.end[0] - sm.start[0], &ss, &se);
			if (se > ss)
				put(o, ys + sm.start[0], sm.end[0] - sm.start[0]);
			pos = sm.end[0];
		}
	}
	putlit(o, U" ");
	if (plural_word)
		put_form(o, 1, c);
	else
		put(o, y.w.s, y.w.n);   /* YEAR_WORD_FORMS has no entry for other words */
	/* R13: the dot of "гг." at the end of a sentence */
	if (rn_fixes & RN_FIX_YEAR_LIST_DOT && plural_word && ends_dot(&y.w) && is_plural_abbrev(&y) &&
	    keep_dot(m))
		putlit(o, U".");
	yword_free(&y);
	return 0;
}

static int r_dual(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L463);
	size_t pn, y1n, y2n, on, cn, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *y1 = grp(m, p, "year1", &y1n);
	const uint32_t *y2 = grp(m, p, "year2", &y2n), *op = grp(m, p, "open", &on);
	const uint32_t *cl = grp(m, p, "close", &cn), *w = grp(m, p, "word", &wn);
	if (!plausible(yval_s(y2, y2n)))
		return whole(m, o);
	yword y = {0};
	yword_set(&y, w, wn);
	enum n2w_case c = explicit_case(prep, pn, &y);
	if (!prep && follows_month_name(m->s, m->start[0]))
		c = N2W_GENT;
	put_prep(o, prep, pn);
	yow_s(o, y1, y1n, c, 0);
	putlit(o, U" ");
	put(o, op, on);
	yow_s(o, y2, y2n, c, 0);
	put(o, cl, cn);
	putlit(o, U" ");
	if (starts(y.w.s, y.w.n, U"г."))
		put_form(o, 0, c);
	else
		put(o, y.w.s, y.w.n);
	yword_free(&y);
	return 0;
}

static int r_paren(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L469);
	size_t yn, on, cn;
	const uint32_t *yr = grp(m, p, "year", &yn), *op = grp(m, p, "open", &on);
	const uint32_t *cl = grp(m, p, "close", &cn);
	ynum y;
	ynum_init(&y, yr, yn);
	if (!plausible(yval(y.d))) {
		ynum_free(&y);
		return whole(m, o);
	}
	rn_str *t;
	size_t a = left(m->start[0], 40);
	rn_tok *tt;
	long all = rn_simple_tokenize(m->s + a, m->start[0] - a, &tt);
	if (all >= 0)
		free(tt);
	long k = left_tokens(m->s, a, m->start[0], &t);
	int ordinal = k >= 2 && (eq(t[k - 2].s, t[k - 2].n, U"под") || eq(t[k - 2].s, t[k - 2].n, U"при"));
	free_tokens(t, all > 0 ? all : 0);
	put(o, op, on);
	if (ordinal)
		yow(o, y.d, N2W_NOMN, 0);
	else if (n2w(o, 0, y.d, N2W_NOMN, 0) < 0)
		put8(o, y.d);
	put(o, cl, cn);
	ynum_free(&y);
	return 0;
}

/* the case of an era year or range by its word or preposition */
static enum n2w_case era_case(const uint32_t *prep, size_t pn, const yword *y, int has_word)
{
	if (has_word)
		return is_g(y) ? (prep ? prep_case(prep, pn, N2W_NOMN) : N2W_NOMN)
		               : explicit_case(prep, pn, y);
	return prep ? prep_case(prep, pn, N2W_NOMN) : N2W_NOMN;
}

static void era_word_form(rn_str *o, const yword *y, int has_word, enum n2w_case c)
{
	if (has_word && !is_g(y))
		put(o, y->w.s, y->w.n);
	else
		put_form(o, 0, c);
}

static int r_era_range(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L477);
	size_t pn, y1n, y2n, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *y1 = grp(m, p, "year1", &y1n);
	const uint32_t *y2 = grp(m, p, "year2", &y2n), *w = grp(m, p, "word", &wn);
	const char32_t *era;
	if (!era_of(m, p, &era) || !era)
		return whole(m, o);
	yword y = {0};
	int hw = w && wn;
	if (hw)
		yword_set(&y, w, wn);
	hw = hw && y.w.n;
	enum n2w_case c = era_case(prep, pn, &y, hw);
	put_prep(o, prep, pn);
	yow_s(o, y1, y1n, c, 0);
	putlit(o, U" — ");
	yow_s(o, y2, y2n, c, 0);
	putlit(o, U" ");
	era_word_form(o, &y, hw, c);
	putlit(o, U" ");
	putlit(o, era);
	if (era_keep_dot(m, p))
		putlit(o, U".");
	yword_free(&y);
	return 0;
}

static int r_era_year(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L473);
	size_t pn, yn, sn, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *yr = grp(m, p, "year", &yn);
	const uint32_t *suf = grp(m, p, "suffix", &sn), *w = grp(m, p, "word", &wn);
	const char32_t *era;
	if (!era_of(m, p, &era) || !era)
		return whole(m, o);
	yword y = {0};
	int hw = w && wn;
	if (hw)
		yword_set(&y, w, wn);
	hw = hw && y.w.n;
	enum n2w_case c;
	if (suf && sn) {
		rn_str t = {0};
		lower(suf, sn, &t);
		c = eq(t.s, t.n, U"м") ? N2W_LOCT : suffix_case(t.s, t.n);
		rn_str_free(&t);
	} else {
		c = era_case(prep, pn, &y, hw);
	}
	put_prep(o, prep, pn);
	yow_s(o, yr, yn, c, 0);
	putlit(o, U" ");
	era_word_form(o, &y, hw, c);
	putlit(o, U" ");
	putlit(o, era);
	if (era_keep_dot(m, p))
		putlit(o, U".");
	yword_free(&y);
	return 0;
}

/* infer_suffix_case */
static enum n2w_case infer_suffix_case(const rn_match *m, const rn_str *suffix)
{
	enum n2w_case dflt = suffix_case(suffix->s, suffix->n);
	int x = eq(suffix->s, suffix->n, U"х"), em = eq(suffix->s, suffix->n, U"м");
	if (!x && !em)
		return dflt;
	rn_match pm;
	size_t a = left(m->start[0], 30);
	if (!sl_search(P(L580), m->s, a, m->start[0], &pm))
		return dflt;
	const rn_re *p = P(L580);
	size_t pn;
	const uint32_t *pr = grp(&pm, p, "prep", &pn);
	rn_str t = {0};
	lower(pr, pn, &t);
	static const char32_t *const loc[] = {U"в", U"во", U"о", U"об", U"на", NULL};
	static const char32_t *const gen[] = {U"с", U"со", U"до", U"от", U"из", NULL};
	static const char32_t *const dat[] = {U"к", U"ко", U"по", NULL};
	enum n2w_case c = dflt;
	if (oneof(t.s, t.n, loc))
		c = N2W_LOCT;
	else if (x && oneof(t.s, t.n, gen))
		c = N2W_GENT;
	else if (em && oneof(t.s, t.n, dat))
		c = N2W_DATV;
	rn_str_free(&t);
	return c;
}

static int r_suffix(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t yn, sn, wn;
	const uint32_t *yr = rn_group(m, 1, &yn), *suf = rn_group(m, 2, &sn);
	const uint32_t *w = rn_group(m, 3, &wn);
	rn_str sx = {0};
	lower(suf, sn, &sx);
	yword y = {0};
	int hw = w && wn;
	if (hw)
		yword_set(&y, w, wn);
	hw = hw && y.w.n;
	enum n2w_case c = infer_suffix_case(m, &sx);
	static const char32_t *const plurals[] = {U"е", U"х", U"м", U"ми", U"ые", U"ых", U"ыми", NULL};
	int em = eq(sx.s, sx.n, U"м");
	int plural = oneof(sx.s, sx.n, plurals) && !(em && c == N2W_LOCT);
	ynum yv;
	ynum_init(&yv, yr, yn);
	long v = yval(yv.d);
	char dec[80];
	const char *d = yv.d;
	int out = 1;
	if (plural) {
		if ((v < 100 && !hw) || (!hw && !plausible(v)))
			out = 0;
		else if (v >= 10 && !(em && ymod(yv.d, 10))) {
			/* (year // 10) * 10 */
			size_t len = strlen(yv.d);
			if (len < sizeof dec) {
				memcpy(dec, yv.d, len + 1);
				dec[len - 1] = '0';
				d = dec;
			} else {
				char *big = yv.d;   /* a long number: change its last digit in place */
				big[len - 1] = '0';
			}
		}
	} else if (!hw && !plausible(v)) {
		out = 0;
	}
	if (!out) {
		ynum_free(&yv);
		yword_free(&y);
		rn_str_free(&sx);
		return whole(m, o);
	}
	yow(o, d, c, plural);
	if (hw) {
		putlit(o, U" ");
		if (is_g(&y))
			put_form(o, 0, c);
		else if (is_plural_abbrev(&y))
			put_form(o, 1, c);
		else
			put(o, y.w.s, y.w.n);
		if (ends_dot(&y.w) && keep_dot(m))
			putlit(o, U".");
	}
	ynum_free(&yv);
	yword_free(&y);
	rn_str_free(&sx);
	return 0;
}

static int r_ot_do(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L490);
	size_t pn, mn, y1n, y2n;
	const uint32_t *prep = grp(m, p, "prep", &pn), *mid = grp(m, p, "mid", &mn);
	const uint32_t *y1 = grp(m, p, "year1", &y1n), *y2 = grp(m, p, "year2", &y2n);
	if (!(plausible(yval_s(y1, y1n)) && plausible(yval_s(y2, y2n))))
		return whole(m, o);
	if (!implicit_year(m->s, m->n, m->end[0]))
		return whole(m, o);
	rn_str t = {0};
	lower(mid, mn, &t);
	enum n2w_case c2 = eq(t.s, t.n, U"по") ? N2W_ACCS : N2W_GENT;
	rn_str_free(&t);
	put(o, prep, pn);
	putlit(o, U" ");
	yow_s(o, y1, y1n, N2W_GENT, 0);
	putlit(o, U" ");
	put(o, mid, mn);
	putlit(o, U" ");
	yow_s(o, y2, y2n, c2, 0);
	return 0;
}

static int r_prep_implicit(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L494);
	size_t pn, yn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *yr = grp(m, p, "year", &yn);
	if (!implicit_year(m->s, m->n, m->end[0]))
		return whole(m, o);
	rn_str t = {0};
	lower(prep, pn, &t);
	int v = eq(t.s, t.n, U"в") || eq(t.s, t.n, U"во");
	if (!v && counted_word_follows(m->s, m->n, m->end[0])) {
		rn_str_free(&t);
		return whole(m, o);
	}
	static const char32_t *const loc[] = {U"в", U"во", U"о", U"об", NULL};
	static const char32_t *const dat[] = {U"к", U"ко", U"по", NULL};
	static const char32_t *const gen[] = {U"с", U"со", U"до", U"от", U"из", NULL};
	static const char32_t *const acc[] = {U"на", U"за", U"под", NULL};
	enum n2w_case c = oneof(t.s, t.n, loc)   ? N2W_LOCT
	                  : oneof(t.s, t.n, dat) ? N2W_DATV
	                  : oneof(t.s, t.n, gen) ? N2W_GENT
	                  : oneof(t.s, t.n, acc) ? N2W_ACCS
	                                         : N2W_NOMN;
	rn_str_free(&t);
	put(o, prep, pn);
	putlit(o, U" ");
	yow_s(o, yr, yn, c, 0);
	return 0;
}

static int r_split(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L499);
	size_t pn, y1n, y2n, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *y1 = grp(m, p, "year1", &y1n);
	const uint32_t *y2 = grp(m, p, "year2", &y2n), *w = grp(m, p, "word", &wn);
	long v1 = yval_s(y1, y1n), v2 = yval_s(y2, y2n);
	if ((v1 + 1) % 100 != v2)
		return whole(m, o);
	yword y = {0};
	yword_set(&y, w, wn);
	int pl = is_plural_abbrev(&y), abbrev = pl || is_g(&y);
	enum n2w_case c = !abbrev ? word_case(&y.wl) : prep ? prep_case(prep, pn, N2W_NOMN) : N2W_GENT;
	put_prep(o, prep, pn);
	yow_s(o, y1, y1n, c, 0);
	putlit(o, U" — ");
	yow_s(o, y2, y2n, c, 0);
	putlit(o, U" ");
	if (abbrev) {
		put_form(o, pl, c);
		if (ends_dot(&y.w) && keep_dot(m))
			putlit(o, U".");
	} else {
		put(o, y.w.s, y.w.n);
	}
	yword_free(&y);
	return 0;
}

static int r_leta(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L504);
	size_t wn, yn;
	const uint32_t *w = grp(m, p, "word", &wn), *yr = grp(m, p, "year", &yn);
	long v = yval_s(yr, yn);
	if (!(plausible(v) || (v >= 5500 && v <= 7600)))
		return whole(m, o);
	if (rn_counted_noun_follows(m->s, m->n, m->start[0], m->end[0], v))
		return whole(m, o);
	put(o, w, wn);
	putlit(o, U" ");
	yow_s(o, yr, yn, N2W_GENT, 0);
	return 0;
}

static int r_month_year(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L509);
	size_t yn, ms = 0;
	const uint32_t *yr = grp(m, p, "year", &yn);
	if (!month_name_precedes(m->s, m->start[0], &ms))
		return whole(m, o);
	long v = yval_s(yr, yn);
	if (rn_counted_noun_follows(m->s, m->n, ms, m->end[0], v))
		return whole(m, o);
	yow_s(o, yr, yn, N2W_GENT, 0);
	return 0;
}

static int r_biblio(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L520);
	size_t pn, yn;
	const uint32_t *pl = grp(m, p, "place", &pn), *yr = grp(m, p, "year", &yn);
	put(o, pl, pn);
	yow_s(o, yr, yn, N2W_NOMN, 0);
	return 0;
}

static int r_before_month(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t yn;
	const uint32_t *yr = grp(m, P(L515), "year", &yn);
	yow_s(o, yr, yn, N2W_GENT, 0);
	putlit(o, U" года");
	return 0;
}

static int r_with_word(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(L481);
	size_t pn, yn, wn;
	const uint32_t *prep = grp(m, p, "prep", &pn), *yr = grp(m, p, "year", &yn);
	const uint32_t *w = grp(m, p, "word", &wn);
	yword y = {0};
	yword_set(&y, w, wn);
	int abbrev, plural;
	if (is_g(&y)) {
		abbrev = 1;
		plural = 0;
	} else if (is_plural_abbrev(&y)) {
		abbrev = 1;
		plural = 1;
	} else {
		static const char32_t *const pls[] = {U"годы", U"годов", U"годам", U"годами", U"годах", NULL};
		abbrev = 0;
		plural = oneof(y.wl.s, y.wl.n, pls);
	}
	ynum yv;
	ynum_init(&yv, yr, yn);
	long v = yval(yv.d), t2 = ymod(yv.d, 100), ld = ymod(yv.d, 10);
	const uint32_t *wl = y.wl.s;
	size_t wln = y.wl.n;
	int counting = ((eq(wl, wln, U"год") || eq(wl, wln, U"годом")) && ld == 1 && t2 != 11) ||
	               (eq(wl, wln, U"года") && ld >= 1 && ld <= 4 && !(t2 >= 11 && t2 <= 14));
	size_t ys = gstart(m, p, "year");
	int dec_tail = ys >= 2 && (m->s[ys - 1] == ',' || m->s[ys - 1] == '.') &&
	               rn_py_isdigit(m->s[ys - 2]);
	int naming = eq(wl, wln, U"году") ||
	             ((eq(wl, wln, U"год") || eq(wl, wln, U"года") || eq(wl, wln, U"годом")) &&
	              !counting && !dec_tail);
	const uint32_t *s = m->s;
	size_t n = m->n;
	int skip = (!naming && v < 1000 && !abbrev) ||
	           (abbrev && mass_context(s, m->start[0])) ||
	           (abbrev && mass_wins(s, n, m->start[0], m->end[0], prep, pn)) ||
	           (abbrev && v < 1000 && !short_year_ok(s, n, m->end[0], prep, pn));
	if (skip) {
		ynum_free(&yv);
		yword_free(&y);
		return whole(m, o);
	}
	enum n2w_case c = !abbrev ? explicit_case(prep, pn, &y)
	                  : prep  ? prep_case(prep, pn, N2W_NOMN)
	                          : N2W_GENT;
	put_prep(o, prep, pn);
	yow(o, yv.d, c, plural);
	putlit(o, U" ");
	if (abbrev) {
		put_form(o, plural, c);
		if (ends_dot(&y.w) && keep_dot(m) && !surname_follows(s, n, m->end[0]))
			putlit(o, U".");
	} else {
		put(o, y.w.s, y.w.n);
	}
	ynum_free(&yv);
	yword_free(&y);
	return 0;
}

/* _gospoda_case_around: a canonical case grammeme, -1 for None */
static int gospoda_case(const uint32_t *s, size_t n, size_t start, size_t end)
{
	/* re.search(r"([а-яёА-ЯЁ]+)\s+$", text[:start]): the letters before the spaces */
	size_t b = start;
	while (b && rn_is_space(s[b - 1]))
		b--;
	if (b == start)
		return -1;
	size_t a = b;
#define CYR(c) (((c) >= 0x430 && (c) <= 0x44F) || (c) == 0x451 || ((c) >= 0x410 && (c) <= 0x42F) || (c) == 0x401)
	while (a && CYR(s[a - 1]))
		a--;
#undef CYR
	if (a == b)
		return -1;
	rn_match am;
	if (!sl_match(P(L319), s, end, rightof(end, 40, n), &am))
		return -1;
	size_t fn, sn;
	const uint32_t *first = rn_group(&am, 1, &fn), *second = rn_group(&am, 2, &sn);
	const uint32_t *mod = NULL;
	size_t modn = 0;
	const cmorphy3_form *head = sel(first, fn, rn_g.NOUN, -1, rn_g.anim, rn_g.plur, -1);
	if (!head && second) {
		head = sel(second, sn, rn_g.NOUN, -1, rn_g.anim, rn_g.plur, -1);
		mod = first;
		modn = fn;
	}
	if (!head)
		return -1;
	int c = rn_canon_case(rn_case(head->tag));
	if (c != rn_g.gent && c != rn_g.datv && c != rn_g.accs && c != rn_g.ablt && c != rn_g.loct)
		return -1;
	if (mod && !sel(mod, modn, rn_g.ADJF, -1, rn_g.plur, -1, c))
		return -1;
	if (!sel(s + a, b - a, rn_g.ADJF, rn_g.NPRO, rn_g.plur, -1, c))
		return -1;
	return c;
}

static int r_orphan(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	if (rn_fixes & RN_FIX_YEAR_GLUED && m->start[0] && rn_py_isdecimal(m->s[m->start[0] - 1]))
		return whole(m, o);   /* R14: glued to a number the oracle did not read */
	int c = gospoda_case(m->s, m->n, m->start[0], m->end[0]);
	if (c >= 0) {
		const cmorphy3_form *f = sel(RN_U("господа"), 7, rn_g.NOUN, -1, rn_g.plur, -1, -1);
		cmorphy3_form inf;
		if (f && rn_minflect(f, rn_gset_or(rn_gset1(c), rn_gset1(rn_g.plur)), &inf)) {
			put8(o, inf.word);
			return 0;
		}
	}
	putlit(o, U"годы");
	return 0;
}

/* ---- normalize_numeric_ranges ---- */

/* _inverted_quantity_precedes */
static int inverted_quantity(const uint32_t *s, size_t start)
{
	rn_match m;
	size_t a = left(start, 24);
	if (!sl_search(P(INVERTED_QUANTITY_TAIL_PATTERN), s, a, start, &m))
		return 0;
	size_t wn;
	const uint32_t *w = rn_group(&m, 1, &wn);
	const cmorphy3_form *f = sel(w, wn, rn_g.NOUN, -1, -1, -1, -1);
	return f && rn_case(f->tag) == rn_g.gent && rn_number(f->tag) == rn_g.plur;
}

static int r_numeric_range(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t an, bn;
	const uint32_t *a = rn_group(m, 1, &an), *b = rn_group(m, 2, &bn);
	if (an >= 4 || bn >= 4 || inverted_quantity(m->s, m->start[0]))
		return whole(m, o);
	put(o, a, an);
	putlit(o, U", ");
	put(o, b, bn);
	return 0;
}

static int has_decimal(const rn_str *s)
{
	for (size_t i = 0; i < s->n; i++)
		if (rn_py_isdecimal(s->s[i]))
			return 1;
	return 0;
}

/* R14: "1999г.", "2000гг." → "1999 г.", "2000 гг.", so the year patterns see them */
static int unglue(rn_str *cur)
{
	rn_str o = {0};
	for (size_t i = 0; i < cur->n; i++) {
		uint32_t c = cur->s[i];
		if (i && c == 0x433 && rn_py_isdecimal(cur->s[i - 1])) {
			size_t j = i + 1;
			if (j < cur->n && cur->s[j] == 0x433)
				j++;
			if ((j >= cur->n || !(rn_py_isalnum(cur->s[j]) || cur->s[j] == '_')) &&
			    rn_str_putc(&o, ' ') < 0)
				goto fail;
		}
		if (rn_str_putc(&o, c) < 0)
			goto fail;
	}
	if (rn_str_copy(cur, &o) < 0)
		goto fail;
	rn_str_free(&o);
	return 0;
fail:
	rn_str_free(&o);
	return -1;
}

int rn_run_years(const rn_str *in, rn_str *out)
{
	if (!has_decimal(in))
		return rn_str_put(out, in->s, in->n);
	rn_mx_trim();
	rn_mx_failed = 0;
	static const struct {
		int pat;
		rn_repl_fn fn;
	} SUBS[] = {
		{RN_YEARS_L499, r_split},        {RN_YEARS_L443, r_range_decade},
		{RN_YEARS_L447, r_s_po},         {RN_YEARS_L451, r_range},
		{RN_YEARS_L455, r_multiple},     {RN_YEARS_L463, r_dual},
		{RN_YEARS_L469, r_paren},        {RN_YEARS_L477, r_era_range},
		{RN_YEARS_L473, r_era_year},     {RN_YEARS_L459, r_suffix},
		{RN_YEARS_L490, r_ot_do},        {RN_YEARS_L494, r_prep_implicit},
		{RN_YEARS_L515, r_before_month}, {RN_YEARS_L504, r_leta},
		{RN_YEARS_L509, r_month_year},   {RN_YEARS_L520, r_biblio},
		{RN_YEARS_L481, r_with_word},    {RN_YEARS_L1140, r_orphan},
	};
	rn_str cur = {0};
	if (rn_str_copy(&cur, in) < 0)
		return -1;
	if (!cur.s && rn_str_put(&cur, NULL, 0) < 0)
		return -1;
	int err = 0;
	if (rn_fixes & RN_FIX_YEAR_GLUED && unglue(&cur) < 0)
		err = 1;
	for (size_t i = 0; i < sizeof SUBS / sizeof *SUBS && !err; i++)
		if (rn_resub(rn_pat(SUBS[i].pat), &cur, SUBS[i].fn, NULL) < 0)
			err = 1;
	/* run_years: normalize_numeric_ranges over the result, after the same decimal check */
	if (!err && has_decimal(&cur) &&
	    rn_resub(P(NUMERIC_RANGE_PATTERN), &cur, r_numeric_range, NULL) < 0)
		err = 1;
	if (!err && rn_str_put(out, cur.s, cur.n) < 0)
		err = 1;
	rn_str_free(&cur);
	return err || rn_mx_failed ? -1 : 0;
}
