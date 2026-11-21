/* numerals/cardinals.py normalize_numeric_unit_ranges: "3-5 км", "обмен 3-5 квартирами" */
#include "rn-npass.h"
#include "rn-nx.h"
#include "rn-pat.h"
#include "rn-pre.h"

static int gram_of(const char *name)
{
	return name ? nx_gram(name) : -1;
}

static const char *const MULTIPLIERS[] = {"тысяча", "миллион", "миллиард", "триллион", NULL};

static int is_multiplier(const rn_unit *u)
{
	for (const char *const *m = MULTIPLIERS; *m; m++)
		if (!strcmp(u->lemma, *m))
			return 1;
	return 0;
}

/* next(p for p in parse_word(w) if POS in pos) */
static const cmorphy3_form *first_pos(const uint32_t *s, size_t n, int pos1, int pos2)
{
	const cmorphy3_form *f;
	int np = rn_mparse(s, n, &f);
	for (int i = 0; i < np; i++) {
		int p = rn_pos(f[i].tag);
		if (pos1 == rn_g.NOUN ? rn_has(f[i].tag, rn_g.NOUN) : (p == pos1 || p == pos2))
			return &f[i];
	}
	return NULL;
}

/* _resolve_range_unit_info */
static const rn_unit *range_unit(const uint32_t *raw, size_t rn)
{
	const rn_unit *d = rn_unit_resolve(raw, rn);
	if (d)
		return d;
	rn_str low = {0};
	nx_lower(raw, rn, &low);
	size_t a = 0, b = low.n;
	while (a < b && low.s[a] == '.')
		a++;
	while (b > a && low.s[b - 1] == '.')
		b--;
	rn_tok parts[3];
	int np = 0;
	for (size_t i = a; i < b;) {
		while (i < b && rn_py_isspace(low.s[i]))
			i++;
		size_t j = i;
		while (j < b && !rn_py_isspace(low.s[j]))
			j++;
		if (j > i) {
			if (np < 3)
				parts[np] = (rn_tok){low.s + i, j - i};
			np++;
		}
		i = j;
	}
	const rn_unit *r = NULL;
	/* the pattern allows at most two extra words; three-word lemmas do not exist */
	if (np == 2) {
		const cmorphy3_form *noun = first_pos(parts[1].s, parts[1].n, rn_g.NOUN, -1);
		const cmorphy3_form *head =
		    noun ? first_pos(parts[0].s, parts[0].n, rn_g.ADJF, rn_g.PRTF) : NULL;
		if (head) {
			size_t l1 = strlen(head->normal_form), l2 = strlen(noun->normal_form);
			char *lemma = malloc(l1 + l2 + 2);
			if (lemma) {
				memcpy(lemma, head->normal_form, l1);
				lemma[l1] = ' ';
				memcpy(lemma + l1 + 1, noun->normal_form, l2 + 1);
				r = rn_unit_by_lemma(lemma);
				free(lemma);
			} else
				nx_fail();
		}
	}
	rn_str_free(&low);
	return r;
}

/* paucal_accusative_case(value, case, animate=False) */
static int paucal_inan(const char *d, int gcase)
{
	if (gcase != rn_g.accs)
		return gcase;
	unsigned v10 = nx_dmod(d, 10), v100 = nx_dmod(d, 100);
	if (v10 < 2 || v10 > 4 || (v100 >= 12 && v100 <= 14))
		return gcase;
	return rn_g.nomn;
}

static int is_oblique(int c)
{
	return c == rn_g.datv || c == rn_g.ablt || c == rn_g.loct;
}

/* repl(match): 1 and the reading in o, or 0 (unchanged) */
static int range_repl(const rn_match *m, rn_str *o, const rn_unit **unit)
{
	const rn_re *p = rn_pat(RN_NUMERALS_CONSTANTS_NUMERIC_UNIT_RANGE_PATTERN);
	const uint32_t *T = m->s;
	size_t ln, rgn, un, st = m->start[0], en = m->end[0];
	const uint32_t *left = nx_grp(m, p, "left", &ln);
	const uint32_t *right = nx_grp(m, p, "right", &rgn);
	const uint32_t *uraw = nx_grp(m, p, "unit", &un);
	const rn_unit *u = range_unit(uraw, un);
	*unit = u;
	if (!u)
		return 0;
	int fcase = -1;
	size_t b = en + 40 < m->n ? en + 40 : m->n;
	rn_tok *R = NULL;
	long rcount = rn_simple_tokenize(T + en, b - en, &R);
	if (rcount < 0)
		nx_fail();
	for (long i = 0; i < rcount && i < 5; i++) {
		size_t x = 0, y = R[i].n;
		while (x < y && nx_inset(R[i].s[x], U".,!?;:()[]{}"))
			x++;
		while (y > x && nx_inset(R[i].s[y - 1], U".,!?;:()[]{}"))
			y--;
		if (x == y)
			continue;
		int stop = 0;
		for (size_t k = 0; k < R[i].n && !stop; k++)
			stop = nx_inset(R[i].s[k], U".!?;:");
		if (stop)
			break;
		nx_selq q = NX_SELQ;
		q.pos[0] = rn_g.NOUN;
		const cmorphy3_form *f = nx_select_w(R[i].s + x, y - x, &q);
		if (f && rn_reliable(f)) {
			int c = rn_has(f->tag, rn_g.loc2) ? rn_g.loct : rn_case(f->tag);
			if (c == rn_g.gent || is_oblique(c))
				fcase = c;
			break;
		}
		q.pos[0] = rn_g.VERB;
		q.pos[1] = rn_g.INFN;
		if (nx_select_w(R[i].s + x, y - x, &q))
			break;
	}
	free(R);
	int gcase = fcase >= 0 ? fcase : nx_case_around(T, st, st, st, left, ln, 40);
	char *ld = malloc(ln + 3), *rd = malloc(rgn + 3);
	int ok = ld && rd && nx_pyint(left, ln, ld, ln + 3) >= 0 &&
	         nx_pyint(right, rgn, rd, rgn + 3) >= 0;
	if (!ok) {
		/* int() cannot fail on \d+; out of memory */
		nx_fail();
		free(ld);
		free(rd);
		return 0;
	}
	int g = gram_of(u->gender);
	nx_numeral(o, left, ln, paucal_inan(ld, gcase), g, 0);
	nx_lit(o, U" — ");
	nx_numeral(o, right, rgn, paucal_inan(rd, gcase), g, 0);
	nx_putc(o, ' ');
	nx_unit_lemma(o, u, nx_ttags(rd, gcase), nx_nform(rd) == 'f');
	if (u->suffix) {
		nx_putc(o, ' ');
		nx_put8(o, u->suffix);
	}
	if (nx_grp(m, p, "udot", &(size_t){0}) && rn_keep_terminal_abbr_dot(T, m->n, en))
		nx_putc(o, '.');
	free(ld);
	free(rd);
	return 1;
}

static int noun_range_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = rn_pat(RN_NUMERALS_CARDINALS_NUMERIC_NOUN_RANGE_PATTERN);
	size_t nn, ln, rgn;
	const uint32_t *noun = nx_grp(m, p, "noun", &nn);
	nx_selq q = NX_SELQ;
	q.pos[0] = rn_g.NOUN;
	const cmorphy3_form *f = nx_select_w(noun, nn, &q);
	if (!f || !rn_reliable(f))
		return nx_whole(m, o);
	int c = rn_has(f->tag, rn_g.loc2) ? rn_g.loct : rn_case(f->tag);
	if (!is_oblique(c))
		return nx_whole(m, o);
	int g = rn_gender(f->tag);
	const uint32_t *l = nx_grp(m, p, "left", &ln), *r = nx_grp(m, p, "right", &rgn);
	nx_numeral(o, l, ln, c, g, 0);
	nx_lit(o, U" — ");
	nx_numeral(o, r, rgn, c, g, 0);
	nx_putg(o, m, p, "space");
	nx_put(o, noun, nn);
	return 0;
}

int rn_n_numeric_unit_ranges(rn_str *t)
{
	const rn_re *p = rn_pat(RN_NUMERALS_CONSTANTS_NUMERIC_UNIT_RANGE_PATTERN);
	const rn_re *sc = rn_pat(RN_NUMERALS_CARDINALS_SCALED_RANGE_UNIT_PATTERN);
	rn_str out = {0}, r = {0};
	size_t pos = 0, from = 0;
	rn_match m;
	for (;;) {
		int k = rn_search(p, t->s, t->n, from, &m);
		if (k < 0)
			nx_fail();
		if (k <= 0)
			break;
		size_t st = m.start[0], en = m.end[0];
		from = en > st ? en : st + 1;
		if (st < pos)
			continue;
		r.n = 0;
		const rn_unit *u;
		size_t end = en;
		if (!range_repl(&m, &r, &u))
			nx_put(&r, t->s + st, en - st);
		else if (is_multiplier(u)) {
			rn_match s;
			int ks = rn_match_at(sc, t->s, t->n, en, &s);
			if (ks < 0)
				nx_fail();
			size_t rn;
			const uint32_t *raw = ks > 0 ? nx_grp(&s, sc, "unit", &rn) : NULL;
			const rn_unit *su = raw ? rn_unit_resolve(raw, rn) : NULL;
			if (su) {
				while (r.n && r.s[r.n - 1] == '.')
					r.n--;
				nx_putc(&r, ' ');
				nx_render_unit(&r, su, nx_gs("gent plur"), 0);
				end = s.end[0];
			}
		}
		nx_put(&out, t->s + pos, st - pos);
		nx_put(&out, r.s, r.n);
		pos = end;
		if (from > t->n)
			break;
	}
	if (pos) {
		nx_put(&out, t->s + pos, t->n - pos);
		rn_str tmp = *t;
		*t = out;
		out = tmp;
	}
	rn_str_free(&out);
	rn_str_free(&r);
	nx_resub(rn_pat(RN_NUMERALS_CARDINALS_NUMERIC_NOUN_RANGE_PATTERN), t, noun_range_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}
