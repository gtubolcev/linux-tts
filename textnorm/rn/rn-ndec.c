/* numerals/decimals.py and numerals/fractions.py */
#include "rn-npass.h"
#include "rn-nx.h"
#include "rn-pat.h"
#include "rn-tables.h"

#define NEG_PH 0xE001u   /* NEGATIVE_NUMBER_PLACEHOLDER */

/* str(int(s)) into a malloc'ed string (NULL on ValueError: never for \d+) */
static char *pyint_dup(const uint32_t *s, size_t n)
{
	char *b = malloc(n + 3);
	if (!b) {
		nx_fail();
		return NULL;
	}
	if (nx_pyint(s, n, b, n + 3) < 0) {
		free(b);
		return NULL;
	}
	return b;
}

static int gram_of(const char *name)
{
	return name ? nx_gram(name) : -1;
}

static cmorphy3_gset gs2(int a, int b)
{
	cmorphy3_gset g = rn_gset1(a);
	return b >= 0 ? rn_gset_or(g, rn_gset1(b)) : g;
}

static cmorphy3_gset gs3(int a, int b, int c)
{
	cmorphy3_gset g = gs2(a, b);
	return c >= 0 ? rn_gset_or(g, rn_gset1(c)) : g;
}

/* the last word of w (split()) replaced: [a, n) */
static size_t last_word_start(const rn_str *w)
{
	size_t b = w->n;
	while (b > 0 && rn_py_isspace(w->s[b - 1]))
		b--;
	size_t a = b;
	while (a > 0 && !rn_py_isspace(w->s[a - 1]))
		a--;
	return a;
}

/* " ".join(w.split()) in place */
static void join_split(rn_str *w)
{
	size_t k = 0, i = 0;
	while (i < w->n) {
		while (i < w->n && rn_py_isspace(w->s[i]))
			i++;
		if (i >= w->n)
			break;
		if (k)
			w->s[k++] = ' ';
		while (i < w->n && !rn_py_isspace(w->s[i]))
			w->s[k++] = w->s[i++];
	}
	w->n = k;
}

/* ===================================================================== decimals */

static const char *const MULTIPLIERS[] = {"тысяча", "миллион", "миллиард", "триллион", NULL};

static int in8(const char *s, const char *const *l)
{
	for (; *l; l++)
		if (!strcmp(s, *l))
			return 1;
	return 0;
}

static const rn_unit *minor_unit(const char *lemma)
{
	static const rn_unit KOP = {"копейка", "femn", "money", NULL, {"копейка", NULL, 0, NULL, NULL, NULL, NULL, 0, 0}};
	static const rn_unit CENT = {"цент", "masc", "money", NULL, {"цент", NULL, 0, NULL, NULL, NULL, NULL, 0, 0}};
	static const rn_unit PENS = {"пенс", "masc", "money", NULL, {"пенс", NULL, 0, NULL, NULL, NULL, NULL, 0, 0}};
	if (!strcmp(lemma, "рубль") || !strcmp(lemma, "гривна"))
		return &KOP;
	if (!strcmp(lemma, "доллар") || !strcmp(lemma, "евро"))
		return &CENT;
	if (!strcmp(lemma, "фунт"))
		return &PENS;
	return NULL;
}

/* render_counted_money(descriptor, int(d), case) */
static void counted_money(rn_str *o, const rn_unit *u, const char *d, int gcase)
{
	nx_numeral_a(o, d, nx_icase(d, gcase), gram_of(u->gender), 0);
	nx_putc(o, ' ');
	nx_render_unit(o, u, nx_ttags(d, gcase), nx_nform(d) == 'f');
}

/* render_money_amount: 1 and appended, or 0 */
static int money_amount(rn_str *o, const rn_unit *u, const char *ints, const uint32_t *frac,
                        size_t fn, int gcase)
{
	if (strcmp(u->category, "money"))
		return 0;
	const rn_unit *minor = minor_unit(u->lemma);
	if (!minor || fn > 2)
		return 0;
	uint32_t padded[2] = {fn > 0 ? frac[0] : '0', fn > 1 ? frac[1] : '0'};
	char mv[8];
	if (nx_pyint(padded, 2, mv, sizeof mv) < 0)
		return 0;
	rn_unit major = *u;
	major.suffix = NULL;
	counted_money(o, &major, ints, gcase);
	if (strcmp(mv, "0")) {
		nx_putc(o, ' ');
		counted_money(o, minor, mv, gcase);
	}
	if (u->suffix) {
		nx_putc(o, ' ');
		nx_put8(o, u->suffix);
	}
	return 1;
}

/* _render_half_multiplier: 1 and appended, or 0 */
static int half_multiplier(rn_str *o, const rn_unit *u, const char *iv, const uint32_t *ints,
                           size_t in, int gcase)
{
	int femn = !strcmp(u->lemma, "тысяча");
	if (!strcmp(iv, "1")) {
		if (gcase == rn_g.nomn || gcase == rn_g.accs) {
			nx_lit(o, femn ? U"полторы " : U"полтора ");
			nx_render_unit(o, u, nx_gs("gent sing"), 0);
		} else {
			nx_lit(o, U"полутора ");
			nx_render_unit(o, u, gs2(gcase, rn_g.plur), 0);
		}
		return 1;
	}
	unsigned v100 = nx_dmod(iv, 100);
	if (v100 % 10 == 1 && v100 != 11)
		return 0;
	int g = femn ? rn_g.femn : rn_g.masc;
	nx_numeral(o, ints, in, nx_icase(iv, gcase), g, 0);
	nx_lit(o, U" с половиной ");
	nx_render_unit(o, u, nx_ttags(iv, gcase), nx_nform(iv) == 'f');
	return 1;
}

/* inflect_fraction_numerator */
static void fraction_numerator(rn_str *o, const uint32_t *s, size_t n, int gcase)
{
	char *d = pyint_dup(s, n);
	if (!d) {
		nx_put(o, s, n);
		return;
	}
	gcase = nx_icase(d, gcase);
	rn_str w = {0};
	nx_numeral(&w, s, n, gcase, -1, -1);
	if (!strcmp(d, "0")) {
		nx_put(o, w.s, w.n);
		goto out;
	}
	size_t a = last_word_start(&w);
	size_t b = w.n;
	while (b > a && rn_py_isspace(w.s[b - 1]))
		b--;
	unsigned v100 = nx_dmod(d, 100);
	cmorphy3_gset tags;
	int have = 0;
	if (v100 % 10 == 1 && v100 != 11) {
		tags = gs3(gcase, rn_g.femn, rn_g.sing);
		have = 1;
	} else if (v100 % 10 == 2 && v100 != 12 && (gcase == rn_g.nomn || gcase == rn_g.accs)) {
		tags = gs2(gcase, rn_g.femn);
		have = 1;
	}
	if (!have || a == b) {
		nx_put(o, w.s, w.n);
		goto out;
	}
	const cmorphy3_form *f = rn_mfirst(w.s + a, b - a);
	rn_str inf = {0};
	if (!nx_inflect(&inf, f, tags)) {
		nx_put(o, w.s, w.n);
		rn_str_free(&inf);
		goto out;
	}
	rn_str parts = {0};
	nx_put(&parts, w.s, a);
	nx_put(&parts, inf.s, inf.n);
	join_split(&parts);
	nx_put(o, parts.s, parts.n);
	rn_str_free(&parts);
	rn_str_free(&inf);
out:
	rn_str_free(&w);
	free(d);
}

/* render_fraction_order_words */
static void fraction_order(rn_str *o, size_t digits, const char *fv, int gcase)
{
	if (!digits)
		return;
	char *ten = malloc(digits + 2);
	if (!ten) {
		nx_fail();
		return;
	}
	ten[0] = '1';
	memset(ten + 1, '0', digits);
	ten[digits + 1] = 0;
	rn_str base = {0};
	if (nx_n2w(&base, 1, ten, N2W_DEFAULTS) < 0) {
		static const char32_t *const FB[] = {NULL, U"десятая", U"сотая", U"тысячная",
		                                     U"десятитысячная", U"стотысячная", U"миллионная"};
		base.n = 0;
		nx_lit(&base, digits <= 6 ? FB[digits] : U"десятитысячная");
	}
	free(ten);
	const cmorphy3_form *f = rn_mfirst(base.s, base.n);
	unsigned v100 = nx_dmod(fv, 100);
	cmorphy3_gset tags = v100 % 10 == 1 && v100 != 11 ? gs3(gcase, rn_g.femn, rn_g.sing)
	                     : gcase == rn_g.nomn || gcase == rn_g.accs ? nx_gs("gent plur")
	                                                                : gs2(gcase, rn_g.plur);
	if (!nx_inflect(o, f, tags)) {
		/* safe_inflect: the heuristic for gent plur ADJF, then the fallback */
		rn_str t = {0};
		size_t cap = base.n * 4 + 1;
		char *u8 = malloc(cap);
		if (u8 && rn_to_utf8(base.s, base.n, u8, &cap)) {
			nx_safe_inflect(&t, f, tags, u8);
			nx_put(o, t.s, t.n);
		} else
			nx_fail();
		free(u8);
		rn_str_free(&t);
	}
	rn_str_free(&base);
}

static int ambiguous_prep(const uint32_t *s, size_t n)
{
	rn_str l = {0};
	nx_lower(s, n, &l);
	size_t a = 0, b = l.n;
	while (a < b && nx_inset(l.s[a], U".,:;!\"«»()[]{}"))
		a++;
	while (b > a && nx_inset(l.s[b - 1], U".,:;!\"«»()[]{}"))
		b--;
	int r = 0;
	if (b > a) {
		int sp = 0;
		for (size_t i = a; i < b; i++)
			sp |= l.s[i] == ' ';
		r = !sp && rn_in32(PREP_CASE, l.s + a, b - a);
	}
	rn_str_free(&l);
	return r;
}

static int skip_unit_candidate(const uint32_t *u, size_t un, const uint32_t *t, size_t tn,
                               size_t st)
{
	if (!ambiguous_prep(u, un))
		return 0;
	while (st < tn && rn_py_isspace(t[st]))
		st++;
	if (st >= tn)
		return 0;
	rn_match m;
	int r = rn_match_at(rn_pat(RN_NUMERALS_DECIMALS_L217), t, st + 1, st, &m);
	if (r < 0)
		nx_fail();
	return r > 0;
}

static int decimal_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = rn_pat(RN_NUMERALS_DECIMALS_DECIMAL_PATTERN);
	const uint32_t *T = m->s;
	size_t TN = m->n;
	size_t numn, un = 0, u2n = 0;
	const uint32_t *num = nx_grp(m, p, "num", &numn);
	const uint32_t *unit = nx_grp(m, p, "unit", &un);
	const uint32_t *unit2 = nx_grp(m, p, "unit2", &u2n);
	const uint32_t *udot = nx_grp(m, p, "unit_dot", &(size_t){0});
	const uint32_t *u2dot = nx_grp(m, p, "unit2_dot", &(size_t){0});
	rn_str s = {0};
	nx_put(&s, num, numn);
	for (size_t i = 0; i < s.n; i++)
		if (s.s[i] == ',')
			s.s[i] = '.';
	size_t st = m->start[0], a0 = nx_left(st, 40);
	rn_tok *L = NULL;
	long nl = rn_simple_tokenize(T + a0, st - a0, &L);
	int gcase = rn_g.nomn;
	if (nl < 0)
		nx_fail();
	else {
		rn_tok *all = malloc((size_t)(nl + 2) * sizeof *all);
		if (!all)
			nx_fail();
		else {
			if (nl)
				memcpy(all, L, (size_t)nl * sizeof *all);
			all[nl] = (rn_tok){s.s, s.n};
			size_t cnt = (size_t)nl + 1;
			if (unit)
				all[cnt++] = (rn_tok){unit, un};
			rn_ncase *c = rn_ncase_new(all, cnt);
			int r = c ? rn_ncase_get(c, (size_t)nl, SIZE_MAX, NULL) : -1;
			rn_ncase_free(c);
			if (r < 0)
				nx_fail();
			else
				gcase = r;
			free(all);
		}
	}
	free(L);
	int neg = s.n && (s.s[0] == '-' || s.s[0] == NEG_PH);
	size_t a = 0;
	while (a < s.n && (s.s[a] == '-' || s.s[a] == NEG_PH))
		a++;
	size_t dot = a;
	while (dot < s.n && s.s[dot] != '.')
		dot++;
	const uint32_t *ints = s.s + a, *frac = s.s + dot + 1;
	size_t in = dot - a, fn = s.n - dot - 1;
	char *iv = pyint_dup(ints, in), *fv = pyint_dup(frac, fn);
	rn_str res = {0};
	if (!iv || !fv) {
		nx_whole(m, o);
		goto out;
	}
	int rc = strcmp(iv, "0") ? gcase : rn_g.nomn;
	nx_numeral(&res, ints, in, nx_icase(iv, rc), rn_g.femn, -1);
	nx_putc(&res, ' ');
	{
		const cmorphy3_form *cel = rn_mfirst(U"целая", 5);
		unsigned v100 = nx_dmod(iv, 100);
		cmorphy3_gset tc = v100 % 10 == 1 && v100 != 11 ? gs3(rc, rn_g.femn, rn_g.sing)
		                   : rc == rn_g.nomn || rc == rn_g.accs ? nx_gs("gent plur")
		                                                        : gs2(rc, rn_g.plur);
		nx_safe_inflect(&res, cel, tc, NULL);
	}
	nx_putc(&res, ' ');
	fraction_numerator(&res, frac, fn, rc);
	nx_putc(&res, ' ');
	fraction_order(&res, fn, fv, rc);
	if (unit) {
		const rn_unit *ui = NULL;
		int consumes = 0;
		if (unit2) {
			/* should_skip_combined_unit_candidate: unit2 stripped of PUNCT_STRIP */
			size_t x = 0, y = u2n;
			while (x < y && nx_inset(unit2[x], U".,:;!\"«»()[]{}"))
				x++;
			while (y > x && nx_inset(unit2[y - 1], U".,:;!\"«»()[]{}"))
				y--;
			if (!(y > x && ambiguous_prep(unit2 + x, y - x))) {
				rn_str cand = {0};
				for (int k = udot ? 0 : 1; k < 2 && !ui; k++) {
					cand.n = 0;
					nx_put(&cand, unit, un);
					if (k == 0)
						nx_putc(&cand, '.');
					nx_put(&cand, unit2, u2n);
					ui = rn_unit_resolve(cand.s, cand.n);
				}
				rn_str_free(&cand);
				consumes = ui != NULL;
			}
		}
		if (!ui && !skip_unit_candidate(unit, un, T, TN, nx_gend(m, p, "unit"))) {
			ui = rn_unit_resolve(unit, un);
			if (!ui) {
				const rn_unit *f = rn_unit_resolve_form(unit, un);
				if (f && ((f->m.paradigm && *f->m.paradigm) || f->m.indeclinable || !strcmp(f->category, "money")))
					ui = f;
			}
		}
		int trailing = udot && unit2 && nx_keep_dot_at(T, TN, nx_gstart(m, p, "unit2"));
		if (ui) {
			int done = 0;
			if (in8(ui->lemma, MULTIPLIERS) && fn == 1 && frac[0] == '5' && strcmp(iv, "0") &&
			    !neg) {
				rn_str h = {0};
				if (half_multiplier(&h, ui, iv, ints, in, rc)) {
					res.n = 0;
					nx_put(&res, h.s, h.n);
					done = 1;
				}
				rn_str_free(&h);
			}
			if (!done) {
				rn_str mo = {0};
				if (!consumes && money_amount(&mo, ui, iv, frac, fn, gcase)) {
					res.n = 0;
					nx_put(&res, mo.s, mo.n);
				} else {
					nx_putc(&res, ' ');
					nx_render_unit(&res, ui, nx_gs("gent sing"), 0);
				}
				rn_str_free(&mo);
			}
			int processed = consumes;
			if (!consumes && unit2) {
				const rn_unit *u2 = rn_unit_resolve(unit2, u2n);
				if (in8(ui->lemma, MULTIPLIERS) && u2) {
					if (!strcmp(u2->lemma, "человек"))
						nx_lit(&res, U" человек");
					else {
						nx_putc(&res, ' ');
						nx_render_unit(&res, u2, nx_gs("gent plur"), 0);
					}
					processed = 1;
				}
			}
			if (unit2 && !processed) {
				if (trailing)
					nx_putc(&res, '.');
				nx_putc(&res, ' ');
				nx_put(&res, unit2, u2n);
			}
		} else {
			nx_putc(&res, ' ');
			nx_put(&res, unit, un);
			if (unit2) {
				if (trailing)
					nx_putc(&res, '.');
				nx_putc(&res, ' ');
				nx_put(&res, unit2, u2n);
			}
		}
		if (((u2dot && unit2) || (udot && !unit2)) && nx_keep_dot_at(T, TN, m->end[0]))
			nx_putc(&res, '.');
	}
	if (neg)
		nx_lit(o, U"минус ");
	if (nx_grp(m, p, "range_sep", &(size_t){0}))
		nx_lit(o, U" - ");
	nx_put(o, res.s, res.n);
out:
	free(iv);
	free(fv);
	rn_str_free(&res);
	rn_str_free(&s);
	return 0;
}

int rn_n_decimals(rn_str *t)
{
	nx_resub(rn_pat(RN_NUMERALS_DECIMALS_DECIMAL_PATTERN), t, decimal_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

/* ===================================================================== fractions */

#define F(x) rn_pat(RN_NUMERALS_FRACTIONS_##x)

/* the text the oracle's closures see: normalize_fractions' `text` after the half
 * attribute sub; later subs index it with their own positions (QUIRKS) */
typedef struct {
	const uint32_t *s;
	size_t n;
} ftext;

static int pressure_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t st = m->start[0];
	if (!nx_found(F(L89), m->s, nx_left(st, 80), st))
		return nx_whole(m, o);
	const rn_re *p = F(PRESSURE_RATIO_PATTERN);
	size_t pn;
	const uint32_t *prep = nx_grp(m, p, "prep", &pn);
	if (prep && pn) {
		rn_str cf = {0};
		rn_py_casefold(prep, pn, &cf);
		if (nx_eq(cf.s, cf.n, U"с"))
			nx_lit(o, U"со");
		else
			nx_put(o, prep, pn);
		rn_str_free(&cf);
		nx_putc(o, ' ');
	}
	nx_putg(o, m, p, "left");
	nx_lit(o, U" на ");
	nx_putg(o, m, p, "right");
	return 0;
}

static int refslash_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = F(REFERENCE_SLASH_PATTERN);
	size_t ln, rn;
	const uint32_t *l = nx_grp(m, p, "left", &ln);
	const uint32_t *r = nx_grp(m, p, "right", &rn);
	char *a = pyint_dup(l, ln), *b = pyint_dup(r, rn);
	rn_str x = {0};
	int ok = a && b;
	if (ok) {
		nx_putg(&x, m, p, "head");
		nx_putc(&x, ' ');
		ok = nx_n2w(&x, 0, a, N2W_DEFAULTS) == 0;
		nx_lit(&x, U" дробь ");
		ok = ok && nx_n2w(&x, 0, b, N2W_DEFAULTS) == 0;
	}
	if (ok)
		nx_put(o, x.s, x.n);
	else
		nx_whole(m, o);
	rn_str_free(&x);
	free(a);
	free(b);
	return 0;
}

/* fraction_case(start) over the closure text */
static int fraction_case(const ftext *ft, size_t start)
{
	if (start > ft->n)
		start = ft->n;   /* Python slicing clamps */
	size_t a = nx_left(start, 16);
	rn_str c = {0};
	nx_lower(ft->s + a, start - a, &c);
	int r = rn_g.nomn;
	if (nx_found(F(GENITIVE_FRACTION_CONTEXT_PATTERN), c.s, 0, c.n))
		r = rn_g.gent;
	else if (nx_found(F(DATIVE_FRACTION_CONTEXT_PATTERN), c.s, 0, c.n))
		r = rn_g.datv;
	else if (nx_found(F(ACCUSATIVE_FRACTION_CONTEXT_PATTERN), c.s, 0, c.n))
		r = rn_g.accs;
	else if (nx_found(F(PREPOSITIONAL_FRACTION_CONTEXT_PATTERN), c.s, 0, c.n))
		r = rn_g.loct;
	else {
		size_t b = nx_left(start, 40);
		rn_str t = {0};
		nx_put(&t, ft->s + b, start - b);
		nx_lit(&t, U" 0");
		rn_tok *toks = NULL;
		long k = rn_simple_tokenize(t.s, t.n, &toks);
		if (k < 0)
			nx_fail();
		else if (k > 0) {
			rn_ncase *nc = rn_ncase_new(toks, (size_t)k);
			int g = nc ? rn_ncase_get(nc, (size_t)k - 1, SIZE_MAX, NULL) : -1;
			rn_ncase_free(nc);
			if (g < 0)
				nx_fail();
			else if (g != rn_g.nomn)
				r = g;
		}
		free(toks);
		rn_str_free(&t);
	}
	rn_str_free(&c);
	return rn_canon_case(r);
}

/* render_fraction: 1 and appended, or 0 (None) */
static int render_fraction(rn_str *o, const char *num, const char *den, int gcase)
{
	unsigned n100 = nx_dmod(num, 100), d10 = n100 % 10;
	int ngender = (d10 == 1 || d10 == 2) && n100 != 11 && n100 != 12 ? rn_g.femn : -1;
	rn_str nt = {0}, dt = {0}, out = {0};
	int ok = 0;
	nx_numeral_a(&nt, num, gcase, ngender, 0);
	if (ngender >= 0) {
		size_t a = last_word_start(&nt), b = nt.n;
		while (b > a && rn_py_isspace(nt.s[b - 1]))
			b--;
		if (b > a) {
			nx_selq q = NX_SELQ;
			q.pos[0] = rn_g.NUMR;
			q.pos[1] = rn_g.ADJF;
			q.pref = nx_gs("Anum");
			const cmorphy3_form *sel = nx_select_w(nt.s + a, b - a, &q);
			if (sel) {
				cmorphy3_gset tags = gs3(gcase, ngender, d10 == 1 ? rn_g.sing : -1);
				size_t cap = (b - a) * 4 + 1;
				char *fb = malloc(cap);
				rn_str nw = {0};
				if (fb && rn_to_utf8(nt.s + a, b - a, fb, &cap)) {
					nx_safe_inflect(&nw, sel, tags, fb);
					nt.n = a;
					nx_put(&nt, nw.s, nw.n);
					join_split(&nt);
				} else
					nx_fail();
				free(fb);
				rn_str_free(&nw);
			}
		}
	}
	if (nx_n2w(&dt, 1, den, N2W_DEFAULTS) < 0)
		goto out;
	/* words = denom_text.split() */
	join_split(&dt);
	size_t fs = 0;
	while (fs < dt.n && dt.s[fs] != ' ')
		fs++;
	if (fs < dt.n) {
		const cmorphy3_form *f0 = rn_mfirst(dt.s, fs);
		if (rn_pos(f0->tag) == rn_g.NUMR) {
			rn_str w0 = {0};
			if (nx_inflect(&w0, f0, rn_gset1(rn_g.gent))) {
				rn_str nd = {0};
				nx_put(&nd, w0.s, w0.n);
				nx_put(&nd, dt.s + fs, dt.n - fs);
				rn_str_free(&dt);
				dt = nd;
			}
			rn_str_free(&w0);
		}
	}
	int sing1 = d10 == 1 && n100 != 11;
	int third = d10 >= 1 && d10 <= 4 && !(n100 >= 11 && n100 <= 14);
	nx_put(&out, nt.s, nt.n);
	nx_putc(&out, ' ');
	if (!strcmp(den, "3") && (gcase == rn_g.nomn || gcase == rn_g.accs) && third) {
		nx_lit(&out, sing1 ? U"треть" : U"трети");
		ok = 1;
		goto out;
	}
	size_t ls = last_word_start(&dt);
	const cmorphy3_form *pl = rn_mfirst(dt.s + ls, dt.n - ls);
	int mag = nx_dge1000(num) && nx_dmod(num, 1000) == 0;
	cmorphy3_gset tags = sing1 ? gs3(gcase, rn_g.femn, rn_g.sing)
	                     : gcase == rn_g.nomn || gcase == rn_g.accs || mag ? nx_gs("gent plur")
	                                                                       : gs2(gcase, rn_g.plur);
	rn_str inf = {0};
	if (nx_inflect(&inf, pl, tags)) {
		dt.n = ls;
		nx_put(&dt, inf.s, inf.n);
	}
	rn_str_free(&inf);
	nx_put(&out, dt.s, dt.n);
	ok = 1;
out:
	if (ok)
		nx_put(o, out.s, out.n);
	rn_str_free(&nt);
	rn_str_free(&dt);
	rn_str_free(&out);
	return ok;
}

/* the next word after end in s (_NEXT_WORD_PATTERN.match): 1 and [*a, *b) */
static int next_word(const uint32_t *s, size_t n, size_t end, size_t *a, size_t *b)
{
	if (end > n)
		return 0;
	rn_match m;
	int r = rn_match_at(F(NEXT_WORD_PATTERN), s, n, end, &m);
	if (r < 0)
		nx_fail();
	if (r <= 0)
		return 0;
	*a = m.start[1];
	*b = m.end[1];
	return 1;
}

static int preposition_reading(const uint32_t *u, size_t un, const uint32_t *s, size_t n,
                               size_t end)
{
	rn_str k = {0};
	rn_py_casefold(u, un, &k);
	size_t x = 0, y = k.n;
	while (x < y && k.s[x] == '.')
		x++;
	while (y > x && k.s[y - 1] == '.')
		y--;
	cmorphy3_gset cases = {0, 0};
	int known = 1;
	if (nx_eq(k.s + x, y - x, U"в"))
		cases = nx_gs("accs loct");
	else if (nx_eq(k.s + x, y - x, U"у"))
		cases = nx_gs("gent");
	else if (nx_eq(k.s + x, y - x, U"к"))
		cases = nx_gs("datv");
	else
		known = 0;
	rn_str_free(&k);
	size_t a, b;
	if (!known || !next_word(s, n, end, &a, &b))
		return 0;
	const cmorphy3_form *f;
	rn_mparse(s + a, b - a, &f);
	int pos = rn_pos(f[0].tag), c = rn_case(f[0].tag);
	return (pos == rn_g.NOUN || pos == rn_g.NPRO || pos == rn_g.ADJF || pos == rn_g.PRTF) &&
	       c >= 0 && cmorphy3_gset_has(cases, c);
}

/* render_half_unit: 1 and appended */
static int half_unit(rn_str *o, const char *iv, int gcase, const rn_unit *u)
{
	int g = gram_of(u->gender);
	if (!strcmp(iv, "1")) {
		if (gcase == rn_g.nomn || gcase == rn_g.accs) {
			nx_lit(o, g == rn_g.femn ? U"полторы " : U"полтора ");
			nx_render_unit(o, u, nx_gs("gent sing"), 0);
		} else {
			nx_lit(o, U"полутора ");
			nx_render_unit(o, u, gs2(gcase, rn_g.plur), 0);
		}
		return 1;
	}
	nx_numeral_a(o, iv, gcase, g, 0);
	nx_lit(o, U" с половиной ");
	nx_render_unit(o, u, nx_ttags(iv, gcase), nx_nform(iv) == 'f');
	return 1;
}

/* render_half_word: 1 and appended, or 0 */
static int half_word(rn_str *o, const char *iv, int gcase, const ftext *ft, size_t end)
{
	if (!strcmp(iv, "1")) {
		nx_selq q = NX_SELQ;
		q.pos[0] = rn_g.NUMR;
		const cmorphy3_form *f = nx_select8("полтора", &q);
		if (!f)
			return 0;
		int c = rn_canon_case(gcase);
		if (c == rn_g.nomn || c == rn_g.accs) {
			nx_lit(o, U"полтора");
			return 1;
		}
		return nx_inflect(o, f, rn_gset1(c));
	}
	int g = -1;
	size_t a, b;
	if (next_word(ft->s, ft->n, end, &a, &b)) {
		rn_str l = {0};
		nx_lower(ft->s + a, b - a, &l);
		nx_selq q = NX_SELQ;
		q.pos[0] = rn_g.NOUN;
		const cmorphy3_form *f = nx_select_w(l.s, l.n, &q);
		if (f)
			g = rn_gender(f->tag);
		rn_str_free(&l);
	}
	nx_numeral_a(o, iv, gcase, g, 0);
	nx_lit(o, U" с половиной");
	return 1;
}

static const rn_unit *unit_or_form(const uint32_t *u, size_t un)
{
	const rn_unit *r = rn_unit_resolve(u, un);
	return r ? r : rn_unit_resolve_form(u, un);
}

static cmorphy3_gset gs_unit(const rn_unit *u)
{
	int g = gram_of(u->gender);
	return g >= 0 ? rn_gset_or(nx_gs("gent sing"), rn_gset1(g)) : nx_gs("gent sing");
}

static int mixed_repl(const rn_match *m, rn_str *o, void *ctx)
{
	const ftext *ft = ctx;
	const rn_re *p = F(MIXED_FRACTION_PATTERN);
	size_t xn, nn, dn, un;
	const uint32_t *xs = nx_grp(m, p, "int", &xn);
	const uint32_t *ns = nx_grp(m, p, "num", &nn);
	const uint32_t *ds = nx_grp(m, p, "denom", &dn);
	const uint32_t *unit = nx_grp(m, p, "unit", &un);
	char *iv = pyint_dup(xs, xn), *num = pyint_dup(ns, nn), *den = pyint_dup(ds, dn);
	rn_str r = {0};
	if (!iv || !num || !den) {
		nx_whole(m, o);
		goto out;
	}
	int gcase = fraction_case(ft, m->start[0]);
	const rn_unit *ui = NULL;
	size_t tail_a = 0, tail_b = 0;
	if (unit) {
		if (!preposition_reading(unit, un, m->s, m->n, m->end[0]))
			ui = unit_or_form(unit, un);
		if (!ui) {
			tail_a = nx_gend(m, p, "denom");
			tail_b = m->end[0];
		}
	}
	if (!strcmp(num, "1") && !strcmp(den, "2")) {
		int ok;
		if (ui) {
			int hc = gcase;
			if (gcase == rn_g.datv) {
				size_t st = m->start[0] > ft->n ? ft->n : m->start[0];
				size_t a = nx_left(st, 8);
				rn_str l = {0};
				nx_lower(ft->s + a, st - a, &l);
				if (nx_found(F(L303), l.s, 0, l.n))
					hc = rn_g.accs;
				rn_str_free(&l);
			}
			ok = half_unit(&r, iv, hc, ui);
		} else
			ok = half_word(&r, iv, gcase, ft, nx_gend(m, p, "denom"));
		if (!ok) {
			nx_whole(m, o);
			goto out;
		}
		nx_put(o, r.s, r.n);
		nx_put(o, m->s + tail_a, tail_b - tail_a);
		goto out;
	}
	rn_str fw = {0};
	if (!render_fraction(&fw, num, den, gcase)) {
		rn_str_free(&fw);
		nx_whole(m, o);
		goto out;
	}
	nx_numeral_a(&r, iv, gcase, rn_g.femn, 0);
	nx_putc(&r, ' ');
	{
		const cmorphy3_form *cel = rn_mfirst(U"целая", 5);
		unsigned v100 = nx_dmod(iv, 100);
		cmorphy3_gset tc = v100 % 10 == 1 && v100 != 11 ? gs3(gcase, rn_g.femn, rn_g.sing)
		                   : gcase == rn_g.nomn || gcase == rn_g.accs ? nx_gs("gent plur")
		                                                              : gs2(gcase, rn_g.plur);
		nx_safe_inflect(&r, cel, tc, "целых");
	}
	nx_putc(&r, ' ');
	nx_put(&r, fw.s, fw.n);
	rn_str_free(&fw);
	if (ui) {
		nx_putc(&r, ' ');
		nx_render_unit(&r, ui, gs_unit(ui), 0);
	} else
		nx_put(&r, m->s + tail_a, tail_b - tail_a);
	nx_put(o, r.s, r.n);
out:
	free(iv);
	free(num);
	free(den);
	rn_str_free(&r);
	return 0;
}

static int frac_unit_common(const rn_match *m, rn_str *o, const ftext *ft, const rn_re *p,
                            const rn_unit *ui)
{
	size_t nn, dn;
	const uint32_t *ns = nx_grp(m, p, "num", &nn);
	const uint32_t *ds = nx_grp(m, p, "denom", &dn);
	char *num = pyint_dup(ns, nn), *den = pyint_dup(ds, dn);
	rn_str r = {0};
	if (num && den && render_fraction(&r, num, den, fraction_case(ft, m->start[0]))) {
		nx_putc(&r, ' ');
		nx_render_unit(&r, ui, gs_unit(ui), 0);
		nx_put(o, r.s, r.n);
	} else
		nx_whole(m, o);
	rn_str_free(&r);
	free(num);
	free(den);
	return 0;
}

static int with_unit_repl(const rn_match *m, rn_str *o, void *ctx)
{
	const rn_re *p = F(FRACTION_UNIT_PATTERN);
	size_t un;
	const uint32_t *unit = nx_grp(m, p, "unit", &un);
	if (preposition_reading(unit, un, m->s, m->n, m->end[0]))
		return nx_whole(m, o);
	const rn_unit *ui = unit_or_form(unit, un);
	if (!ui)
		return nx_whole(m, o);
	return frac_unit_common(m, o, ctx, p, ui);
}

static int compound_unit_repl(const rn_match *m, rn_str *o, void *ctx)
{
	const rn_re *p = F(FRACTION_COMPOUND_UNIT_PATTERN);
	size_t un;
	const uint32_t *unit = nx_grp(m, p, "unit", &un);
	rn_str u = {0};
	for (size_t i = 0; i < un; i++)
		if (unit[i] != ' ')
			nx_putc(&u, unit[i]);
	const rn_unit *ui = rn_unit_resolve(u.s, u.n);
	rn_str_free(&u);
	if (!ui)
		return nx_whole(m, o);
	return frac_unit_common(m, o, ctx, p, ui);
}

static int plain_repl(const rn_match *m, rn_str *o, void *ctx)
{
	size_t nn, dn;
	const uint32_t *ns = rn_group(m, 1, &nn);
	const uint32_t *ds = rn_group(m, 2, &dn);
	char *num = pyint_dup(ns, nn), *den = pyint_dup(ds, dn);
	if (!(num && den && render_fraction(o, num, den, fraction_case(ctx, m->start[0]))))
		nx_whole(m, o);
	free(num);
	free(den);
	return 0;
}

int rn_n_fractions(rn_str *t)
{
	nx_resub(F(PRESSURE_RATIO_PATTERN), t, pressure_repl, NULL);
	nx_resub(F(REFERENCE_SLASH_PATTERN), t, refslash_repl, NULL);
	if (rn_resub_tmpl(F(HALF_ATTRIBUTE_PATTERN), t, RN_U("полу")) < 0)
		nx_fail();
	rn_str text = {0};
	nx_put(&text, t->s, t->n);
	ftext ft = {text.s, text.n};
	/* R20 (RN_FIX_FRAC_STALE_TEXT): each sub sees the text it rewrites */
	static const struct {
		enum rn_pattern pat;
		rn_repl_fn fn;
	} SUBS[] = {
		{RN_NUMERALS_FRACTIONS_MIXED_FRACTION_PATTERN, mixed_repl},
		{RN_NUMERALS_FRACTIONS_FRACTION_COMPOUND_UNIT_PATTERN, compound_unit_repl},
		{RN_NUMERALS_FRACTIONS_FRACTION_UNIT_PATTERN, with_unit_repl},
		{RN_NUMERALS_CONSTANTS_FRACTION_PATTERN, plain_repl},
	};
	for (size_t i = 0; i < sizeof SUBS / sizeof *SUBS; i++) {
		if (i && (rn_fixes & RN_FIX_FRAC_STALE_TEXT)) {
			text.n = 0;
			nx_put(&text, t->s, t->n);
			ft = (ftext){text.s, text.n};
		}
		nx_resub(rn_pat(SUBS[i].pat), t, SUBS[i].fn, &ft);
	}
	rn_str_free(&text);
	return rn_mx_failed ? -1 : 0;
}
