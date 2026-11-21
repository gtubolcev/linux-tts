/* shared helpers of the numerals passes: see rn-nx.h */
#include "rn-nx.h"

#include <limits.h>
#include <stdio.h>

/* ---- numbers ---- */

static void put_n2w(rn_str *out, int ordinal, const char *num, n2w_opts o, int *raised);

int nx_pyint(const uint32_t *s, size_t n, char *buf, size_t cap)
{
	size_t a = 0, b = n, k = 0;
	int neg = 0;
	while (a < b && rn_py_isspace(s[a]))
		a++;
	while (b > a && rn_py_isspace(s[b - 1]))
		b--;
	if (a < b && (s[a] == '+' || s[a] == '-'))
		neg = s[a++] == '-';
	if (a >= b || cap < 3)
		return -1;
	buf[k++] = '-';
	int prev_us = 1;   /* no leading underscore */
	for (size_t i = a; i < b; i++) {
		if (s[i] == '_') {
			if (prev_us)
				return -1;
			prev_us = 1;
			continue;
		}
		int d = rn_py_decimal(s[i]);
		if (d < 0)
			return -1;
		prev_us = 0;
		if (k == 1 && d == 0)   /* leading zeros */
			continue;
		if (k + 2 > cap)
			return -1;
		buf[k++] = (char)('0' + d);
	}
	if (prev_us)
		return -1;
	if (k == 1)
		buf[k++] = '0', neg = 0;
	buf[k] = 0;
	if (!neg) {
		memmove(buf, buf + 1, k);
		k--;
	}
	return (int)k;
}

int nx_pyint_long(const uint32_t *s, size_t n, long *v)
{
	char b[64];
	int l = nx_pyint(s, n, b, sizeof b);
	if (l < 0) {
		/* too long for b, or invalid: tell them apart by the digits */
		size_t a = 0;
		while (a < n && (rn_py_isspace(s[a]) || s[a] == '+' || s[a] == '-' || s[a] == '_' ||
		                 rn_py_decimal(s[a]) >= 0))
			a++;
		if (a < n || !n)
			return 0;
		char *big = malloc(n + 2);
		if (!big) {
			nx_fail();
			return 0;
		}
		l = nx_pyint(s, n, big, n + 2);
		*v = big[0] == '-' ? LONG_MIN : LONG_MAX;
		free(big);
		return l >= 0;
	}
	*v = strtol(b, NULL, 10);
	return 1;
}

int nx_n2w(rn_str *out, int ordinal, const char *num, n2w_opts o)
{
	int raised;
	put_n2w(out, ordinal, num, o, &raised);
	return raised ? -1 : 0;
}

static void put_n2w(rn_str *out, int ordinal, const char *num, n2w_opts o, int *raised)
{
	char buf[1024];
	o.fixes = rn_n2w_fixes;
	int len = ordinal ? n2w_ru_ordinal(num, o, buf, sizeof buf)
	                  : n2w_ru_cardinal(num, o, buf, sizeof buf);
	if (len < 0 || (size_t)len >= sizeof buf) {
		*raised = 1;
		return;
	}
	*raised = 0;
	nx_put8(out, buf);
}

static enum n2w_gender n2w_gender_of(int g)
{
	return g == rn_g.femn ? N2W_FEMN : g == rn_g.neut ? N2W_NEUT : N2W_MASC;
}

static int is_gender(int g)
{
	return g >= 0 && (g == rn_g.masc || g == rn_g.femn || g == rn_g.neut);
}

void nx_numeral_a(rn_str *out, const char *num, int gcase, int gender, int animate)
{
	n2w_opts o = N2W_DEFAULTS;
	o.gcase = (enum n2w_case)rn_n2w_case(rn_canon_case(gcase));
	if (gender >= 0 && gender == rn_g.plur)
		o.plural = 1;
	else if (is_gender(gender))
		o.gender = n2w_gender_of(gender);
	if (animate >= 0)
		o.animate = animate;
	int raised;
	put_n2w(out, 0, num, o, &raised);
	if (raised)   /* num2words(value) without the keywords raises as well */
		nx_put8(out, num);
}

void nx_numeral(rn_str *out, const uint32_t *s, size_t n, int gcase, int gender, int animate)
{
	char small[128], *num = small;
	if (n + 2 > sizeof small && !(num = malloc(n + 2))) {
		nx_fail();
		return;
	}
	if (nx_pyint(s, n, num, n + 2 > sizeof small ? n + 2 : sizeof small) < 0)
		nx_put(out, s, n);   /* ValueError: num_str */
	else {
		/* num2words raises for the long ones: then num_str, not str(int) */
		rn_str tmp = {0};
		n2w_opts o = N2W_DEFAULTS;
		o.gcase = (enum n2w_case)rn_n2w_case(rn_canon_case(gcase));
		if (gender >= 0 && gender == rn_g.plur)
			o.plural = 1;
		else if (is_gender(gender))
			o.gender = n2w_gender_of(gender);
		if (animate >= 0)
			o.animate = animate;
		int raised;
		put_n2w(&tmp, 0, num, o, &raised);
		if (raised)
			nx_put(out, s, n);
		else
			nx_put(out, tmp.s, tmp.n);
		rn_str_free(&tmp);
	}
	if (num != small)
		free(num);
}

void nx_ordinal(rn_str *out, const char *num, int gcase, int gender, int plural, int inanimate)
{
	int c = rn_canon_case(gcase);
	if (c == rn_g.accs && inanimate && (plural || gender == rn_g.masc || gender == rn_g.neut))
		c = rn_g.nomn;
	n2w_opts o = N2W_DEFAULTS;
	o.gcase = (enum n2w_case)rn_n2w_case(c);
	if (plural)
		o.plural = 1;
	else if (is_gender(gender))
		o.gender = n2w_gender_of(gender);
	int raised;
	put_n2w(out, 1, num, o, &raised);
	if (!raised)
		return;
	/* the parse/inflect fallback runs only where num2words raises with the keywords and
	 * not without them, which never happens for ints; num2words(number) raising gives
	 * str(number) */
	put_n2w(out, 1, num, N2W_DEFAULTS, &raised);
	if (raised)
		nx_put8(out, num);
}

void nx_ordinal_l(rn_str *out, long v, int gcase, int gender, int plural, int inanimate)
{
	char b[32];
	snprintf(b, sizeof b, "%ld", v);
	nx_ordinal(out, b, gcase, gender, plural, inanimate);
}

/* ---- morphology ---- */

int nx_gram(const char *name)
{
	return cmorphy3_grammeme(rn_morph, name);
}

cmorphy3_gset nx_gs(const char *names)
{
	cmorphy3_gset g = {0, 0};
	char w[32];
	while (*names) {
		while (*names == ' ')
			names++;
		size_t l = 0;
		while (names[l] && names[l] != ' ')
			l++;
		if (!l)
			break;
		if (l < sizeof w) {
			memcpy(w, names, l);
			w[l] = 0;
			int i = cmorphy3_grammeme(rn_morph, w);
			if (i >= 0)
				g = rn_gset_or(g, rn_gset1(i));
		}
		names += l;
	}
	return g;
}

static int gs_empty(cmorphy3_gset g)
{
	return !g.lo && !g.hi;
}

static int gs_count(cmorphy3_gset g)
{
	return __builtin_popcountll(g.lo) + __builtin_popcountll(g.hi);
}

static cmorphy3_gset gs_and(cmorphy3_gset a, cmorphy3_gset b)
{
	return (cmorphy3_gset){a.lo & b.lo, a.hi & b.hi};
}

/* normal_form.lower() in the list (the dictionary's normal forms are lower case) */
static int nf_in(const char *nf, const char *const *l)
{
	for (; *l; l++)
		if (!strcmp(nf, *l))
			return 1;
	return 0;
}

const cmorphy3_form *nx_select(const cmorphy3_form *f, int count, const nx_selq *q)
{
	cmorphy3_gset req = rn_canon(q->req), forb = rn_canon(q->forb), pref = rn_canon(q->pref);
	int tcase = q->gcase >= 0 ? rn_canon_case(q->gcase) : -1;
	const cmorphy3_form *best = NULL;
	int best_n = -1;
	for (int i = 0; i < count; i++) {
		uint16_t t = f[i].tag;
		cmorphy3_gset g = rn_canon(rn_gs(t));
		if (q->pos[0] >= 0) {
			int p = rn_pos(t), ok = 0;
			for (int k = 0; k < 4 && q->pos[k] >= 0; k++)
				if (p == q->pos[k])
					ok = 1;
			if (!ok)
				continue;
		}
		if (q->nforms && !nf_in(f[i].normal_form, q->nforms))
			continue;
		if (!cmorphy3_gset_subset(req, g))
			continue;
		if (!gs_empty(gs_and(forb, g)))
			continue;
		if (q->gender >= 0 && rn_gender(t) != q->gender)
			continue;
		if (q->number >= 0 && rn_number(t) != q->number)
			continue;
		if (tcase >= 0 && rn_canon_case(rn_case(t)) != tcase)
			continue;
		int pc = gs_count(gs_and(pref, g));
		if (pc > best_n) {
			best = &f[i];
			best_n = pc;
		}
	}
	return best;
}

const cmorphy3_form *nx_select_w(const uint32_t *s, size_t n, const nx_selq *q)
{
	const cmorphy3_form *f;
	int c = rn_mparse(s, n, &f);
	return nx_select(f, c, q);
}

const cmorphy3_form *nx_select8(const char *word, const nx_selq *q)
{
	const cmorphy3_form *f;
	int c = rn_mparse8(word, &f);
	return nx_select(f, c, q);
}

int nx_inflect(rn_str *out, const cmorphy3_form *f, cmorphy3_gset tags)
{
	cmorphy3_form r;
	if (!rn_minflect(f, tags, &r))
		return 0;
	nx_put8(out, r.word);
	return 1;
}

static int ends8(const char *w, size_t l, const char *suf)
{
	size_t k = strlen(suf);
	return k <= l && !memcmp(w + l - k, suf, k);
}

void nx_safe_inflect(rn_str *out, const cmorphy3_form *f, cmorphy3_gset tags,
                     const char *fallback)
{
	if (nx_inflect(out, f, tags))
		return;
	int pos = rn_pos(f->tag);
	if (cmorphy3_gset_has(tags, rn_g.gent) && cmorphy3_gset_has(tags, rn_g.plur) &&
	    (pos == rn_g.ADJF || pos == rn_g.PRTF)) {
		/* apply_genitive_plural_heuristic; parsed.word is lower case */
		static const char *const H[][2] = {{"ые", "ых"}, {"ие", "их"}, {"ый", "ого"},
		                                   {"ой", "ого"}, {"ий", "его"}, {"ее", "его"},
		                                   {"ая", "ой"},  {"яя", "ей"}};
		size_t l = strlen(f->word);
		for (size_t i = 0; i < sizeof H / sizeof *H; i++)
			if (ends8(f->word, l, H[i][0])) {
				size_t k = l - strlen(H[i][0]);
				char *b = malloc(k + 8);
				if (!b) {
					nx_fail();
					return;
				}
				memcpy(b, f->word, k);
				strcpy(b + k, H[i][1]);
				nx_put8(out, b);
				free(b);
				return;
			}
	}
	nx_put8(out, fallback ? fallback : f->word);
}

/* morphology.form(case, number) of the "case number word;..." paradigm: 1 and appended */
static int paradigm_form(rn_str *out, const char *par, const char *c, const char *num)
{
	if (!par)
		return 0;
	size_t cl = strlen(c), nl = strlen(num);
	while (*par) {
		const char *end = strchr(par, ';');
		size_t l = end ? (size_t)(end - par) : strlen(par);
		if (l > cl + nl + 2 && !memcmp(par, c, cl) && par[cl] == ' ' &&
		    !memcmp(par + cl + 1, num, nl) && par[cl + 1 + nl] == ' ') {
			size_t wl = l - cl - nl - 2;
			char *w = malloc(wl + 1);
			if (!w) {
				nx_fail();
				return 1;
			}
			memcpy(w, par + cl + nl + 2, wl);
			w[wl] = 0;
			nx_put8(out, w);
			free(w);
			return 1;
		}
		par += l;
		if (*par == ';')
			par++;
	}
	return 0;
}

static const cmorphy3_form *preferred_parse8(const char *w)
{
	const cmorphy3_form *f;
	int c = rn_mparse8(w, &f);
	for (int i = 0; i < c; i++)
		if (rn_has(f[i].tag, rn_g.NOUN))
			return &f[i];
	return c > 0 ? &f[0] : NULL;
}

static void unit_lemma(rn_str *out, const char *lemma, const char *gender, const rn_unit_morph *m,
                       cmorphy3_gset tags, int paucal)
{
	tags = rn_canon(tags);
	if (!strchr(lemma, ' ')) {
		static const char *const CASES[] = {"nomn", "gent", "datv", "accs", "ablt", "loct"};
		const char *tc = "nomn";
		/* next(g for g in target_tags if g in cases): the order of a Python set; callers
		 * pass one case */
		for (int i = 0; i < 6; i++)
			if (cmorphy3_gset_has(tags, nx_gram(CASES[i]))) {
				tc = CASES[i];
				break;
			}
		const char *tn = cmorphy3_gset_has(tags, rn_g.plur) ? "plur" : "sing";
		if (m->plural_only)
			tn = "plur";
		if (m->indeclinable) {
			nx_put8(out, lemma);
			return;
		}
		if (paradigm_form(out, m->paradigm, tc, tn))
			return;
		nx_selq q = NX_SELQ;
		q.pos[0] = rn_g.NOUN;
		const char *nf[2] = {m->expected_normal_form ? m->expected_normal_form : lemma, NULL};
		q.nforms = nf;
		if (m->forbidden)
			q.forb = nx_gs(m->forbidden);
		if (m->byte_family)
			q.forb = rn_gset_or(q.forb, nx_gs("Coun"));
		if (gender && (!strcmp(gender, "masc") || !strcmp(gender, "femn") ||
		               !strcmp(gender, "neut")))
			q.gender = nx_gram(gender);
		if (m->animacy)
			q.req = nx_gs(m->animacy);
		if (m->preferred)
			q.pref = nx_gs(m->preferred);
		const cmorphy3_form *sel = nx_select8(lemma, &q);
		if (!sel && m->seed) {
			q.pref = (cmorphy3_gset){0, 0};
			sel = nx_select8(m->seed, &q);
		}
		if (sel) {
			rn_str t = {0};
			nx_safe_inflect(&t, sel, tags, "");
			if (t.n) {
				nx_put(out, t.s, t.n);
				rn_str_free(&t);
				return;
			}
			rn_str_free(&t);
		}
		nx_put8(out, lemma);
		return;
	}
	const char *sp = strchr(lemma, ' ');
	if (strchr(sp + 1, ' ') || sp == lemma || !sp[1]) {
		/* lemma.split() of != 2 parts (the registry has no doubled spaces) */
		nx_put8(out, lemma);
		return;
	}
	size_t al = (size_t)(sp - lemma);
	char *adj = malloc(al + 1);
	if (!adj) {
		nx_fail();
		return;
	}
	memcpy(adj, lemma, al);
	adj[al] = 0;
	const char *noun = sp + 1;
	const cmorphy3_form *ap = preferred_parse8(adj);
	const cmorphy3_form *np = ap ? preferred_parse8(noun) : NULL;
	if (!ap || !np) {
		nx_put8(out, lemma);
		free(adj);
		return;
	}
	/* the noun parse may be overwritten by later parses: copy the strings needed */
	cmorphy3_form npc = *np;
	rn_str nform = {0};
	nx_safe_inflect(&nform, &npc, tags, noun);
	cmorphy3_gset keep = nx_gs("nomn gent datv accs ablt loct sing plur");
	cmorphy3_gset at = gs_and(tags, keep);
	cmorphy3_gset gs1 = nx_gs("gent sing");
	if (paucal && at.lo == gs1.lo && at.hi == gs1.hi) {
		at = rn_gender(npc.tag) == rn_g.femn ? nx_gs("plur nomn") : nx_gs("gent plur");
	} else if (!cmorphy3_gset_has(at, rn_g.plur)) {
		int g = rn_gender(npc.tag);
		if (g >= 0)
			at = rn_gset_or(at, rn_gset1(g));
	}
	nx_safe_inflect(out, ap, at, adj);
	nx_putc(out, ' ');
	nx_put(out, nform.s, nform.n);
	rn_str_free(&nform);
	free(adj);
}

void nx_unit_lemma(rn_str *out, const rn_unit *u, cmorphy3_gset tags, int paucal)
{
	unit_lemma(out, u->lemma, u->gender, &u->m, tags, paucal);
}

void nx_unit_lemma8(rn_str *out, const char *lemma, cmorphy3_gset tags, int paucal)
{
	rn_unit_morph m = {0};
	m.expected_normal_form = lemma;
	unit_lemma(out, lemma, "masc", &m, tags, paucal);
}

void nx_render_unit(rn_str *out, const rn_unit *u, cmorphy3_gset tags, int paucal)
{
	nx_unit_lemma(out, u, tags, paucal);
	if (u->suffix && *u->suffix) {
		nx_putc(out, ' ');
		nx_put8(out, u->suffix);
	}
}

int nx_has_pos(const uint32_t *s, size_t n, int pos)
{
	const cmorphy3_form *f;
	int c = rn_mparse(s, n, &f);
	for (int i = 0; i < c; i++)
		if (rn_pos(f[i].tag) == pos)
			return 1;
	return 0;
}

int nx_has_pos8(const char *s, int pos)
{
	const cmorphy3_form *f;
	int c = rn_mparse8(s, &f);
	for (int i = 0; i < c; i++)
		if (rn_pos(f[i].tag) == pos)
			return 1;
	return 0;
}

int nx_case_around(const uint32_t *t, size_t tn, size_t st, size_t en, const uint32_t *num,
                   size_t numn, size_t k)
{
	size_t a = nx_left(st, k), rn = en + k < tn ? k : tn - en;
	rn_tok *L = NULL, *R = NULL, *all = NULL;
	long nl = rn_simple_tokenize(t + a, st - a, &L);
	long nr = nl < 0 ? -1 : rn_simple_tokenize(t + en, rn, &R);
	int r = -1;
	if (nr >= 0 && (all = malloc((size_t)(nl + nr + 1) * sizeof *all))) {
		if (nl)
			memcpy(all, L, (size_t)nl * sizeof *all);
		all[nl] = (rn_tok){num, numn};
		if (nr)
			memcpy(all + nl + 1, R, (size_t)nr * sizeof *all);
		rn_ncase *c = rn_ncase_new(all, (size_t)(nl + nr + 1));
		if (c) {
			r = rn_ncase_get(c, (size_t)nl, SIZE_MAX, NULL);
			rn_ncase_free(c);
		}
	}
	free(L);
	free(R);
	free(all);
	if (r < 0) {
		nx_fail();
		return rn_g.nomn;
	}
	return r;
}

void nx_left_word(const uint32_t *t, size_t st, size_t k, rn_str *out)
{
	size_t a = nx_left(st, k);
	rn_tok *L = NULL;
	long nl = rn_simple_tokenize(t + a, st - a, &L);
	out->n = 0;
	if (nl < 0) {
		nx_fail();
		return;
	}
	for (long i = nl - 1; i >= 0; i--) {
		int alpha = 0;
		for (size_t j = 0; j < L[i].n; j++)
			if (rn_py_isalpha(L[i].s[j]))
				alpha = 1;
		if (alpha) {
			nx_lower(L[i].s, L[i].n, out);
			break;
		}
	}
	free(L);
}

unsigned nx_dmod(const char *d, unsigned m)
{
	unsigned long long r = 0;
	for (; *d; d++)
		if (*d >= '0' && *d <= '9')
			r = (r * 10 + (unsigned)(*d - '0')) % m;
	return (unsigned)r;
}

int nx_icase(const char *d, int gcase)
{
	if (gcase != rn_g.accs)
		return gcase;
	unsigned v100 = nx_dmod(d, 100);
	size_t l = strlen(d);
	unsigned th = 0;
	for (size_t i = 0; i + 3 < l; i++)
		if (d[i] >= '0' && d[i] <= '9')
			th = (th * 10 + (unsigned)(d[i] - '0')) % 1000;
	int fin = v100 % 10 == 1 && v100 != 11;
	int ths = th % 10 == 1 && th % 100 != 11;
	return fin || ths ? rn_g.accs : rn_g.nomn;
}

char nx_nform(const char *d)
{
	if (!strcmp(d, "0"))
		return 'm';
	unsigned v = nx_dmod(d, 100);
	if (v >= 11 && v <= 14)
		return 'm';
	if (v % 10 == 1)
		return 'o';
	if (v % 10 >= 2 && v % 10 <= 4)
		return 'f';
	return 'm';
}

cmorphy3_gset nx_ttags(const char *d, int gcase)
{
	char f = nx_nform(d);
	int sing = rn_g.sing, plur = rn_g.plur, gent = rn_g.gent;
	if (gcase == rn_g.nomn || gcase == rn_g.accs)
		return f == 'o'   ? rn_gset_or(rn_gset1(gcase), rn_gset1(sing))
		       : f == 'f' ? rn_gset_or(rn_gset1(gent), rn_gset1(sing))
		                  : rn_gset_or(rn_gset1(gent), rn_gset1(plur));
	if (nx_dge1000(d) && nx_dmod(d, 1000) == 0)
		return rn_gset_or(rn_gset1(gent), rn_gset1(plur));
	return rn_gset_or(rn_gset1(gcase), rn_gset1(f == 'o' ? sing : plur));
}

int nx_keep_dot_at(const uint32_t *s, size_t n, size_t i)
{
	int nl = 0;
	while (i < n && rn_py_isspace(s[i])) {
		nl |= s[i] == '\n';
		i++;
	}
	if (i >= n || nl)
		return 1;
	uint32_t c = s[i];
	if (nx_inset(c, U".!?…"))
		return 1;
	if (c == ',')
		return 0;
	return !(rn_py_islower(c) || rn_py_isdigit(c));
}
