/* normalize_dates_and_time of dates_time.py: text dates ("5 мая 1945 г."), numeric and ISO
 * dates, day.month with a context word, times ("14:30", "в 9.15 утра").
 *
 * The oracle calls num2words directly here (not render_ordinal), so do we. Errors (out of
 * memory, a regex failure) set rn_mx_failed; rn_run_dates_time reports them at the end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>

#include "rn-mx.h"
#include "rn-num.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-units.h"
#include "rn-util.h"

#define P(x) rn_pat(RN_DATES_TIME_##x)

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

/* the named group of m (matched with pattern p); NULL if unmatched or absent */
static const uint32_t *grp(const rn_match *m, const rn_re *p, const char *name, size_t *n)
{
	return rn_group(m, rn_re_group(p, name), n);
}

/* int(s) of decimal digits (any script); -1 if not a number */
static long ival(const uint32_t *s, size_t n)
{
	char b[32];
	if (!s || rn_int_str(s, n, b, sizeof b) < 0)
		return -1;
	return strtol(b, NULL, 10);
}

/* int(m.group(name)) */
static long gint(const rn_match *m, const rn_re *p, const char *name)
{
	size_t n;
	const uint32_t *s = grp(m, p, name, &n);
	return ival(s, n);
}

/* num2words(v, lang="ru", to=..., case=c, gender=g); 0, -1 where it raises */
static int n2w(rn_str *o, int ordinal, long v, enum n2w_case c, enum n2w_gender g)
{
	char num[24], buf[512];
	snprintf(num, sizeof num, "%ld", v);
	n2w_opts op = N2W_DEFAULTS;
	op.fixes = rn_n2w_fixes;
	op.gcase = c;
	op.gender = g;
	int len = ordinal ? n2w_ru_ordinal(num, op, buf, sizeof buf)
	                  : n2w_ru_cardinal(num, op, buf, sizeof buf);
	if (len < 0 || (size_t)len >= sizeof buf)
		return -1;
	put8(o, buf);
	return 0;
}

/* _day_to_ordinal(day, case) appended; 0, or -1 (None: the day is not 1..31) */
static int day_ordinal(rn_str *o, long day, enum n2w_case c)
{
	if (day < 1 || day > 31)
		return -1;
	if (n2w(o, 1, day, c, N2W_NEUT) < 0 && n2w(o, 1, day, N2W_NOMN, N2W_MASC) < 0)
		return -1;
	return 0;
}

/* s.lower() into t (cleared) */
static void lower(const uint32_t *s, size_t n, rn_str *t)
{
	t->n = 0;
	if (rn_py_lower(s, n, t) < 0)
		fail();
}

/* _month_output(raw) */
static void month_output(rn_str *o, const uint32_t *s, size_t n)
{
	rn_str t = {0};
	lower(s, n, &t);
	if (eq(t.s, t.n, U"маия") || eq(t.s, t.n, U"маія"))
		putlit(o, U"мая");
	else
		put(o, t.s, t.n);
	rn_str_free(&t);
}

/* pattern.search(s[a:b]) */
static int slice_search(const rn_re *re, const uint32_t *s, size_t a, size_t b)
{
	rn_match m;
	int r = rn_search(re, s + a, b - a, 0, &m);
	if (r < 0) {
		fail();
		return 0;
	}
	return r;
}

/* pattern.match(s[a:b]) */
static int slice_match(const rn_re *re, const uint32_t *s, size_t a, size_t b, rn_match *m)
{
	int r = rn_match_at(re, s + a, b - a, 0, m);
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

static size_t right(const rn_match *m, size_t k)
{
	size_t e = m->end[0] + k;
	return e < m->n ? e : m->n;
}

/* _era_tail_dot */
static void era_tail_dot(const rn_match *m, const rn_re *p, rn_str *o)
{
	size_t n;
	const uint32_t *w = grp(m, p, "era_word", &n);
	if (w && n && w[n - 1] == '.' && rn_keep_terminal_abbr_dot(m->s, m->n, m->end[0]))
		putlit(o, U".");
}

/* _date_year_tail */
static void year_tail(const rn_match *m, const rn_re *p, rn_str *o)
{
	size_t yn, en;
	const uint32_t *y = grp(m, p, "year", &yn);
	if (!y || !yn)
		return;
	rn_match d;
	int dual = rn_match_at(P(DUAL_DATING_TAIL_PATTERN), m->s, m->n, m->end[0], &d);
	if (dual < 0)
		fail();
	long year = ival(y, yn);
	putlit(o, U" ");
	if (dual == 1 || (!grp(m, p, "era_word", &en) && !(year >= 1000 && year <= 2100))) {
		put(o, y, yn);
		return;
	}
	/* year_to_ordinal_words(year, "gent"): num2words' genitive ordinal for years < 10000 */
	if (n2w(o, 1, year, N2W_GENT, N2W_MASC) < 0)
		fail();
	putlit(o, U" года");
}

/* the tail every text date ends with */
static void date_tail(const rn_match *m, const rn_re *p, rn_str *o)
{
	year_tail(m, p, o);
	era_tail_dot(m, p, o);
}

/* ---- normalize_text_dates ---- */

static int r_from_to(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(TEXT_DATE_FROM_TO_PATTERN);
	size_t sn, en, d1n, d2n, mn;
	const uint32_t *sp = grp(m, p, "start_prep", &sn), *ep = grp(m, p, "end_prep", &en);
	const uint32_t *d1 = grp(m, p, "day1", &d1n), *d2 = grp(m, p, "day2", &d2n);
	const uint32_t *mo = grp(m, p, "month", &mn);
	rn_str t = {0}, a = {0}, b = {0};
	lower(ep, en, &t);
	enum n2w_case end_case = eq(t.s, t.n, U"по") ? N2W_ACCS : N2W_GENT;
	rn_str_free(&t);
	if (day_ordinal(&a, ival(d1, d1n), N2W_GENT) < 0 ||
	    day_ordinal(&b, ival(d2, d2n), end_case) < 0) {
		rn_str_free(&a);
		rn_str_free(&b);
		return whole(m, o);
	}
	put(o, sp, sn);
	putlit(o, U" ");
	put(o, a.s, a.n);
	putlit(o, U" ");
	put(o, ep, en);
	putlit(o, U" ");
	put(o, b.s, b.n);
	putlit(o, U" ");
	month_output(o, mo, mn);
	date_tail(m, p, o);
	rn_str_free(&a);
	rn_str_free(&b);
	return 0;
}

/* TEXT_DATE_RANGE_CASES.get(prep.lower(), "genitive") */
static enum n2w_case range_case(const uint32_t *prep, size_t n)
{
	if (!prep)
		return N2W_GENT;
	rn_str t = {0};
	lower(prep, n, &t);
	int acc = eq(t.s, t.n, U"за") || eq(t.s, t.n, U"на") || eq(t.s, t.n, U"в") ||
	          eq(t.s, t.n, U"во") || eq(t.s, t.n, U"по");
	rn_str_free(&t);
	return acc ? N2W_ACCS : N2W_GENT;
}

static int r_range(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(TEXT_DATE_RANGE_PATTERN);
	size_t pn, d1n, d2n, mn;
	const uint32_t *pr = grp(m, p, "prep", &pn);
	const uint32_t *d1 = grp(m, p, "day1", &d1n), *d2 = grp(m, p, "day2", &d2n);
	const uint32_t *mo = grp(m, p, "month", &mn);
	enum n2w_case c = range_case(pr, pn);
	rn_str a = {0}, b = {0};
	if (day_ordinal(&a, ival(d1, d1n), c) < 0 || day_ordinal(&b, ival(d2, d2n), c) < 0) {
		rn_str_free(&a);
		rn_str_free(&b);
		return whole(m, o);
	}
	if (pr) {
		put(o, pr, pn);
		putlit(o, U" ");
	}
	put(o, a.s, a.n);
	putlit(o, U" — ");
	put(o, b.s, b.n);
	putlit(o, U" ");
	month_output(o, mo, mn);
	date_tail(m, p, o);
	rn_str_free(&a);
	rn_str_free(&b);
	return 0;
}

/* R10: the case a preposition just before start governs: за/по/на → accusative, к/ко →
 * dative, перед → instrumental; -1 for none of them */
static int left_prep_case(const uint32_t *s, size_t start)
{
	size_t b = start;
	while (b && rn_py_isspace(s[b - 1]))
		b--;
	size_t a = b;
	while (a && rn_py_isalpha(s[a - 1]))
		a--;
	if (a == b || b - a > 5 || (a && (rn_py_isalnum(s[a - 1]) || s[a - 1] == '_')))
		return -1;
	rn_str t = {0};
	lower(s + a, b - a, &t);
	int c = eq(t.s, t.n, U"за") || eq(t.s, t.n, U"по") || eq(t.s, t.n, U"на") ? N2W_ACCS
	        : eq(t.s, t.n, U"к") || eq(t.s, t.n, U"ко")                      ? N2W_DATV
	        : eq(t.s, t.n, U"перед")                                         ? N2W_ABLT
	                                                                         : -1;
	rn_str_free(&t);
	return c;
}

/* R10: a date without its own preposition takes the case of the one before it */
static enum n2w_case outer_case(const rn_match *m, enum n2w_case dflt)
{
	if (!(rn_fixes & RN_FIX_DATE_PREP_CASE))
		return dflt;
	int c = left_prep_case(m->s, m->start[0]);
	return c < 0 ? dflt : (enum n2w_case)c;
}

static int r_list(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(TEXT_DATE_LIST_PATTERN);
	size_t dn, mn;
	const uint32_t *days = grp(m, p, "days", &dn), *mo = grp(m, p, "month", &mn);
	rn_str r = {0};
	/* re.split(r"(\s*(?:,|и)\s*)", days, flags=re.IGNORECASE): numbers between the
	 * separators; the separators stay as they are */
	enum n2w_case c = outer_case(m, N2W_GENT);
	rn_iter it = rn_iter_init(P(L334), days, dn, 0);
	rn_match sm;
	size_t pos = 0;
	int ok = 1, more = 1;
	while (ok && more) {
		int k = rn_iter_next(&it, &sm);
		if (k < 0) {
			fail();
			k = 0;
		}
		size_t a = pos, b = k ? sm.start[0] : dn;
		more = k == 1;
		/* part.strip() is the number, which isdigit() and day_ordinal accept or not */
		size_t sa, sb;
		rn_py_strip(days + a, b - a, &sa, &sb);
		if (sb > sa) {
			if (!rn_py_str_isdigit(days + a + sa, sb - sa) ||
			    day_ordinal(&r, ival(days + a + sa, sb - sa), c) < 0)
				ok = 0;
		}
		if (more) {
			put(&r, days + sm.start[0], sm.end[0] - sm.start[0]);
			pos = sm.end[0];
		}
	}
	if (!ok) {
		rn_str_free(&r);
		return whole(m, o);
	}
	put(o, r.s, r.n);
	putlit(o, U" ");
	month_output(o, mo, mn);
	date_tail(m, p, o);
	rn_str_free(&r);
	return 0;
}

static int r_text_date(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(TEXT_DATE_PATTERN);
	size_t pn, dn, mn;
	const uint32_t *pr = grp(m, p, "prep", &pn), *d = grp(m, p, "day", &dn);
	const uint32_t *mo = grp(m, p, "month", &mn);
	enum n2w_case c = outer_case(m, N2W_GENT);
	if (pr) {
		rn_str t = {0};
		lower(pr, pn, &t);
		c = eq(t.s, t.n, U"на")                            ? N2W_ACCS
		    : eq(t.s, t.n, U"к") || eq(t.s, t.n, U"ко") ? N2W_DATV
		                                                  : N2W_ABLT;
		rn_str_free(&t);
	}
	rn_str w = {0};
	if (day_ordinal(&w, ival(d, dn), c) < 0) {
		rn_str_free(&w);
		return whole(m, o);
	}
	if (pr) {
		put(o, pr, pn);
		putlit(o, U" ");
	}
	put(o, w.s, w.n);
	putlit(o, U" ");
	month_output(o, mo, mn);
	date_tail(m, p, o);
	rn_str_free(&w);
	return 0;
}

/* _counted_noun_follows of years.py: "до мая 1500 солдат" */
/* the word before start (over spaces) has a preposition parse first: "до мая" */
static int prep_before(const uint32_t *s, size_t start)
{
	size_t b = start;
	while (b && rn_py_isspace(s[b - 1]))
		b--;
	size_t a = b;
	while (a && rn_py_isalpha(s[a - 1]))
		a--;
	if (a == b)
		return 0;
	const cmorphy3_form *f = rn_mfirst(s + a, b - a);
	return rn_pos(f->tag) == rn_g.PREP;
}

/* mstart: where the month starts (R11) */
int rn_counted_noun_follows(const uint32_t *s, size_t n, size_t mstart, size_t end,
                                long number)
{
	rn_match w;
	size_t e = end + 24 < n ? end + 24 : n;
	if (!slice_match(rn_pat(RN_YEARS_L244), s, end, e, &w))
		return 0;
	long tail = number % 100, last = number % 10;
	/* R11: after 1, 21 … the noun is nominative singular ("до мая 21 солдат"); the oracle
	 * gives up. Only where a preposition governs the month: "ноября 1 генерал-прокурор
	 * предложил" is a date. */
	int one = last == 1 && tail != 11;
	if (one && (!(rn_fixes & RN_FIX_COUNTED_ONE) || !prep_before(s, mstart)))
		return 0;
	int req = one || (last >= 2 && last <= 4 && !(tail >= 12 && tail <= 14)) ? rn_g.sing : rn_g.plur;
	int want_case = one ? rn_g.nomn : rn_g.gent;
	size_t wn;
	const uint32_t *ws = rn_group(&w, 1, &wn);
	const cmorphy3_form *f;
	int count = rn_mparse(ws, wn, &f);
	/* select_parse(word, pos="NOUN", required_grammemes=(req,), case="gent") */
	for (int i = 0; i < count; i++) {
		if (rn_pos(f[i].tag) != rn_g.NOUN || !cmorphy3_gset_has(rn_canon(rn_gs(f[i].tag)), req) ||
		    rn_canon_case(rn_case(f[i].tag)) != want_case ||
		    (one && (rn_has(f[i].tag, rn_g.Name) || rn_has(f[i].tag, rn_g.Surn) ||
		             rn_has(f[i].tag, rn_g.Patr) || rn_has(f[i].tag, rn_g.Geox))))
			continue;
		const char *nf = f[i].normal_form;
		return strcmp(nf, "год") && strcmp(nf, "день") && strcmp(nf, "число");
	}
	return 0;
}

static int r_month_first(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(MONTH_FIRST_DATE_PATTERN);
	size_t dn, mn;
	const uint32_t *d = grp(m, p, "day", &dn), *mo = grp(m, p, "month", &mn);
	long day = ival(d, dn);
	if (rn_counted_noun_follows(m->s, m->n, m->start[0], m->end[0], day))
		return whole(m, o);
	rn_str w = {0};
	if (day_ordinal(&w, day, N2W_GENT) < 0) {
		rn_str_free(&w);
		return whole(m, o);
	}
	put(o, mo, mn);
	putlit(o, U" ");
	put(o, w.s, w.n);
	rn_str_free(&w);
	return 0;
}

static int r_slash_double(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(SLASH_DOUBLE_DATE_PATTERN);
	size_t d1n, d2n, mn;
	const uint32_t *d1 = grp(m, p, "day1", &d1n), *d2 = grp(m, p, "day2", &d2n);
	const uint32_t *mo = grp(m, p, "month", &mn);
	long a = ival(d1, d1n), b = ival(d2, d2n);
	if (!(b - a >= 10 && b - a <= 13))
		return whole(m, o);
	size_t st = m->start[0];
	enum n2w_case c = slice_search(P(L411), m->s, left(st, 12), st) ? N2W_ACCS : N2W_GENT;
	rn_str x = {0}, y = {0};
	if (day_ordinal(&x, a, c) < 0 || day_ordinal(&y, b, c) < 0) {
		rn_str_free(&x);
		rn_str_free(&y);
		return whole(m, o);
	}
	put(o, x.s, x.n);
	putlit(o, U" / ");
	put(o, y.s, y.n);
	putlit(o, U" ");
	month_output(o, mo, mn);
	rn_str_free(&x);
	rn_str_free(&y);
	return 0;
}

/* ---- the numeric forms ---- */

static const char32_t *const MONTHS[13] = {
	NULL,       U"января", U"февраля", U"марта",    U"апреля",  U"мая",    U"июня",
	U"июля",    U"августа", U"сентября", U"октября", U"ноября", U"декабря",
};

static int r_numeric(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(NUMERIC_DATE_PATTERN);

	long day = gint(m, p, "day");
	long month = gint(m, p, "month");
	long year = gint(m, p, "year");
	if (day < 1 || day > 31 || month < 1 || month > 12)
		return whole(m, o);
	if (year < 100)
		year = year >= 50 ? 1900 + year : 2000 + year;
	size_t st = m->start[0];
	/* date_case: "на|по" before → accusative, else genitive */
	enum n2w_case c = outer_case(
		m, slice_search(P(L411), m->s, left(st, 12), st) ? N2W_ACCS : N2W_GENT);
	if (n2w(o, 1, day, c, N2W_NEUT) < 0 && n2w(o, 1, day, N2W_NOMN, N2W_MASC) < 0)
		fail();
	putlit(o, U" ");
	putlit(o, MONTHS[month]);
	putlit(o, U" ");
	if (n2w(o, 1, year, N2W_GENT, N2W_MASC) < 0 && n2w(o, 1, year, N2W_NOMN, N2W_MASC) < 0)
		fail();
	putlit(o, U" года");
	era_tail_dot(m, p, o);
	return 0;
}

static int r_abbreviated(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	static const struct {
		const char32_t *k;
		int v;
	} ABBR[] = {{U"янв", 1},  {U"фев", 2},  {U"февр", 2}, {U"мар", 3},  {U"апр", 4},
	            {U"май", 5},  {U"июн", 6},  {U"июл", 7},  {U"авг", 8},  {U"сен", 9},
	            {U"сент", 9}, {U"окт", 10}, {U"ноя", 11}, {U"нояб", 11}, {U"дек", 12}};
	const rn_re *p = P(ABBREVIATED_TEXT_DATE_PATTERN);
	size_t dn, mn, yn;
	const uint32_t *d = grp(m, p, "day", &dn), *mo = grp(m, p, "month", &mn);
	const uint32_t *y = grp(m, p, "year", &yn);
	rn_str t = {0};
	lower(mo, mn, &t);
	while (t.n && t.s[t.n - 1] == '.')
		t.n--;
	int month = 0;
	for (size_t i = 0; i < sizeof ABBR / sizeof *ABBR && !month; i++)
		if (eq(t.s, t.n, ABBR[i].k))
			month = ABBR[i].v;
	rn_str_free(&t);
	if (!month)
		return whole(m, o);
	char b[8];
	snprintf(b, sizeof b, ".%02d.", month);
	put(o, d, dn);
	put8(o, b);
	put(o, y, yn);
	return 0;
}

static int r_day_month(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(DAY_MONTH_DATE_PATTERN);
	size_t st = m->start[0];
	rn_match r;
	if (!slice_search(P(DAY_MONTH_DATE_CONTEXT_PATTERN), m->s, left(st, 64), st) &&
	    !slice_match(P(L469), m->s, m->end[0], right(m, 16), &r))
		return whole(m, o);
	rn_str w = {0};
	if (day_ordinal(&w, gint(m, p, "day"), N2W_GENT) < 0) {
		rn_str_free(&w);
		return whole(m, o);
	}
	put(o, w.s, w.n);
	putlit(o, U" ");
	putlit(o, MONTHS[gint(m, p, "month")]);
	rn_str_free(&w);
	return 0;
}

static int r_iso(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(ISO_DATE_PATTERN);
	size_t dn, mn, yn;
	const uint32_t *d = grp(m, p, "day", &dn), *mo = grp(m, p, "month", &mn);
	const uint32_t *y = grp(m, p, "year", &yn);
	put(o, d, dn);
	putlit(o, U".");
	put(o, mo, mn);
	putlit(o, U".");
	put(o, y, yn);
	return 0;
}

/* ---- normalize_time ---- */

static const char32_t *const HOURS[3] = {U"час", U"часа", U"часов"};
static const char32_t *const MINUTES[3] = {U"минута", U"минуты", U"минут"};
static const char32_t *const SECONDS[3] = {U"секунда", U"секунды", U"секунд"};

static const char32_t *counted(long v, const char32_t *const f[3])
{
	long t = v % 100, l = v % 10;
	if (t >= 11 && t <= 14)
		return f[2];
	if (l == 1)
		return f[0];
	if (l >= 2 && l <= 4)
		return f[1];
	return f[2];
}

static const char32_t *gen_counted(long v, const char32_t *const f[3])
{
	return v % 10 == 1 && v % 100 != 11 ? f[1] : f[2];
}

static int governed_range(const rn_match *m)
{
	size_t st = m->start[0];
	return slice_search(P(L503), m->s, left(st, 12), st);
}

/* render_time; p is the pattern m comes from */
static int render_time(const rn_match *m, const rn_re *p, rn_str *o)
{
	size_t mn, sn;
	long hour = gint(m, p, "hour");
	const uint32_t *ms = grp(m, p, "minute", &mn);
	long minute = ival(ms, mn);
	const uint32_t *ss = grp(m, p, "second", &sn);
	int gov = governed_range(m);
	if (ss) {
		long second = ival(ss, sn);
		rn_str t = {0};
		int bad = n2w(&t, 0, hour, N2W_NOMN, N2W_MASC) < 0;
		if (!bad) {
			putlit(&t, U" ");
			putlit(&t, counted(hour, HOURS));
			putlit(&t, U" ");
			bad = n2w(&t, 0, minute, N2W_NOMN, N2W_FEMN) < 0;
		}
		if (!bad) {
			putlit(&t, U" ");
			putlit(&t, counted(minute, MINUTES));
			putlit(&t, U" ");
			/* R7: секунда is feminine */
			bad = n2w(&t, 0, second, N2W_NOMN,
			          rn_fixes & RN_FIX_SECONDS_FEMN ? N2W_FEMN : N2W_MASC) < 0;
		}
		if (!bad) {
			putlit(&t, U" ");
			putlit(&t, counted(second, SECONDS));
			put(o, t.s, t.n);
		}
		rn_str_free(&t);
		return bad ? whole(m, o) : 0;
	}
	if (n2w(o, 0, hour, gov ? N2W_GENT : N2W_NOMN, N2W_MASC) < 0) {
		char b[24];
		snprintf(b, sizeof b, "%ld", hour);
		put8(o, b);
	}
	if (gov) {
		putlit(o, U" ");
		putlit(o, gen_counted(hour, HOURS));
		/* R9: "с девяти часов", not "с девяти часов ноля минут" */
		if (!minute && (rn_fixes & RN_FIX_TIME_GENT_ZERO))
			return 0;
		putlit(o, U" ");
		/* R8: минута is feminine */
		if (n2w(o, 0, minute, N2W_GENT, rn_fixes & RN_FIX_TIME_GENT_FEMN ? N2W_FEMN : N2W_MASC) < 0)
			put(o, ms, mn);
		putlit(o, U" ");
		putlit(o, gen_counted(minute, MINUTES));
		return 0;
	}
	/* R6: "четырнадцать тридцать", "девять ноль-ноль", not "четырнадцать, тридцать" */
	if (rn_fixes & RN_FIX_TIME_NO_COMMA) {
		putlit(o, U" ");
		if (!minute) {
			putlit(o, U"ноль-ноль");
			return 0;
		}
	} else {
		putlit(o, U", ");
	}
	if (ms[0] == '0') {
		putlit(o, U"ноль ");
		if (n2w(o, 0, ival(ms + 1, 1), N2W_NOMN, N2W_MASC) < 0)
			put(o, ms + 1, 1);
	} else if (n2w(o, 0, minute, N2W_NOMN, N2W_MASC) < 0) {
		put(o, ms, mn);
	}
	return 0;
}

static int r_time(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	return render_time(m, P(TIME_PATTERN), o);
}

static int r_time_hour_word(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(TIME_WITH_HOUR_WORD_PATTERN);

	long hour = gint(m, p, "hour");
	long minute = gint(m, p, "minute");
	int gov = governed_range(m);
	enum n2w_case c = gov ? N2W_GENT : N2W_NOMN;
	const char32_t *(*noun)(long, const char32_t *const[3]) = gov ? gen_counted : counted;
	rn_str h = {0}, mi = {0};
	if (n2w(&h, 0, hour, c, N2W_MASC) < 0 || n2w(&mi, 0, minute, c, N2W_FEMN) < 0) {
		rn_str_free(&h);
		rn_str_free(&mi);
		return whole(m, o);
	}
	put(o, h.s, h.n);
	putlit(o, U" ");
	putlit(o, noun(hour, HOURS));
	if (minute) {
		putlit(o, U" ");
		put(o, mi.s, mi.n);
		putlit(o, U" ");
		putlit(o, noun(minute, MINUTES));
	}
	rn_str_free(&h);
	rn_str_free(&mi);
	return 0;
}

/* _currency_word_follows(text[end:end+24]) */
static int currency_follows(const rn_match *m)
{
	rn_match t;
	if (!slice_match(P(DOTTED_TIME_NEXT_TOKEN_PATTERN), m->s, m->end[0], right(m, 24), &t))
		return 0;
	size_t n;
	const uint32_t *w = rn_group(&t, 1, &n);
	const rn_unit *u = rn_unit_resolve_form(w, n);
	return u && !strcmp(u->category, "money");
}

static int r_dotted_time(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t st = m->start[0];
	if (!slice_search(P(DOTTED_TIME_LEFT_CONTEXT_PATTERN), m->s, left(st, 16), st) &&
	    !slice_search(P(DOTTED_TIME_RIGHT_CONTEXT_PATTERN), m->s, m->end[0], right(m, 24)))
		return whole(m, o);
	if (currency_follows(m))
		return whole(m, o);
	return render_time(m, P(DOTTED_TIME_PATTERN), o);
}

int rn_run_dates_time(const rn_str *in, rn_str *out)
{
	static const struct {
		enum rn_pattern pat;
		rn_repl_fn fn;
	} STEPS[] = {
		{RN_DATES_TIME_ABBREVIATED_TEXT_DATE_PATTERN, r_abbreviated},
		{RN_DATES_TIME_DAY_MONTH_DATE_PATTERN, r_day_month},
		{RN_DATES_TIME_SLASH_DOUBLE_DATE_PATTERN, r_slash_double},
		{RN_DATES_TIME_MONTH_FIRST_DATE_PATTERN, r_month_first},
		{RN_DATES_TIME_TEXT_DATE_FROM_TO_PATTERN, r_from_to},
		{RN_DATES_TIME_TEXT_DATE_RANGE_PATTERN, r_range},
		{RN_DATES_TIME_TEXT_DATE_LIST_PATTERN, r_list},
		{RN_DATES_TIME_TEXT_DATE_PATTERN, r_text_date},
		{RN_DATES_TIME_ISO_DATE_PATTERN, r_iso},
		{RN_DATES_TIME_NUMERIC_DATE_PATTERN, r_numeric},
		{RN_DATES_TIME_TIME_WITH_HOUR_WORD_PATTERN, r_time_hour_word},
		{RN_DATES_TIME_TIME_PATTERN, r_time},
		{RN_DATES_TIME_DOTTED_TIME_PATTERN, r_dotted_time},
	};
	size_t i = 0;
	while (i < in->n && !rn_py_isdecimal(in->s[i]))
		i++;
	if (i == in->n)
		return rn_str_put(out, in->s, in->n);
	rn_mx_trim();
	rn_mx_failed = 0;
	rn_str cur = {0};
	int r = -1;
	if (rn_str_copy(&cur, in) < 0)
		goto done;
	for (i = 0; i < sizeof STEPS / sizeof *STEPS; i++)
		if (rn_resub(rn_pat(STEPS[i].pat), &cur, STEPS[i].fn, NULL) < 0 || rn_mx_failed)
			goto done;
	r = rn_str_put(out, cur.s, cur.n);
done:
	rn_str_free(&cur);
	return r;
}
