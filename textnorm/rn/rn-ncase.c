/* get_numeral_case: numerals/_helpers.py (CASE_RULES) and numerals/_rules.py */
#include "rn-ncase.h"

#include <math.h>
#include <uchar.h>
#include <stdlib.h>
#include <string.h>

#include "rn-pat.h"
#include "rn-ph.h"
#include "rn-pystr.h"
#include "rn-tables.h"
#include "rn-units.h"
#include "rn-util.h"

#define NONE (-1)
#define PENDING (-2)

static const char32_t PUNCT[] = U".,:;!\"«»()[]{}";   /* PUNCT_STRIP */
static const char32_t PUNCT2[] = U".,!?;:";

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

static int has_any(const uint32_t *s, size_t n, const char32_t *set)
{
	for (size_t i = 0; i < n; i++)
		if (inset(s[i], set))
			return 1;
	return 0;
}

/* s.strip(set) in place on the slice */
static void strip(const uint32_t **s, size_t *n, const char32_t *set)
{
	while (*n && inset(**s, set)) {
		(*s)++;
		(*n)--;
	}
	while (*n && inset((*s)[*n - 1], set))
		(*n)--;
}

static size_t rstrip_comma(const uint32_t *s, size_t n)
{
	while (n && s[n - 1] == ',')
		n--;
	return n;
}

/* out = s.lower().strip(set) */
static int lower_strip(const uint32_t *s, size_t n, const char32_t *set, rn_str *out)
{
	out->n = 0;
	if (rn_py_lower(s, n, out) < 0) {
		rn_mx_failed = 1;
		out->n = 0;
		return -1;
	}
	const uint32_t *p = out->s;
	size_t k = out->n;
	strip(&p, &k, set);
	if (k && p != out->s)
		memmove(out->s, p, k * sizeof *p);
	out->n = k;
	return 0;
}

#define IN(T, s, n) (rn_table_get32(RN_T_##T, s, n) != NULL)

static int is_unit(const uint32_t *s, size_t n)
{
	return rn_unit_resolve(s, n) != NULL;
}

/* ---- integers ---- */

int rn_parse_int_token(const uint32_t *s, size_t n, rn_int *o)
{
	strip(&s, &n, PUNCT);
	memset(o, 0, sizeof *o);
	if (n && s[0] == RN_NEGATIVE_NUMBER) {
		o->neg = 1;
		s++;
		n--;
	}
	if (!n)
		return 0;
	size_t i = 0;
	for (; i < n; i++)
		if (rn_py_digit(s[i]) < 0)
			break;
	if (i == n) {
		for (i = 0; i < n; i++) {
			unsigned d = (unsigned)rn_py_digit(s[i]);
			if (o->v != UINT64_MAX)
				o->v = o->v > (UINT64_MAX - d) / 10 ? UINT64_MAX : o->v * 10 + d;
			o->m1000 = (o->m1000 * 10 + d) % 1000;
			if (n < sizeof o->digits)
				o->digits[i] = (char)('0' + d);
		}
		return 1;
	}
	uint64_t v;
	if (n == 1 && rn_py_intnumeric(s[0], &v)) {
		o->v = v;
		o->m1000 = (unsigned)(v % 1000);
		/* str(int(value)) */
		char b[24];
		int k = 0;
		do
			b[k++] = (char)('0' + v % 10);
		while (v /= 10);
		for (int j = 0; j < k; j++)
			o->digits[j] = b[k - 1 - j];
		return 1;
	}
	return 0;
}

int rn_is_int_token(const uint32_t *s, size_t n)
{
	rn_int x;
	return rn_parse_int_token(s, n, &x);
}

/* noun_number_form: 1 one, 2 few, 3 many */
static int number_form(const rn_int *x)
{
	if (x->v == 0)
		return 3;
	unsigned l2 = x->m1000 % 100, l1 = x->m1000 % 10;
	if (l2 >= 11 && l2 <= 14)
		return 3;
	return l1 == 1 ? 1 : (l1 >= 2 && l1 <= 4) ? 2 : 3;
}

/* get_target_tags_for_number(n, nomn) */
static cmorphy3_gset target_nomn(const rn_int *x)
{
	int f = number_form(x);
	return f == 1   ? rn_gset_or(rn_gset1(rn_g.nomn), rn_gset1(rn_g.sing))
	       : f == 2 ? rn_gset_or(rn_gset1(rn_g.gent), rn_gset1(rn_g.sing))
	                : rn_gset_or(rn_gset1(rn_g.gent), rn_gset1(rn_g.plur));
}

static int decimal_token(const uint32_t *s, size_t n)
{
	rn_match m;
	int r = rn_search(rn_pat(RN_NUMERALS_HELPERS_DECIMAL_TOKEN_PATTERN), s ? s : RN_U(""), n, 0,
	                  &m);
	if (r < 0)
		rn_mx_failed = 1;
	return r == 1;
}

static double dmax(double a, double b)
{
	return a > b ? a : b;
}

/* top_scoring_verb_parses: indices into f (math.isclose, rel 1e-9, abs 1e-12) */
static int top_verbs(const cmorphy3_form *f, int n, int *out)
{
	double top = f[0].score;
	for (int i = 1; i < n; i++)
		if (f[i].score > top)
			top = f[i].score;
	int k = 0;
	for (int i = 0; i < n; i++) {
		int p = rn_pos(f[i].tag);
		if (p != rn_g.VERB && p != rn_g.INFN)
			continue;
		double a = f[i].score, d = fabs(a - top), m = dmax(fabs(a), fabs(top));
		if (d <= dmax(1e-9 * m, 1e-12))
			out[k++] = i;
	}
	return k;
}

static int nf_in(const cmorphy3_form *f, enum rn_table t)
{
	return f && rn_table_get(t, f->normal_form) != NULL;
}

/* ---- context ---- */

typedef struct {
	size_t pos, vlen;
	int val;   /* NONE or a case */
	int used;
} memo;

struct rn_ncase {
	const rn_tok *t;
	size_t n;
	size_t *lss;
	rn_str *words;
	unsigned char *wdone;
	memo *memo;
	size_t mcap, mn;
	long pending;
	rn_str tmp, tmp2;
};

static const rn_str *word_at(rn_ncase *c, size_t pos)
{
	if (!c->wdone[pos]) {
		lower_strip(c->t[pos].s, c->t[pos].n, PUNCT, &c->words[pos]);
		c->wdone[pos] = 1;
	}
	return &c->words[pos];
}

static int barrier(const rn_str *w)
{
	return IN(NUMERAL_CONTEXT_BARRIERS, w->s, w->n);
}

void rn_ncase_free(rn_ncase *c)
{
	if (!c)
		return;
	for (size_t i = 0; i < c->n; i++)
		rn_str_free(&c->words[i]);
	free(c->words);
	free(c->wdone);
	free(c->lss);
	free(c->memo);
	rn_str_free(&c->tmp);
	rn_str_free(&c->tmp2);
	free(c);
}

rn_ncase *rn_ncase_new(const rn_tok *toks, size_t n)
{
	rn_ncase *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	c->t = toks;
	c->n = n;
	c->lss = calloc(n + 1, sizeof *c->lss);
	c->words = calloc(n + 1, sizeof *c->words);
	c->wdone = calloc(n + 1, 1);
	if (!c->lss || !c->words || !c->wdone) {
		rn_ncase_free(c);
		return NULL;
	}
	size_t cur = 0;
	for (size_t i = 0; i < n; i++) {
		c->lss[i] = cur;
		if (barrier(word_at(c, i)) || IN(SENTENCE_BOUNDARY_TOKENS, toks[i].s, toks[i].n))
			cur = i + 1;
	}
	return c;
}

static memo *memo_find(rn_ncase *c, size_t pos, size_t vlen, int add)
{
	if (add && (c->mn + 1) * 2 > c->mcap) {
		size_t cap = c->mcap ? c->mcap * 2 : 64;
		memo *m = calloc(cap, sizeof *m);
		if (!m) {
			rn_mx_failed = 1;
			return NULL;
		}
		for (size_t i = 0; i < c->mcap; i++)
			if (c->memo[i].used) {
				size_t j = (c->memo[i].pos * 31 + c->memo[i].vlen) & (cap - 1);
				while (m[j].used)
					j = (j + 1) & (cap - 1);
				m[j] = c->memo[i];
			}
		free(c->memo);
		c->memo = m;
		c->mcap = cap;
	}
	if (!c->mcap)
		return NULL;
	size_t j = (pos * 31 + vlen) & (c->mcap - 1);
	for (; c->memo[j].used; j = (j + 1) & (c->mcap - 1))
		if (c->memo[j].pos == pos && c->memo[j].vlen == vlen)
			return &c->memo[j];
	if (!add)
		return NULL;
	c->memo[j] = (memo){pos, vlen, NONE, 1};
	c->mn++;
	return &c->memo[j];
}

typedef struct {
	rn_ncase *c;
	long idx, vlen;
} X;

static const uint32_t *raw(const X *x, long off, size_t *n)
{
	long p = x->idx + off;
	if (p < 0 || p >= x->vlen) {
		*n = 0;
		return RN_U("");
	}
	*n = x->c->t[p].n;
	return x->c->t[p].s;
}

static const rn_str EMPTY;

static const rn_str *word(const X *x, long off)
{
	long p = x->idx + off;
	if (p < 0 || p >= x->vlen)
		return &EMPTY;
	return word_at(x->c, (size_t)p);
}

static int weq(const X *x, long off, const char32_t *lit)
{
	const rn_str *w = word(x, off);
	return eq(w->s, w->n, lit);
}

#define WIN(x, off, T) (word_in(x, off, RN_T_##T))
static int word_in(const X *x, long off, enum rn_table t)
{
	const rn_str *w = word(x, off);
	return rn_table_get32(t, w->s, w->n) != NULL;
}

static const cmorphy3_form *parse(const X *x, long off)
{
	const rn_str *w = word(x, off);
	if (!w->n)
		return NULL;
	return rn_mfirst(w->s, w->n);
}

static int raw_eq(const X *x, long off, const char32_t *lit)
{
	size_t n;
	const uint32_t *s = raw(x, off, &n);
	return eq(s, n, lit);
}

static int raw_isalpha(const X *x, long off)
{
	size_t n;
	const uint32_t *s = raw(x, off, &n);
	return rn_py_str_isalpha(s, n);
}

static int raw_is_int(const X *x, long off)
{
	size_t n;
	const uint32_t *s = raw(x, off, &n);
	return rn_is_int_token(s, n);
}

static int raw_dash(const X *x, long off)
{
	size_t n;
	const uint32_t *s = raw(x, off, &n);
	return IN(DASH_TOKENS, s, n);
}

static int raw_has(const X *x, long off, const char32_t *set)
{
	size_t n;
	const uint32_t *s = raw(x, off, &n);
	return has_any(s, n, set);
}

static int raw_first_upper(const X *x, long off)
{
	size_t n;
	const uint32_t *s = raw(x, off, &n);
	return n && rn_py_isupper(s[0]);
}

static int dependency_case(X *x, long target)
{
	memo *m = memo_find(x->c, (size_t)target, (size_t)x->vlen, 0);
	if (m)
		return m->val >= 0 ? m->val : rn_g.nomn;
	x->c->pending = target;
	return PENDING;
}

static int is_counting_group(rn_ncase *c, long idx, long vlen)
{
	const rn_tok *t = c->t;
	if (idx + 1 >= vlen)
		return 0;
	rn_int pi;
	cmorphy3_gset req;
	if (rn_parse_int_token(t[idx].s, t[idx].n, &pi))
		req = rn_canon(target_nomn(&pi));
	else if (decimal_token(t[idx].s, t[idx].n))
		req = rn_canon(rn_gset_or(rn_gset1(rn_g.gent), rn_gset1(rn_g.sing)));
	else
		return 0;
	long ci = idx + 1;
	if ((eq(t[ci].s, t[ci].n, U",") || eq(t[ci].s, t[ci].n, U".")) && ci + 2 < vlen &&
	    rn_py_str_isdigit(t[ci + 1].s, t[ci + 1].n))
		ci += 2;
	const uint32_t *cr = t[ci].s;
	size_t crn = t[ci].n;
	strip(&cr, &crn, PUNCT);
	rn_str low = {0};
	if (rn_py_lower(cr, crn, &low) < 0) {
		rn_mx_failed = 1;
		return 0;
	}
	int r = 0;
	if (!low.n)
		r = 0;
	else if (eq(low.s, low.n, U"с") || eq(low.s, low.n, U"с."))
		r = 1;
	else if (is_unit(cr, crn) || is_unit(low.s, low.n))
		r = 1;
	else {
		const cmorphy3_form *f;
		int k = rn_mparse(low.s, low.n, &f);
		for (int i = 0; i < k && !r; i++)
			r = rn_reliable(&f[i]) && cmorphy3_gset_subset(req, rn_canon(rn_gs(f[i].tag)));
	}
	rn_str_free(&low);
	return r;
}

/* _rendered_numeral_case */
static int rendered_case(rn_ncase *c, const uint32_t *s, size_t n)
{
	rn_str w = {0};
	int r = NONE;
	lower_strip(s, n, PUNCT, &w);
	if (w.n && !rn_is_int_token(w.s, w.n)) {
		const cmorphy3_form *p = rn_mfirst(w.s, w.n);
		if (rn_pos(p->tag) == rn_g.NUMR && rn_case(p->tag) >= 0)
			r = rn_case(p->tag);
	}
	(void)c;
	rn_str_free(&w);
	return r;
}

static int loctish(const cmorphy3_form *p)
{
	return rn_case(p->tag) == rn_g.loct || rn_has(p->tag, rn_g.loc2);
}

/* ---- the rules ---- */

static int r_operand_after_arithmetic_verb(X *x)
{
	return x->idx > 1 && weq(x, -1, U"на") && WIN(x, -2, ARITHMETIC_OPERATOR_VERBS) ? rn_g.nomn
	                                                                                : NONE;
}

static int r_operand_before_multiplication(X *x)
{
	return x->idx + 2 < x->vlen && WIN(x, 1, MULTIPLICATION_MARKERS) && weq(x, 2, U"на")
	           ? rn_g.nomn
	           : NONE;
}

static int r_ratio_left_operand(X *x)
{
	return x->idx + 2 < x->vlen && weq(x, 1, U"к") && raw_is_int(x, 2) ? rn_g.nomn : NONE;
}

static int r_dimension_first_coordinate(X *x)
{
	if (!(x->idx + 2 < x->vlen && weq(x, 1, U"на") && raw_is_int(x, 2)))
		return NONE;
	long stop = -5 > -x->idx - 1 ? -5 : -x->idx - 1;
	for (long b = -1; b > stop; b--) {
		if (raw_has(x, b, U".!?;:"))
			break;
		if (WIN(x, b, DIMENSION_HEADS))
			return rn_g.nomn;
	}
	return NONE;
}

static int r_temperature_equality(X *x)
{
	return x->idx > 0 && weq(x, -1, U"равно") && x->idx + 2 < x->vlen && weq(x, 1, U"°") &&
	               WIN(x, 2, TEMPERATURE_SCALE_SYMBOLS)
	           ? rn_g.datv
	           : NONE;
}

static int r_comparison_or_equal(X *x)
{
	if (x->idx < 3)
		return NONE;
	rn_str *j = &x->c->tmp;
	j->n = 0;
	for (long o = -3; o <= -1; o++) {
		const rn_str *w = word(x, o);
		if ((o > -3 && rn_str_putc(j, ' ') < 0) || rn_str_put(j, w->s, w->n) < 0) {
			rn_mx_failed = 1;
			return NONE;
		}
	}
	return IN(COMPARISON_PHRASES, j->s, j->n) ? rn_g.gent : NONE;
}

static int r_equality_before_degree_unit(X *x)
{
	if (x->idx >= x->vlen - 1 || !WIN(x, 1, ANGLE_AND_DEGREE_UNITS))
		return NONE;
	long stop = -7 > -x->idx - 1 ? -7 : -x->idx - 1;
	for (long b = -1; b > stop; b--) {
		if (WIN(x, b, EQUALITY_PREDICATES))
			return rn_g.datv;
		if (raw_has(x, b, U".!?;:"))
			break;
	}
	return NONE;
}

static int r_power_exponent(X *x)
{
	return x->idx > 1 && weq(x, -1, U"степени") && weq(x, -2, U"в") ? rn_g.nomn : NONE;
}

static int r_distance_from_landmark(X *x)
{
	long left = 0, right = 0;
	if (x->idx + 2 < x->vlen && raw_dash(x, 1) && raw_is_int(x, 2))
		right = 2;
	else if (x->idx >= 2 && raw_dash(x, -1) && raw_is_int(x, -2))
		left = -2;
	if (x->idx + left <= 0 || !weq(x, left - 1, U"в"))
		return NONE;
	long uo = right + 1;
	if (x->idx + uo >= x->vlen)
		return NONE;
	size_t n;
	const uint32_t *s = raw(x, uo, &n);
	rn_str *u = &x->c->tmp;
	lower_strip(s, n, PUNCT, u);
	long lo = uo + 1;
	int isu = is_unit(u->s, u->n);
	if (!isu && raw_eq(x, lo, U".")) {
		if (rn_str_putc(u, '.') < 0)
			rn_mx_failed = 1;
		isu = is_unit(u->s, u->n);
		lo++;
	}
	if (!(isu && x->idx + lo < x->vlen && WIN(x, lo, LANDMARK_MARKERS)))
		return NONE;
	int era = x->idx + lo + 1 < x->vlen && WIN(x, lo + 1, ERA_MARKER_WORDS);
	return era ? NONE : rn_g.loct;
}

static int r_measurement_continuation(X *x)
{
	size_t n;
	const uint32_t *s;
	if (x->idx < 2)
		return NONE;
	s = raw(x, 1, &n);
	if (!is_unit(s, n))
		return NONE;
	s = raw(x, -1, &n);
	if (!n || !(rn_py_isalpha(s[n - 1]) || s[n - 1] == 0xB0))
		return NONE;
	if (!is_unit(s, n)) {
		const cmorphy3_form *p = parse(x, -1);
		if (!p || !nf_in(p, RN_T_ALL_UNITS))
			return NONE;
	}
	s = raw(x, -2, &n);
	int c = rendered_case(x->c, s, n);
	if (c == NONE && rn_is_int_token(s, n))
		c = dependency_case(x, x->idx - 2);
	if (c == PENDING)
		return PENDING;
	if (c == NONE || c == rn_g.nomn)
		return NONE;
	return c;
}

static int r_coordinated_first_number(X *x)
{
	if (x->idx < 2)
		return NONE;
	size_t pn, n;
	const uint32_t *prev = raw(x, -1, &pn), *s;
	if (eq(prev, pn, U"и")) {
		long stop = -6 > -x->idx - 1 ? -6 : -x->idx - 1;
		for (long b = -2; b > stop; b--) {
			s = raw(x, b, &n);
			if (has_any(s, n, U".!?;:"))
				break;
			if (rn_is_int_token(s, rstrip_comma(s, n)))
				return dependency_case(x, x->idx + b);
			int r = rendered_case(x->c, s, n);
			if (r != NONE)
				return r;
		}
		return NONE;
	}
	if (eq(prev, pn, U",") && raw_is_int(x, -2))
		return dependency_case(x, x->idx - 2);
	int endc = pn && prev[pn - 1] == ',';
	if (endc && rn_is_int_token(prev, rstrip_comma(prev, pn)))
		return dependency_case(x, x->idx - 1);
	if (endc) {
		long stop = -4 > -x->idx - 1 ? -4 : -x->idx - 1;
		for (long b = -1; b > stop; b--) {
			s = raw(x, b, &n);
			n = rstrip_comma(s, n);
			if (rn_is_int_token(s, n))
				return dependency_case(x, x->idx + b);
			int r = rendered_case(x->c, s, n);
			if (r != NONE)
				return r;
			rn_str *w = &x->c->tmp;
			lower_strip(s, n, PUNCT, w);
			if (w->n && rn_reliable(rn_mfirst(w->s, w->n)))
				break;
		}
	}
	return NONE;
}

static int r_range_right_boundary(X *x)
{
	if (x->idx > 1 && raw_dash(x, -1) && raw_is_int(x, -2))
		return dependency_case(x, x->idx - 2);
	return NONE;
}

static int r_fresh_quantity_after_dash(X *x)
{
	return x->idx > 0 && raw_dash(x, -1) ? rn_g.nomn : NONE;
}

static int prep_case_of(const rn_str *w)
{
	const rn_kv *kv = rn_table_get32(RN_T_PREP_CASE, w->s, w->n);
	return kv ? cmorphy3_grammeme(rn_morph, kv->value) : NONE;
}

static int r_range_start_governed_by_left(X *x)
{
	if (!(x->idx < x->vlen - 1 && IN(DASH_TOKENS, x->c->t[x->idx + 1].s, x->c->t[x->idx + 1].n) &&
	      x->idx > 0))
		return NONE;
	size_t n;
	const uint32_t *s = raw(x, -1, &n);
	strip(&s, &n, PUNCT2);
	if (!n)
		return NONE;
	const cmorphy3_form *p = rn_mfirst(s, n);
	int pos = rn_pos(p->tag);
	if (pos == rn_g.VERB && nf_in(p, RN_T_SCORE_VERBS))
		return rn_g.nomn;
	if (pos == rn_g.NOUN && nf_in(p, RN_T_SCORE_NOUNS))
		return rn_g.nomn;
	int pc = rn_has(p->tag, rn_g.loc2) ? rn_g.loct : rn_case(p->tag);
	if (pc < 0)
		return NONE;
	if (rn_reliable(p)) {
		if (nf_in(p, RN_T_LABEL_NOUN_LEMMAS) &&
		    !(x->idx + 2 < x->vlen && is_counting_group(x->c, x->idx + 2, x->vlen)))
			return rn_g.nomn;
		if (nf_in(p, RN_T_GENITIVE_QUANTITY_GOVERNORS))
			return rn_g.gent;
		if (rn_case(p->tag) == rn_g.gent && rn_number(p->tag) == rn_g.plur &&
		    !WIN(x, -2, PREP_CASE))
			return rn_g.nomn;
		if (x->idx > 1) {
			if (WIN(x, -2, LOCATIVE_PREPOSITIONS))
				return rn_g.loct;
			int c = prep_case_of(word(x, -2));
			if (c != NONE) {
				if (x->idx + 2 < x->vlen && is_counting_group(x->c, x->idx + 2, x->vlen))
					return NONE;
				return c;
			}
		}
		return pc;
	}
	if (pos == rn_g.ADJF || pos == rn_g.PRTF)
		return pc;
	return NONE;
}

static int ablt_plur_only(const rn_str *w, int pa, int pb)
{
	const cmorphy3_form *f;
	int k = rn_mparse(w->s, w->n, &f), any = 0;
	for (int i = 0; i < k; i++) {
		int pos = rn_pos(f[i].tag);
		if (pos != pa && pos != pb)
			continue;
		any = 1;
		if (rn_case(f[i].tag) != rn_g.ablt || rn_number(f[i].tag) != rn_g.plur)
			return 0;
	}
	return any;
}

static int r_instrumental_group(X *x)
{
	for (long off = 1; off <= 3 && raw_isalpha(x, off); off++) {
		const rn_str *w = word(x, off);
		if (ablt_plur_only(w, rn_g.NOUN, -100)) {
			if (raw_first_upper(x, off) && off == 1)
				return NONE;
			return rn_g.ablt;
		}
		if (!ablt_plur_only(w, rn_g.ADJF, rn_g.PRTF))
			break;
	}
	if (!raw_isalpha(x, -1) || !(weq(x, -1, U"этими") || weq(x, -1, U"теми") ||
	                             weq(x, -1, U"всеми") || weq(x, -1, U"обоими")))
		return NONE;
	const cmorphy3_form *r = raw_isalpha(x, 1) ? parse(x, 1) : NULL;
	if (r) {
		int pos = rn_pos(r->tag);
		if (pos == rn_g.NOUN || pos == rn_g.ADJF || pos == rn_g.PRTF)
			return NONE;
	}
	return rn_g.ablt;
}

static int r_label_prefix(X *x)
{
	return x->idx > 0 && WIN(x, -1, LABEL_PREFIX_TOKENS) ? rn_g.nomn : NONE;
}

static int r_numbering_head(X *x)
{
	return x->idx > 0 && WIN(x, -1, NUMBERING_HEAD_TOKENS) ? rn_g.nomn : NONE;
}

static int r_label_noun_head(X *x)
{
	const cmorphy3_form *p = parse(x, -1);
	return p && rn_reliable(p) && nf_in(p, RN_T_LABEL_NOUN_LEMMAS) ? rn_g.nomn : NONE;
}

static int r_genitive_quantity_governor(X *x)
{
	const cmorphy3_form *p = parse(x, -1);
	if (!p)
		return NONE;
	int c = rn_case(p->tag);
	return rn_reliable(p) && nf_in(p, RN_T_GENITIVE_QUANTITY_GOVERNORS) &&
	               (c == rn_g.nomn || c == rn_g.gent)
	           ? rn_g.gent
	           : NONE;
}

static int r_negated_existence(X *x)
{
	const rn_str *left = word(x, -1);
	if (!left->n || !raw_isalpha(x, -1))
		return NONE;
	if (!IN(NEGATED_GENITIVE_PREDICATIVES, left->s, left->n)) {
		if (!weq(x, -2, U"не") || !raw_isalpha(x, -2))
			return NONE;
		const cmorphy3_form *f;
		int k = rn_mparse(left->s, left->n, &f), *tv = malloc((size_t)k * sizeof *tv), ok = 0;
		if (!tv) {
			rn_mx_failed = 1;
			return NONE;
		}
		int nt = top_verbs(f, k, tv);
		for (int i = 0; i < nt && !ok; i++)
			ok = nf_in(&f[tv[i]], RN_T_NEGATED_GENITIVE_VERB_LEMMAS);
		free(tv);
		if (!ok)
			return NONE;
	}
	const rn_str *right = word(x, 1);
	if (!right->n)
		return NONE;
	const cmorphy3_form *f;
	int k = rn_mparse(right->s, right->n, &f);
	for (int i = 0; i < k; i++) {
		int pos = rn_pos(f[i].tag);
		if ((pos == rn_g.NOUN || pos == rn_g.ADJF || pos == rn_g.PRTF) &&
		    rn_case(f[i].tag) == rn_g.gent)
			return rn_g.gent;
	}
	return NONE;
}

static int has_name(uint16_t tag)
{
	return rn_has(tag, rn_g.Name) || rn_has(tag, rn_g.Surn) || rn_has(tag, rn_g.Patr);
}

static int person_head(const cmorphy3_form *p)
{
	if (!p || rn_pos(p->tag) != rn_g.NOUN || rn_case(p->tag) != rn_g.nomn)
		return 0;
	return rn_has(p->tag, rn_g.anim) && !has_name(p->tag);
}

static int r_age_attribute(X *x)
{
	if (weq(x, -1, U"лет")) {
		if (!raw_eq(x, -1, U"лет"))
			return NONE;
		long ho = raw_eq(x, -2, U",") ? -3 : -2;
		if (person_head(parse(x, ho)))
			return rn_g.gent;
		if (weq(x, 1, U"на") && weq(x, 2, U"вид"))
			return rn_g.gent;
		return NONE;
	}
	if (!raw_eq(x, 1, U"лет") || !raw_isalpha(x, -1))
		return NONE;
	if (!person_head(parse(x, -1)))
		return NONE;
	const cmorphy3_form *a = parse(x, 2);
	if (a) {
		int pos = rn_pos(a->tag);
		if (pos == rn_g.VERB || pos == rn_g.INFN || pos == rn_g.GRND || weq(x, 2, U"назад") ||
		    weq(x, 2, U"спустя") || weq(x, 2, U"не") || weq(x, 2, U"уже") ||
		    weq(x, 2, U"ещё") || weq(x, 2, U"подряд"))
			return NONE;
	}
	return rn_g.gent;
}

static int r_noun_head_genitive(X *x)
{
	rn_int pi;
	const rn_tok *tk = &x->c->t[x->idx];
	if (!rn_parse_int_token(tk->s, tk->n, &pi) || number_form(&pi) == 1)
		return NONE;
	long ho = -1;
	if (!raw_isalpha(x, -1))
		return NONE;
	const cmorphy3_form *l = parse(x, -1);
	if (l && (rn_pos(l->tag) == rn_g.ADJF || rn_pos(l->tag) == rn_g.PRTF)) {
		if (rn_case(l->tag) != rn_g.gent || rn_number(l->tag) != rn_g.plur)
			return NONE;
		ho = -2;
	}
	if (!raw_isalpha(x, ho))
		return NONE;
	const cmorphy3_form *h = parse(x, ho);
	if (!h || rn_pos(h->tag) != rn_g.NOUN || rn_case(h->tag) != rn_g.nomn)
		return NONE;
	if (!rn_reliable(h) || rn_has(h->tag, rn_g.anim))
		return NONE;
	if (has_name(h->tag) || rn_has(h->tag, rn_g.Geox) || rn_has(h->tag, rn_g.Orgn) ||
	    rn_has(h->tag, rn_g.Abbr))
		return NONE;
	if (nf_in(h, RN_T_VALUE_HEAD_LEMMAS) || nf_in(h, RN_T_MEASURE_PARAMETER_HEADS) ||
	    nf_in(h, RN_T_LABEL_NOUN_LEMMAS) || nf_in(h, RN_T_TIME_CONTEXT_LEMMAS) ||
	    nf_in(h, RN_T_TIME_COUNTED_LEMMAS))
		return NONE;
	const cmorphy3_form *b = parse(x, ho - 1);
	if (b) {
		int pos = rn_pos(b->tag);
		if (pos == rn_g.PREP || pos == rn_g.VERB || pos == rn_g.INFN)
			return NONE;
	}
	const rn_str *right = word(x, 1);
	if (!right->n || !raw_isalpha(x, 1) || !rn_word_is_known(right->s, right->n))
		return NONE;
	if (raw_first_upper(x, 1))
		return NONE;
	size_t n;
	const uint32_t *s = raw(x, 1, &n);
	if (is_unit(s, n) && !WIN(x, 1, TIME_WORDS))
		return NONE;
	const cmorphy3_form *top = parse(x, 1);
	if (!top || !strcmp(top->normal_form, "раз") ||
	    (rn_pos(top->tag) != rn_g.NOUN && rn_pos(top->tag) != rn_g.ADJF) ||
	    rn_case(top->tag) != rn_g.gent || rn_number(top->tag) != rn_g.plur)
		return NONE;
	const cmorphy3_form *a = parse(x, 2);
	if (WIN(x, 2, GROUP_END_MARKERS))
		return NONE;
	if (nf_in(top, RN_T_TIME_COUNTED_LEMMAS)) {
		if (ho == -1 &&
		    (!a || rn_pos(a->tag) != rn_g.NOUN || rn_case(a->tag) != rn_g.gent))
			return NONE;
	} else if (a && (rn_pos(a->tag) == rn_g.INFN ||
	                 (rn_pos(a->tag) == rn_g.VERB && rn_has(a->tag, rn_g.tran)))) {
		return NONE;
	}
	return rn_g.gent;
}

static int r_locative_quantity_head(X *x)
{
	if (x->idx <= 1)
		return NONE;
	const cmorphy3_form *p = parse(x, -1);
	if (!p)
		return NONE;
	return rn_reliable(p) && nf_in(p, RN_T_LOCATIVE_GENITIVE_QUANTITY_HEADS) && loctish(p) &&
	               (weq(x, -2, U"в") || weq(x, -2, U"во"))
	           ? rn_g.gent
	           : NONE;
}

static int r_measure_parameter_head(X *x)
{
	const cmorphy3_form *p = parse(x, -1);
	if (!p)
		return NONE;
	return rn_reliable(p) && nf_in(p, RN_T_MEASURE_PARAMETER_HEADS) &&
	               is_counting_group(x->c, x->idx, x->vlen)
	           ? rn_g.nomn
	           : NONE;
}

static int r_oblique_noun_before_bare_number(X *x)
{
	if (x->idx <= 1)
		return NONE;
	const rn_tok *tk = &x->c->t[x->idx];
	if (!decimal_token(tk->s, tk->n)) {
		const rn_str *nw = word(x, 1);
		if (nw->n) {
			int pos = rn_pos(rn_mfirst(nw->s, nw->n)->tag);
			if (pos == rn_g.NOUN || pos == rn_g.ADJF || pos == rn_g.PRTF)
				return NONE;
		}
	}
	const cmorphy3_form *l = parse(x, -1);
	if (!l)
		return NONE;
	if (!rn_reliable(l) || is_counting_group(x->c, x->idx, x->vlen))
		return NONE;
	const rn_str *lw = word(x, -1);
	const cmorphy3_form *f;
	int k = rn_mparse(lw->s, lw->n, &f);
	for (int i = 0; i < k; i++)
		if (rn_has(f[i].tag, rn_g.ADJF))
			return NONE;
	int c = rn_case(l->tag);
	if (!(c == rn_g.gent || c == rn_g.datv || c == rn_g.ablt || c == rn_g.loct ||
	      rn_has(l->tag, rn_g.loc2)))
		return NONE;
	if (!(x->idx + 1 < x->vlen && word(x, 1)->n))
		return NONE;
	if (!(weq(x, -2, U"в") || weq(x, -2, U"во") || weq(x, -2, U"на") || weq(x, -2, U"о") ||
	      weq(x, -2, U"об") || weq(x, -2, U"при")))
		return NONE;
	return rn_has(l->tag, rn_g.loc2) ? rn_g.loct : c;
}

static int r_coordinated_series_head(X *x)
{
	size_t n;
	const uint32_t *s;
	/* _is_series_continuation */
	if (x->idx >= 2) {
		size_t pn;
		const uint32_t *prev = raw(x, -1, &pn);
		if (pn && prev[pn - 1] == ',' && rn_is_int_token(prev, rstrip_comma(prev, pn)))
			return NONE;
		if (eq(prev, pn, U",") || WIN(x, -1, SERIES_COORDINATORS)) {
			s = raw(x, -2, &n);
			n = rstrip_comma(s, n);
			if (rn_is_int_token(s, n) || rendered_case(x->c, s, n) != NONE)
				return NONE;
		}
	}
	/* _series_head_offset */
	long off = 0, members = 1;
	while (members < 512) {
		s = raw(x, off, &n);
		if (!n)
			return NONE;
		long next;
		if (s[n - 1] == ',')
			next = off + 1;
		else if (WIN(x, off + 1, SERIES_COORDINATORS) || raw_eq(x, off + 1, U","))
			next = off + 2;
		else
			break;
		s = raw(x, next, &n);
		if (!rn_is_int_token(s, rstrip_comma(s, n)))
			break;
		off = next;
		members++;
	}
	if (members < 2)
		return NONE;
	long head = off + 1;
	long limit = x->vlen - x->idx < head + 6 ? x->vlen - x->idx : head + 6;
	for (long o = head; o < limit; o++) {
		s = raw(x, o, &n);
		if (has_any(s, n, U".!?;:)]}»"))
			break;
		strip(&s, &n, PUNCT);
		if (!n)
			continue;
		const cmorphy3_form *best = rn_mfirst(s, n);
		int pos = rn_pos(best->tag);
		if ((pos == rn_g.VERB || pos == rn_g.INFN || pos == rn_g.PREP || pos == rn_g.CONJ) &&
		    !weq(x, o, U"и"))
			break;
		if (rn_py_isupper(s[0]))
			break;
		if (pos != rn_g.NOUN)
			continue;
		/* select_parse(parse_word(token), pos="NOUN"): the first NOUN parse */
		const cmorphy3_form *f, *pn = NULL;
		int k = rn_mparse(s, n, &f);
		for (int i = 0; i < k && !pn; i++)
			if (rn_pos(f[i].tag) == rn_g.NOUN)
				pn = &f[i];
		if (pn && rn_reliable(pn)) {
			int hc = rn_has(pn->tag, rn_g.loc2) ? rn_g.loct : rn_case(pn->tag);
			return hc == rn_g.datv || hc == rn_g.ablt || hc == rn_g.loct ? hc : NONE;
		}
	}
	return NONE;
}

/* _get_preposition_before_number: the PREP_CASE entry, NULL */
static const rn_kv *prep_before(rn_ncase *c, long idx)
{
	int maxl = idx < 3 ? (int)idx : 3;
	for (int pl = maxl; pl > 1; pl--) {
		long st = idx - pl;
		int ok = 1;
		for (long i = st; i < idx && ok; i++) {
			const rn_str *w = word_at(c, (size_t)i);
			ok = w->n && !barrier(w);
		}
		for (long i = st; i < idx && ok; i++) {
			const rn_str *w = word_at(c, (size_t)i);
			int pos = rn_pos(rn_mfirst(w->s, w->n)->tag);
			ok = pos != rn_g.VERB && pos != rn_g.INFN && pos != rn_g.GRND;
		}
		if (!ok)
			continue;
		rn_str *j = &c->tmp2;
		j->n = 0;
		for (long i = st; i < idx; i++) {
			const rn_str *w = word_at(c, (size_t)i);
			if ((i > st && rn_str_putc(j, ' ') < 0) || rn_str_put(j, w->s, w->n) < 0)
				rn_mx_failed = 1;
		}
		const rn_kv *kv = rn_table_get32(RN_T_PREP_CASE, j->s, j->n);
		if (kv)
			return kv;
	}
	long stop = idx - 3 > -1 ? idx - 3 : -1;
	for (long i = idx - 1; i > stop; i--) {
		const rn_str *w = word_at(c, (size_t)i);
		if (eq(w->s, w->n, U"чем") || barrier(w))
			break;
		if ((eq(w->s, w->n, U"того") || eq(w->s, w->n, U"этого")) && i == idx - 1)
			break;
		const cmorphy3_form *p = rn_mfirst(w->s, w->n);
		int pos = rn_pos(p->tag);
		if (pos == rn_g.VERB || pos == rn_g.INFN || pos == rn_g.GRND)
			break;
		const rn_kv *kv = rn_table_get32(RN_T_PREP_CASE, w->s, w->n);
		if (kv)
			return kv;
		if (pos == rn_g.NPRO || (rn_reliable(p) && !nf_in(p, RN_T_TIME_CONTEXT_LEMMAS) &&
		                         !nf_in(p, RN_T_PARTITIVE_BOUNDARY_LEMMAS)))
			break;
	}
	return NULL;
}

static int utf8_eq_lower(const char *u, const uint32_t *s, size_t n)
{
	rn_str l = {0};
	char *b = NULL;
	size_t cap = 0;
	int r = 0;
	if (rn_py_lower(s, n, &l) == 0 && (b = rn_to_utf8(l.s, l.n, NULL, &cap)))
		r = !strcmp(u, b);
	else
		rn_mx_failed = 1;
	free(b);
	rn_str_free(&l);
	return r;
}

static int r_preposition_before_number(X *x)
{
	const rn_kv *pm = prep_before(x->c, x->idx);
	if (!pm)
		return NONE;
	const char *wl = pm->key;
	long lim5 = x->vlen - x->idx < 5 ? x->vlen - x->idx : 5;
	size_t n;
	const uint32_t *s;
	const rn_tok *tk = &x->c->t[x->idx];
	if (!strcmp(wl, "за") || !strcmp(wl, "под")) {
		for (long o = 1; o < lim5; o++) {
			s = raw(x, o, &n);
			const cmorphy3_form *rp = rn_mfirst(s, n);
			if (rn_reliable(rp)) {
				if (rn_case(rp->tag) == rn_g.ablt)
					return rn_g.ablt;
				break;
			}
		}
	}
	if (!strcmp(wl, "с") || !strcmp(wl, "со")) {
		for (long o = 1; o < lim5; o++) {
			s = raw(x, o, &n);
			rn_str *l = &x->c->tmp;
			l->n = 0;
			if (rn_py_lower(s, n, l) < 0)
				rn_mx_failed = 1;
			if (eq(l->s, l->n, U"до") || eq(l->s, l->n, U"по"))
				return rn_g.gent;
			const cmorphy3_form *rp = rn_mfirst(s, n);
			if (rn_reliable(rp)) {
				if (rn_case(rp->tag) == rn_g.ablt)
					return rn_g.ablt;
				break;
			}
		}
	}
	if (!strcmp(wl, "в") || !strcmp(wl, "на")) {
		if (weq(x, -1, U"в") || weq(x, -1, U"во")) {
			const cmorphy3_form *ih = parse(x, -2);
			s = raw(x, 1, &n);
			int alnum = 0;
			for (size_t i = 0; i < n && !alnum; i++)
				alnum = rn_py_isalnum(s[i]);
			if (ih && rn_reliable(ih) && loctish(ih) && (!n || !alnum))
				return rn_g.loct;
		}
		if (is_counting_group(x->c, x->idx, x->vlen))
			return rn_g.accs;
		long lim6 = x->vlen - x->idx < 6 ? x->vlen - x->idx : 6;
		for (long o = 1; o < lim6; o++) {
			s = raw(x, o, &n);
			if (has_any(s, n, U".!?;:,"))
				break;
			const cmorphy3_form *p = rn_mfirst(s, n);
			if (rn_reliable(p)) {
				if (loctish(p))
					return rn_g.loct;
				cmorphy3_form il, ia;
				int hl = rn_minflect(p, rn_gset1(rn_g.loct), &il);
				int ha = rn_minflect(p, rn_gset1(rn_g.accs), &ia);
				if (hl && utf8_eq_lower(il.word, s, n))
					return rn_g.loct;
				if (ha && utf8_eq_lower(ia.word, s, n))
					return rn_g.accs;
			}
			if (rn_has(p->tag, rn_g.VERB) || rn_has(p->tag, rn_g.INFN))
				break;
		}
		long lim4 = x->vlen - x->idx < 4 ? x->vlen - x->idx : 4;
		for (long o = 1; o < lim4; o++) {
			s = raw(x, o, &n);
			if (nf_in(rn_mfirst(s, n), RN_T_TIME_CONTEXT_LEMMAS)) {
				rn_int pi;
				if (rn_parse_int_token(tk->s, tk->n, &pi))
					return pi.v >= 1000 && pi.v <= 2100 ? rn_g.loct : rn_g.accs;
				return rn_g.loct;
			}
		}
		return rn_g.accs;
	}
	if (!strcmp(wl, "по")) {
		rn_int pi;
		if (rn_parse_int_token(tk->s, tk->n, &pi))
			return pi.m1000 % 10 == 1 && pi.m1000 % 100 != 11 ? rn_g.datv : rn_g.accs;
		return rn_g.datv;
	}
	return cmorphy3_grammeme(rn_morph, pm->value);
}

static long lss_stop(X *x, long back)
{
	long a = (long)x->c->lss[x->idx] - 1, b = x->idx - back;
	return a > b ? a : b;
}

static int r_adverbial_accusative(X *x)
{
	if (weq(x, 1, U"раз") && raw_eq(x, 1, U"раз"))
		return rn_g.accs;
	if (WIN(x, 1, TIME_WORDS) && WIN(x, 2, AGO_WORDS))
		return rn_g.accs;
	return NONE;
}

static int r_passive_participle_predicate(X *x)
{
	for (long i = x->idx - 1; i > lss_stop(x, 3); i--) {
		const cmorphy3_form *p = rn_mfirst(x->c->t[i].s, x->c->t[i].n);
		int pos = rn_pos(p->tag);
		if (pos == rn_g.PRTS && rn_has(p->tag, rn_g.pssv))
			return rn_g.nomn;
		if (pos == rn_g.NOUN || pos == rn_g.ADJF || pos == rn_g.PRTF)
			break;
	}
	return NONE;
}

static int r_adjective_agreement(X *x)
{
	int cg = is_counting_group(x->c, x->idx, x->vlen), blocked = 0;
	long lls = (long)x->c->lss[x->idx];
	const rn_tok *t = x->c->t;
	for (long i = x->idx - 1; i > lss_stop(x, 3); i--) {
		if (has_any(t[i].s, t[i].n, U",")) {
			lls = i + 1;
			break;
		}
		int pos = rn_pos(rn_mfirst(t[i].s, t[i].n)->tag);
		if (pos == rn_g.VERB || pos == rn_g.INFN || pos == rn_g.GRND) {
			lls = i + 1;
			break;
		}
		if (pos == rn_g.NOUN) {
			blocked = 1;
			break;
		}
	}
	for (long i = lls > x->idx - 2 ? lls : x->idx - 2; i < x->idx; i++) {
		rn_str *l = &x->c->tmp;
		l->n = 0;
		if (rn_py_lower(t[i].s, t[i].n, l) < 0)
			rn_mx_failed = 1;
		if (IN(TOTALIZING_PARTICLES, l->s, l->n))
			continue;
		const cmorphy3_form *p = rn_mfirst(t[i].s, t[i].n);
		int pos = rn_pos(p->tag), c = rn_case(p->tag);
		int adj = pos == rn_g.ADJF || pos == rn_g.PRTF;
		if (blocked && adj)
			continue;
		if (adj && c >= 0) {
			if (cg && c != rn_g.datv && c != rn_g.ablt && c != rn_g.loct)
				return NONE;
			return c;
		}
	}
	return NONE;
}

static int r_verb_government(X *x)
{
	const rn_tok *t = x->c->t;
	for (long i = x->idx - 1; i > lss_stop(x, 5); i--) {
		rn_str *wl = &x->c->tmp;
		lower_strip(t[i].s, t[i].n, PUNCT2, wl);
		const cmorphy3_form *f;
		int k = rn_mparse(wl->s, wl->n, &f);
		int tvb[64], *tv = k <= 64 ? tvb : malloc((size_t)k * sizeof *tv);
		if (!tv) {
			rn_mx_failed = 1;
			return NONE;
		}
		int nt = top_verbs(f, k, tv), r = PENDING;   /* PENDING: go on */
		for (int j = 0; j < nt && r == PENDING; j++) {
			const rn_kv *kv = rn_table_get(RN_T_VERB_CASE, f[tv[j]].normal_form);
			if (kv)
				r = cmorphy3_grammeme(rn_morph, kv->value);
		}
		if (r == PENDING && nt && WIN(x, 1, TIME_WORDS)) {
			int all = 1;
			for (int j = 0; j < nt && all; j++)
				all = nf_in(&f[tv[j]], RN_T_ELAPSE_VERB_LEMMAS) &&
				      rn_has(f[tv[j]].tag, rn_g.past) && rn_has(f[tv[j]].tag, rn_g.neut);
			if (all)
				r = NONE;
		}
		if (r == PENDING && nt) {
			int all = 1;
			for (int j = 0; j < nt && all; j++)
				all = rn_has(f[tv[j]].tag, rn_g.tran);
			if (all) {
				r = rn_g.accs;
				if (x->idx < x->vlen - 1) {
					size_t n;
					const uint32_t *s = raw(x, 1, &n);
					rn_str *wr = &x->c->tmp2;
					lower_strip(s, n, PUNCT2, wr);
					if (wr->n) {
						const cmorphy3_form *pr = rn_mfirst(wr->s, wr->n);
						int c = rn_case(pr->tag);
						if (rn_reliable(pr) && (c == rn_g.datv || c == rn_g.ablt ||
						                        c == rn_g.loct || rn_has(pr->tag, rn_g.loc2)))
							r = NONE;
					}
				}
			}
		}
		if (tv != tvb)
			free(tv);
		if (r != PENDING)
			return r;
		if (has_any(t[i].s, t[i].n, U".!?"))
			break;
	}
	return NONE;
}

static const cmorphy3_form *right_parse(X *x, rn_str **wr)
{
	if (x->idx >= x->vlen - 1)
		return NULL;
	size_t n;
	const uint32_t *s = raw(x, 1, &n);
	*wr = &x->c->tmp;
	lower_strip(s, n, PUNCT2, *wr);
	if (!(*wr)->n)
		return NULL;
	return rn_mfirst((*wr)->s, (*wr)->n);
}

static int r_entity_keyword_default(X *x)
{
	rn_str *wr;
	const cmorphy3_form *p = right_parse(x, &wr);
	if (!p)
		return NONE;
	int c = rn_case(p->tag);
	if (rn_reliable(p) && (c == rn_g.gent || c == rn_g.datv || c == rn_g.ablt ||
	                       c == rn_g.loct || rn_has(p->tag, rn_g.loc2)))
		return NONE;
	if (nf_in(p, RN_T_ENTITY_KEYWORDS) || IN(ENTITY_KEYWORDS, wr->s, wr->n))
		return rn_g.nomn;
	return NONE;
}

static int r_oblique_noun_on_the_right(X *x)
{
	rn_str *wr;
	const cmorphy3_form *p = right_parse(x, &wr);
	if (!p || !rn_reliable(p))
		return NONE;
	int c = rn_case(p->tag);
	if (c == rn_g.datv || c == rn_g.ablt || c == rn_g.loct || rn_has(p->tag, rn_g.loc2))
		return loctish(p) ? rn_g.loct : c;
	return NONE;
}

static int (*const RULES[])(X *) = {
	r_operand_after_arithmetic_verb,
	r_operand_before_multiplication,
	r_ratio_left_operand,
	r_dimension_first_coordinate,
	r_temperature_equality,
	r_comparison_or_equal,
	r_equality_before_degree_unit,
	r_power_exponent,
	r_distance_from_landmark,
	r_measurement_continuation,
	r_coordinated_first_number,
	r_range_right_boundary,
	r_fresh_quantity_after_dash,
	r_range_start_governed_by_left,
	r_instrumental_group,
	r_label_prefix,
	r_numbering_head,
	r_label_noun_head,
	r_genitive_quantity_governor,
	r_negated_existence,
	r_age_attribute,
	r_noun_head_genitive,
	r_locative_quantity_head,
	r_measure_parameter_head,
	r_oblique_noun_before_bare_number,
	r_coordinated_series_head,
	r_preposition_before_number,
	r_adverbial_accusative,
	r_passive_participle_predicate,
	r_adjective_agreement,
	r_verb_government,
	r_entity_keyword_default,
	r_oblique_noun_on_the_right,
};

/* run_rules; PENDING when a rule waits for a dependency (the result is discarded then:
 * resolve_case retries after solving it, so the remaining rules need not run) */
static int run_rules(X *x)
{
	for (size_t i = 0; i < sizeof RULES / sizeof *RULES; i++) {
		int d = RULES[i](x);
		if (d != NONE)
			return d;
	}
	return NONE;
}

int rn_ncase_get(rn_ncase *c, size_t idx, size_t vlen, int *explicit_)
{
	if (vlen > c->n)
		vlen = c->n;
	if (idx >= c->n)
		return -1;
	size_t cap = 16, top = 0;
	size_t *st = malloc(cap * sizeof *st);
	if (!st)
		return -1;
	st[top++] = idx;
	while (top) {
		size_t pos = st[top - 1];
		if (memo_find(c, pos, vlen, 0)) {
			top--;
			continue;
		}
		c->pending = -1;
		X x = {c, (long)pos, (long)vlen};
		int d = run_rules(&x);
		if (d == PENDING) {
			if (c->pending < 0 || (size_t)c->pending >= pos) {
				free(st);   /* dependencies point left; cannot happen */
				return -1;
			}
			if (top == cap) {
				size_t *s2 = realloc(st, (cap *= 2) * sizeof *st);
				if (!s2) {
					free(st);
					return -1;
				}
				st = s2;
			}
			st[top++] = (size_t)c->pending;
			continue;
		}
		memo *m = memo_find(c, pos, vlen, 1);
		if (!m) {
			free(st);
			return -1;
		}
		m->val = d;
		top--;
	}
	free(st);
	memo *m = memo_find(c, idx, vlen, 0);
	if (!m || rn_mx_failed)
		return -1;
	if (explicit_)
		*explicit_ = m->val != NONE;
	return m->val != NONE ? m->val : rn_g.nomn;
}

long rn_simple_tokenize(const uint32_t *s, size_t n, rn_tok **out)
{
	size_t cap = 16, k = 0;
	rn_tok *t = malloc(cap * sizeof *t);
	if (!t)
		return -1;
	rn_iter it = rn_iter_init(rn_pat(RN_TEXT_CONTEXT_TOKEN_PATTERN), s ? s : RN_U(""), n, 0);
	rn_match m;
	int r;
	while ((r = rn_iter_next(&it, &m)) == 1) {
		if (k == cap) {
			rn_tok *t2 = realloc(t, (cap *= 2) * sizeof *t);
			if (!t2) {
				free(t);
				return -1;
			}
			t = t2;
		}
		t[k++] = (rn_tok){m.s + m.start[0], m.end[0] - m.start[0]};
	}
	if (r < 0) {
		free(t);
		return -1;
	}
	*out = t;
	return (long)k;
}
