/* numerals/ordinals.py, _hyphen.py, versions.py and normalize_square_unit_attributes of
 * unit_expressions.py. Each replacement sees the input of its own sub as the text (m->s). */
#include "rn-npass.h"
#include "rn-nx.h"
#include "rn-pat.h"
#include "rn-tables.h"

#define O(x) rn_pat(RN_NUMERALS_ORDINALS_##x)

/* ---- suffix sets ---- */

static const char32_t *const ORDINAL_SUFFIXES[] = {
	U"й",  U"ый", U"ий", U"ой", U"я",  U"ая", U"е",  U"ее", U"ое", U"ые", U"ых", U"ыми",
	U"го", U"ого", U"му", U"ому", U"м", U"ом", U"ем", U"ю",  U"ую", U"ей", U"ым", U"им", NULL};
static const char32_t *const CARDINAL_SUFFIXES[] = {
	U"ти", U"и", U"ми", U"х", U"мя", U"тью", U"ух", U"ех", U"ёх", U"ум", U"ём", U"у", U"ть", NULL};

/* s.lower().strip(".") (casefold is the same for the letters that matter) */
static void key_of(const uint32_t *s, size_t n, rn_str *k)
{
	nx_lower(s, n, k);
	size_t a = 0, b = k->n;
	while (a < b && k->s[a] == '.')
		a++;
	while (b > a && k->s[b - 1] == '.')
		b--;
	memmove(k->s, k->s + a, (b - a) * sizeof *k->s);
	k->n = b - a;
}

int rn_n_is_ordinal_suffix(const uint32_t *s, size_t n)
{
	return nx_oneof(s, n, ORDINAL_SUFFIXES);
}

int rn_n_is_cardinal_suffix(const uint32_t *s, size_t n)
{
	return nx_oneof(s, n, CARDINAL_SUFFIXES);
}

/* is_numeric_suffix */
int rn_n_is_numeric_suffix(const uint32_t *s, size_t n)
{
	rn_str k = {0};
	rn_py_casefold(s, n, &k);
	size_t a = 0, b = k.n;
	while (a < b && k.s[a] == '.')
		a++;
	while (b > a && k.s[b - 1] == '.')
		b--;
	int r = b > a ? rn_n_is_ordinal_suffix(k.s + a, b - a) ||
	                    rn_n_is_cardinal_suffix(k.s + a, b - a)
	              : rn_n_is_ordinal_suffix(U"", 0);
	rn_str_free(&k);
	return r;
}

/* is_safe_numeric_hyphen_unit */
int rn_n_safe_hyphen_unit(const uint32_t *s, size_t n)
{
	rn_str k = {0};
	key_of(s, n, &k);
	int r = 0;
	if (!rn_n_is_ordinal_suffix(k.s, k.n) && !rn_n_is_cardinal_suffix(k.s, k.n) &&
	    rn_unit_resolve(s, n))
		r = !(k.n == 1 && nx_inset(k.s[0], U"глмнрстфч"));
	rn_str_free(&k);
	return r;
}

/* ---- _hyphen.py ---- */

static int unit_hyphen_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = rn_pat(RN_NUMERALS_HYPHEN_NUMERIC_UNIT_HYPHEN_PATTERN);
	size_t un, dn;
	const uint32_t *u = nx_grp(m, p, "unit", &un);
	const uint32_t *d = nx_grp(m, p, "unit_dot", &dn);
	size_t st = m->start[0];
	if (un == 1 && st > 1 && rn_py_isspace(m->s[st - 1]) && rn_py_isalpha(m->s[st - 2]))
		return nx_whole(m, o);
	/* unit + unit_dot are adjacent in the text */
	if (!rn_n_safe_hyphen_unit(u, un + (d ? dn : 0)))
		return nx_whole(m, o);
	nx_putg(o, m, p, "num");
	nx_putc(o, ' ');
	nx_put(o, u, un + (d ? dn : 0));
	return 0;
}

int rn_n_numeric_unit_hyphen_links(rn_str *t)
{
	nx_resub(rn_pat(RN_NUMERALS_HYPHEN_NUMERIC_UNIT_HYPHEN_PATTERN), t, unit_hyphen_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

int rn_n_spaced_numeric_hyphen_words(rn_str *t)
{
	if (rn_resub_tmpl(rn_pat(RN_NUMERALS_HYPHEN_SPACED_NUMERIC_HYPHEN_WORD_PATTERN), t,
	                  (const uint32_t *)U"\1-\2") < 0)
		nx_fail();
	return rn_mx_failed ? -1 : 0;
}

/* ---- compound numeral prefixes ---- */

/* _compound_numeral_prefix of str(int) digits d */
static void cprefix_d(rn_str *o, const char *d)
{
	size_t l = strlen(d);
	if (!strcmp(d, "0")) {
		nx_lit(o, U"нуле");
		return;
	}
	static const struct {
		size_t k;
		const char32_t *stem;
	} SC[] = {{9, U"миллиардо"}, {6, U"миллионо"}, {3, U"тысяче"}};
	for (int i = 0; i < 3; i++) {
		if (l <= SC[i].k)
			continue;
		char *cnt = malloc(l + 1);
		if (!cnt) {
			nx_fail();
			return;
		}
		memcpy(cnt, d, l - SC[i].k);
		cnt[l - SC[i].k] = 0;
		const char *rem = d + l - SC[i].k;
		while (*rem == '0')
			rem++;
		if (strcmp(cnt, "1"))
			cprefix_d(o, cnt);
		nx_lit(o, SC[i].stem);
		if (*rem)
			cprefix_d(o, rem);
		free(cnt);
		return;
	}
	static const char32_t *const SMALL[] = {NULL, U"одно", U"двух", U"трёх", U"четырёх"};
	int v = atoi(d);
	if (v >= 1 && v <= 4) {
		nx_lit(o, SMALL[v]);
		return;
	}
	rn_str w = {0};
	nx_numeral_a(&w, d, rn_g.gent, -1, -1);
	if (v == 100) {
		nx_lit(o, U"сто");
		rn_str_free(&w);
		return;
	}
	/* split(), the fixes, "".join() */
	size_t i = 0, nw = 0;
	while (i < w.n) {
		while (i < w.n && rn_py_isspace(w.s[i]))
			i++;
		size_t a = i;
		while (i < w.n && !rn_py_isspace(w.s[i]))
			i++;
		if (a == i)
			break;
		const uint32_t *ws = w.s + a;
		size_t wl = i - a;
		size_t j = i;
		while (j < w.n && rn_py_isspace(w.s[j]))
			j++;
		int last = j >= w.n;
		if (nx_eq(ws, wl, U"девяноста"))
			nx_lit(o, U"девяносто");
		else if (nw == 0 && v > 100 && v < 200 && nx_eq(ws, wl, U"ста"))
			nx_lit(o, U"сто");
		else if (last && v % 10 == 1 && v % 100 != 11 && nx_eq(ws, wl, U"одного"))
			nx_lit(o, U"одно");
		else
			nx_put(o, ws, wl);
		nw++;
	}
	rn_str_free(&w);
}

/* _compound_numeral_prefix(num_str) */
static void cprefix(rn_str *o, const uint32_t *s, size_t n)
{
	char small[64], *d = small;
	if (n + 2 > sizeof small && !(d = malloc(n + 2))) {
		nx_fail();
		return;
	}
	if (nx_pyint(s, n, d, n + 2 > sizeof small ? n + 2 : sizeof small) < 0 || d[0] == '-')
		nx_put(o, s, n);   /* ValueError (no negatives reach here) */
	else
		cprefix_d(o, d);
	if (d != small)
		free(d);
}

/* _join_compound_numeral(prefix, word) */
static void join_compound(rn_str *o, const uint32_t *p, size_t pn, const uint32_t *w, size_t wn)
{
	rn_str c = {0};
	nx_put(&c, p, pn);
	if (rn_py_lower(w, wn, &c) < 0)
		nx_fail();
	if (rn_py_str_isupper(w, wn)) {
		if (rn_py_upper(c.s, c.n, o) < 0)
			nx_fail();
	} else if (wn && rn_py_isupper(w[0])) {
		if (rn_py_capitalize(c.s, c.n, o) < 0)
			nx_fail();
	} else
		nx_put(o, c.s, c.n);
	rn_str_free(&c);
}

/* _modern_spelling(word_lower) */
static void modern(const uint32_t *s, size_t n, rn_str *o)
{
	o->n = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		nx_putc(o, c == U'і' || c == U'ѵ' ? U'и' : c == U'ѣ' ? U'е' : c == U'ѳ' ? U'ф' : c);
	}
	if (o->n && o->s[o->n - 1] == U'ъ')
		o->n--;
	if (nx_ends(o->s, o->n, U"аго")) {
		o->n -= 3;
		nx_lit(o, U"ого");
	} else if (nx_ends(o->s, o->n, U"яго")) {
		o->n -= 3;
		nx_lit(o, U"его");
	} else if (nx_ends(o->s, o->n, U"ыя")) {
		o->n -= 2;
		nx_lit(o, U"ые");
	} else if (nx_ends(o->s, o->n, U"ия") && o->n > 4) {
		o->n -= 2;
		nx_lit(o, U"ие");
	}
}

static int is_adj(const rn_str *sp)
{
	return nx_has_pos(sp->s, sp->n, rn_g.ADJF);
}

/* ---- ordinals.py ---- */

/* render_ordinal(int(s), ...) */
static void ordinal_of(rn_str *o, const uint32_t *s, size_t n, int c, int g, int plural,
                       int inanimate)
{
	char small[96], *b = small;
	if (n + 2 > sizeof small && !(b = malloc(n + 2))) {
		nx_fail();
		return;
	}
	if (nx_pyint(s, n, b, n + 2 > sizeof small ? n + 2 : sizeof small) < 0)
		nx_put(o, s, n);
	else
		nx_ordinal(o, b, c, g, plural, inanimate);
	if (b != small)
		free(b);
}

static int edition_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *s = nx_grp(m, O(EDITION_ORDINAL_PATTERN), "num", &n);
	nx_lit(o, U"издание ");
	ordinal_of(o, s, n, rn_g.nomn, rn_g.neut, 0, 0);
	return 0;
}

int rn_n_edition_ordinals(rn_str *t)
{
	nx_resub(O(EDITION_ORDINAL_PATTERN), t, edition_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

static int adverb_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *s = nx_grp(m, O(PREFIXED_ORDINAL_ADVERB_PATTERN), "num", &n);
	nx_putg(o, m, O(PREFIXED_ORDINAL_ADVERB_PATTERN), "prefix");
	nx_putc(o, '-');
	ordinal_of(o, s, n, rn_g.gent, -1, 1, 1);
	return 0;
}

int rn_n_prefixed_ordinal_adverbs(rn_str *t)
{
	nx_resub(O(PREFIXED_ORDINAL_ADVERB_PATTERN), t, adverb_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

static const cmorphy3_form *first_pos(const uint32_t *s, size_t n, int pos)
{
	const cmorphy3_form *f;
	int c = rn_mparse(s, n, &f);
	for (int i = 0; i < c; i++)
		if (rn_pos(f[i].tag) == pos)
			return &f[i];
	return NULL;
}

static const cmorphy3_form *first_pos8(const char *s, int pos)
{
	const cmorphy3_form *f;
	int c = rn_mparse8(s, &f);
	for (int i = 0; i < c; i++)
		if (rn_pos(f[i].tag) == pos)
			return &f[i];
	return NULL;
}

static int thousands_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(THOUSANDS_ABBREVIATED_ATTRIBUTE_PATTERN);
	size_t nn, numn;
	const uint32_t *noun = nx_grp(m, p, "noun", &nn);
	const uint32_t *num = nx_grp(m, p, "num", &numn);
	const cmorphy3_form *np = first_pos(noun, nn, rn_g.NOUN);
	if (!np)
		return nx_whole(m, o);
	uint16_t nt = np->tag;
	const cmorphy3_form *ap = first_pos8("тысячный", rn_g.ADJF);
	if (!ap)
		return nx_whole(m, o);
	cmorphy3_gset tags = rn_gset1(rn_canon_case(rn_case(nt)));
	if (rn_has(nt, rn_g.plur))
		tags = rn_gset_or(tags, rn_gset1(rn_g.plur));
	else {
		tags = rn_gset_or(tags, rn_gset1(rn_g.sing));
		if (rn_gender(nt) >= 0)
			tags = rn_gset_or(tags, rn_gset1(rn_gender(nt)));
	}
	rn_str w = {0}, pre = {0};
	if (!nx_inflect(&w, ap, tags)) {
		rn_str_free(&w);
		return nx_whole(m, o);
	}
	cprefix(&pre, num, numn);
	join_compound(o, pre.s, pre.n, w.s, w.n);
	nx_putc(o, ' ');
	nx_put(o, noun, nn);
	rn_str_free(&w);
	rn_str_free(&pre);
	return 0;
}

int rn_n_thousands_abbreviated_attributes(rn_str *t)
{
	nx_resub(O(THOUSANDS_ABBREVIATED_ATTRIBUTE_PATTERN), t, thousands_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

static int last_digit(const uint32_t *s, size_t n, int k)
{
	/* int(s) % 10**k for k <= 2, of \d+ digits */
	int v = 0, mul = 1;
	for (size_t i = n; i > 0 && k > 0; i--, k--) {
		v += rn_py_decimal(s[i - 1]) * mul;
		mul *= 10;
	}
	return v;
}

static int compound_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(COMPOUND_ADJECTIVE_PATTERN);
	size_t an, nn;
	const uint32_t *adj = nx_grp(m, p, "adj", &an);
	const uint32_t *num = nx_grp(m, p, "num", &nn);
	const cmorphy3_form *f;
	int c = rn_mparse(adj, an, &f), any = 0, plur = 0;
	for (int i = 0; i < c; i++)
		if (rn_pos(f[i].tag) == rn_g.ADJF) {
			any = 1;
			if (rn_has(f[i].tag, rn_g.plur))
				plur = 1;
		}
	if (!any)
		return nx_whole(m, o);
	int v100 = last_digit(num, nn, 2);
	if ((v100 % 10 == 1 && v100 != 11) || plur)
		return nx_whole(m, o);
	rn_str pre = {0};
	cprefix(&pre, num, nn);
	join_compound(o, pre.s, pre.n, adj, an);
	rn_str_free(&pre);
	return 0;
}

static int percent_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(PERCENT_COMPOUND_ADJECTIVE_PATTERN);
	size_t sn, nn;
	const uint32_t *suf = nx_grp(m, p, "suffix", &sn);
	const uint32_t *num = nx_grp(m, p, "num", &nn);
	rn_str pre = {0}, w = {0}, word = {0};
	cprefix(&pre, num, nn);
	nx_lit(&w, U"процент");
	if (rn_py_lower(suf, sn, &w) < 0)
		nx_fail();
	if (rn_py_str_isupper(suf, sn))
		rn_py_upper(w.s, w.n, &word);
	else if (sn && rn_py_isupper(suf[0]))
		rn_py_capitalize(w.s, w.n, &word);
	else
		nx_put(&word, w.s, w.n);
	join_compound(o, pre.s, pre.n, word.s, word.n);
	rn_str_free(&pre);
	rn_str_free(&w);
	rn_str_free(&word);
	return 0;
}

int rn_n_compound_numeric_adjectives(rn_str *t)
{
	nx_resub(O(PERCENT_COMPOUND_ADJECTIVE_PATTERN), t, percent_repl, NULL);
	nx_resub(O(COMPOUND_ADJECTIVE_PATTERN), t, compound_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

/* ---- headings ---- */

static const char32_t *const AMBIGUOUS_HEADS[] = {U"главы", U"части", U"раздела", U"тома",
                                                  U"книги", U"квартала", NULL};

/* HEADING_CONTEXT_CASES.get(word): a grammeme or -1 */
static int heading_context_case(const rn_str *w)
{
	static const struct {
		const char32_t *w;
		const char *c;
	} T[] = {
		{U"в", "loct"},      {U"во", "loct"},     {U"о", "loct"},     {U"об", "loct"},
		{U"обо", "loct"},    {U"при", "loct"},    {U"к", "datv"},     {U"ко", "datv"},
		{U"по", "datv"},     {U"с", "gent"},      {U"со", "gent"},    {U"из", "gent"},
		{U"до", "gent"},     {U"от", "gent"},     {U"у", "gent"},     {U"без", "gent"},
		{U"после", "gent"},  {U"для", "gent"},    {U"около", "gent"}, {U"возле", "gent"},
		{U"вокруг", "gent"}, {U"кроме", "gent"},  {U"начало", "gent"}, {U"конец", "gent"},
		{U"середина", "gent"}, {U"см", "accs"},   {U"смотри", "accs"}, {U"на", "accs"},
		{U"про", "accs"},
	};
	for (size_t i = 0; i < sizeof T / sizeof *T; i++)
		if (nx_eq(w->s, w->n, T[i].w))
			return nx_gram(T[i].c);
	return -1;
}

/* the NOUN parses of head.lower(), the inanimate ones if any: up to cap in out */
static int head_candidates(const uint32_t *h, size_t hn, const cmorphy3_form **out, int cap)
{
	const cmorphy3_form *f;
	int c = rn_mparse(h, hn, &f), k = 0, inan = 0;
	for (int i = 0; i < c; i++)
		if (rn_pos(f[i].tag) == rn_g.NOUN && rn_has(f[i].tag, rn_g.inan))
			inan = 1;
	for (int i = 0; i < c && k < cap; i++)
		if (rn_pos(f[i].tag) == rn_g.NOUN && (!inan || rn_has(f[i].tag, rn_g.inan)))
			out[k++] = &f[i];
	return k;
}

/* _heading_gender */
static int heading_gender(const uint32_t *h, size_t hn)
{
	const cmorphy3_form *c[32];
	int k = head_candidates(h, hn, c, 32);
	for (int i = 0; i < k; i++)
		if (rn_gender(c[i]->tag) >= 0)
			return rn_gender(c[i]->tag);
	return rn_g.masc;
}

/* _heading_parse */
static const cmorphy3_form *heading_parse(const uint32_t *t, size_t st, const uint32_t *h,
                                          size_t hn)
{
	const cmorphy3_form *c[32];
	int k = head_candidates(h, hn, c, 32);
	if (!k)
		return NULL;
	rn_str lw = {0};
	nx_lower(h, hn, &lw);
	int amb = nx_oneof(lw.s, lw.n, AMBIGUOUS_HEADS);
	if (!amb) {
		rn_str_free(&lw);
		for (int i = 0; i < k; i++)
			if (rn_has(c[i]->tag, rn_g.sing) && rn_has(c[i]->tag, rn_g.nomn))
				return c[i];
		return c[0];
	}
	nx_left_word(t, st, 40, &lw);
	int tc = heading_context_case(&lw);
	rn_str_free(&lw);
	if (tc < 0)
		return NULL;
	for (int i = 0; i < k; i++)
		if (rn_canon_case(rn_case(c[i]->tag)) == tc && rn_has(c[i]->tag, rn_g.sing))
			return c[i];
	for (int i = 0; i < k; i++)
		if (rn_canon_case(rn_case(c[i]->tag)) == tc)
			return c[i];
	return NULL;
}

/* _pick_range_preposition(first) + " " */
static void range_prep(rn_str *o, const rn_str *first)
{
	int so = 0;
	if (first->n >= 2) {
		rn_str h = {0};
		nx_lower(first->s, 2, &h);
		if (h.n >= 2 && nx_inset(h.s[0], U"вфсзшж")) {
			rn_str u = {0};
			rn_py_upper(h.s + 1, 1, &u);
			so = !(u.n == 1 && nx_inset(u.s[0], U"АЕЁИОУЫЭЮЯ"));
			rn_str_free(&u);
		}
		rn_str_free(&h);
	}
	nx_lit(o, so ? U"со " : U"с ");
}

static void render_range(rn_str *o, const uint32_t *h, size_t hn, const cmorphy3_form *np,
                         const uint32_t *l, size_t ln, const uint32_t *r, size_t rn)
{
	int g = np && rn_gender(np->tag) >= 0 ? rn_gender(np->tag) : heading_gender(h, hn);
	rn_str lo = {0};
	ordinal_of(&lo, l, ln, rn_g.gent, g, 0, 0);
	int rc = g == rn_g.femn ? rn_g.accs : rn_g.nomn;
	int done = 0;
	if (np) {
		int tc = rn_case(np->tag) >= 0 ? rn_case(np->tag) : rn_g.nomn;
		rn_str ph = {0};
		if (nx_inflect(&ph, np, rn_gset_or(rn_gset1(tc), rn_gset1(rn_g.plur)))) {
			if (hn && rn_py_isupper(h[0]))
				rn_py_capitalize(ph.s, ph.n, o);
			else
				nx_put(o, ph.s, ph.n);
			done = 1;
		}
		rn_str_free(&ph);
	}
	if (!done)
		nx_put(o, h, hn);
	nx_putc(o, ' ');
	range_prep(o, &lo);
	nx_put(o, lo.s, lo.n);
	nx_lit(o, U" по ");
	ordinal_of(o, r, rn, rc, g, 0, 0);
	rn_str_free(&lo);
}

static int hrange_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(HEADING_RANGE_PATTERN);
	size_t hn, ln, rn;
	const uint32_t *h = nx_grp(m, p, "head", &hn);
	const uint32_t *l = nx_grp(m, p, "left", &ln);
	const uint32_t *r = nx_grp(m, p, "right", &rn);
	render_range(o, h, hn, heading_parse(m->s, m->start[0], h, hn), l, ln, r, rn);
	return 0;
}

/* the first parse of "глава" with NOUN (and nomn, inan if strict) */
static const cmorphy3_form *glava(int strict)
{
	const cmorphy3_form *f;
	int c = rn_mparse8("глава", &f);
	for (int i = 0; i < c; i++)
		if (rn_pos(f[i].tag) == rn_g.NOUN &&
		    (!strict || (rn_has(f[i].tag, rn_g.nomn) && rn_has(f[i].tag, rn_g.inan))))
			return &f[i];
	return NULL;
}

static int abbr_range_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(ABBREVIATED_HEADING_RANGE_PATTERN);
	rn_str lw = {0};
	nx_left_word(m->s, m->start[0], 40, &lw);
	int tc = heading_context_case(&lw);
	rn_str_free(&lw);
	if (tc < 0)
		tc = rn_g.nomn;
	const cmorphy3_form *np = glava(1);
	rn_str noun = {0};
	if (!np || !nx_inflect(&noun, np, rn_gset_or(rn_gset1(tc), rn_gset1(rn_g.plur)))) {
		rn_str_free(&noun);
		return nx_whole(m, o);
	}
	size_t ln, rn;
	const uint32_t *l = nx_grp(m, p, "left", &ln);
	const uint32_t *r = nx_grp(m, p, "right", &rn);
	rn_str lo = {0};
	ordinal_of(&lo, l, ln, rn_g.gent, rn_g.femn, 0, 0);
	nx_put(o, noun.s, noun.n);
	nx_putc(o, ' ');
	range_prep(o, &lo);
	nx_put(o, lo.s, lo.n);
	nx_lit(o, U" по ");
	ordinal_of(o, r, rn, rn_g.accs, rn_g.femn, 0, 0);
	rn_str_free(&lo);
	rn_str_free(&noun);
	return 0;
}

int rn_n_heading_ranges(rn_str *t)
{
	nx_resub(O(ABBREVIATED_HEADING_RANGE_PATTERN), t, abbr_range_repl, NULL);
	nx_resub(O(HEADING_RANGE_PATTERN), t, hrange_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

static int abbr_heading_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(ABBREVIATED_HEADING_PATTERN);
	rn_str lw = {0};
	nx_left_word(m->s, m->start[0], 40, &lw);
	int tc = heading_context_case(&lw);
	rn_str_free(&lw);
	if (tc < 0)
		tc = rn_g.nomn;
	const cmorphy3_form *np = glava(0);
	rn_str noun = {0};
	if (!np || !nx_inflect(&noun, np, rn_gset_or(rn_gset1(tc), rn_gset1(rn_g.sing)))) {
		rn_str_free(&noun);
		return nx_whole(m, o);
	}
	size_t ln, nn;
	const uint32_t *lead = nx_grp(m, p, "lead", &ln);
	const uint32_t *num = lead ? lead : nx_grp(m, p, "number", &nn);
	if (lead)
		nn = ln;
	if (lead) {
		ordinal_of(o, num, nn, tc, rn_g.femn, 0, 0);
		nx_putc(o, ' ');
		nx_put(o, noun.s, noun.n);
	} else {
		nx_put(o, noun.s, noun.n);
		nx_putc(o, ' ');
		ordinal_of(o, num, nn, tc, rn_g.femn, 0, 0);
	}
	rn_str_free(&noun);
	return 0;
}

static int heading_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(HEADING_SINGLE_PATTERN);
	size_t hn, nn;
	const uint32_t *h = nx_grp(m, p, "head", &hn);
	const uint32_t *num = nx_grp(m, p, "number", &nn);
	const cmorphy3_form *np = heading_parse(m->s, m->start[0], h, hn);
	if (!np)
		return nx_whole(m, o);
	int g = rn_gender(np->tag) >= 0 ? rn_gender(np->tag) : rn_g.masc;
	int c = rn_case(np->tag) >= 0 ? rn_case(np->tag) : rn_g.nomn;
	nx_put(o, h, hn);
	nx_putc(o, ' ');
	ordinal_of(o, num, nn, c, g, 0, 0);
	return 0;
}

int rn_n_heading_numbers(rn_str *t)
{
	nx_resub(O(ABBREVIATED_HEADING_PATTERN), t, abbr_heading_repl, NULL);
	nx_resub(O(HEADING_SINGLE_PATTERN), t, heading_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

/* ---- hyphenated words ---- */

/* _strip_duplicated_infix: 1 and *skip = len(infix), or 0 */
static int strip_infix(const rn_str *prefix, const uint32_t *w, size_t wn, size_t *skip)
{
	static const char32_t *const INF[] = {U"тью", U"ти", U"ми", U"мя", U"ух", U"ех", U"ёх", U"х"};
	rn_str wl = {0}, rl = {0}, sp = {0};
	nx_lower(w, wn, &wl);
	int r = 0;
	for (size_t i = 0; i < sizeof INF / sizeof *INF && !r; i++) {
		size_t il = 0;
		while (INF[i][il])
			il++;
		if (!nx_starts(wl.s, wl.n, INF[i]) || !nx_ends(prefix->s, prefix->n, INF[i]))
			continue;
		nx_lower(w + il, wn - il, &rl);
		modern(rl.s, rl.n, &sp);
		if (sp.n >= 4 && rn_word_is_known(sp.s, sp.n) && is_adj(&sp)) {
			*skip = il;
			r = 1;
		}
	}
	rn_str_free(&wl);
	rn_str_free(&rl);
	rn_str_free(&sp);
	return r;
}

static int range_adj_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = O(RANGE_COMPOUND_ADJECTIVE_PATTERN);
	size_t wn, ln, rn;
	const uint32_t *w = nx_grp(m, p, "word", &wn);
	const uint32_t *l = nx_grp(m, p, "left", &ln);
	const uint32_t *r = nx_grp(m, p, "right", &rn);
	rn_str wl = {0}, sp = {0};
	nx_lower(w, wn, &wl);
	modern(wl.s, wl.n, &sp);
	int adj = is_adj(&sp);
	rn_str_free(&wl);
	rn_str_free(&sp);
	if (!adj)
		return nx_whole(m, o);
	rn_str rp = {0};
	cprefix(o, l, ln);
	nx_putc(o, '-');
	cprefix(&rp, r, rn);
	join_compound(o, rp.s, rp.n, w, wn);
	rn_str_free(&rp);
	return 0;
}

static const char32_t *const ADJ_ENDINGS[] = {
	U"дневный", U"часовой", U"минутный", U"летний", U"этажный", U"тонный",
	U"процентный", U"кратный", U"кратного", U"кратном", U"кратных", NULL};

static int hyphen_word_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = rn_pat(RN_NUMERALS_CONSTANTS_HYPHENATED_WORD_PATTERN);
	size_t nn, in, wn;
	const uint32_t *num = nx_grp(m, p, "num", &nn);
	const uint32_t *infix = nx_grp(m, p, "infix", &in);
	const uint32_t *w = nx_grp(m, p, "word", &wn);
	if (!infix && (rn_n_is_numeric_suffix(w, wn) || rn_n_safe_hyphen_unit(w, wn)))
		return nx_whole(m, o);
	rn_str numstr = {0}, word = {0}, wl = {0}, pre = {0}, sp = {0};
	nx_put(&numstr, num, nn);
	nx_put(&word, w, wn);
	nx_lower(w, wn, &wl);
	/* value >= 1000, value % 1000 == 0 and the тысячный tail */
	char small[64], *d = small;
	if (nn + 2 > sizeof small && !(d = malloc(nn + 2))) {
		nx_fail();
		return nx_whole(m, o);
	}
	int dl = nx_pyint(num, nn, d, nn + 2 > sizeof small ? nn + 2 : sizeof small);
	rn_match fm;
	if (dl >= 4 && !strcmp(d + dl - 3, "000") &&
	    rn_fullmatch(O(THOUSANDS_ADJECTIVE_SUFFIX_PATTERN), wl.s, wl.n, 0, &fm) == 1) {
		d[dl - 3] = 0;
		numstr.n = 0;
		nx_put8(&numstr, d);
		word.n = 0;
		nx_lit(&word, U"тысяч");
		nx_put(&word, w, wn);
		nx_lower(word.s, word.n, &wl);
	}
	if (d != small)
		free(d);
	cprefix(&pre, numstr.s, numstr.n);
	size_t skip;
	if (strip_infix(&pre, word.s, word.n, &skip)) {
		memmove(word.s, word.s + skip, (word.n - skip) * sizeof *word.s);
		word.n -= skip;
		nx_lower(word.s, word.n, &wl);
	}
	modern(wl.s, wl.n, &sp);
	const cmorphy3_form *f;
	int c = rn_mparse(sp.s, sp.n, &f), adj = 0, noun = 0;
	for (int i = 0; i < c; i++) {
		if (rn_pos(f[i].tag) == rn_g.ADJF)
			adj = 1;
		if (rn_has(f[i].tag, rn_g.NOUN))
			noun = 1;
	}
	for (int i = 0; !adj && ADJ_ENDINGS[i]; i++)
		if (nx_ends(sp.s, sp.n, ADJ_ENDINGS[i]))
			adj = 1;
	if (adj)
		join_compound(o, pre.s, pre.n, word.s, word.n);
	else if (infix || !noun)
		nx_whole(m, o);
	else {
		int cs = nx_case_around(m->s, m->n, m->start[0], m->end[0], numstr.s, numstr.n, 60);
		nx_numeral(o, numstr.s, numstr.n, cs, -1, -1);
		nx_putc(o, ' ');
		nx_put(o, word.s, word.n);
	}
	rn_str_free(&numstr);
	rn_str_free(&word);
	rn_str_free(&wl);
	rn_str_free(&pre);
	rn_str_free(&sp);
	return 0;
}

int rn_n_hyphenated_words(rn_str *t)
{
	nx_resub(O(RANGE_COMPOUND_ADJECTIVE_PATTERN), t, range_adj_repl, NULL);
	nx_resub(rn_pat(RN_NUMERALS_CONSTANTS_HYPHENATED_WORD_PATTERN), t, hyphen_word_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

/* ---- unit_expressions.py: square units ---- */

static int square_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = rn_pat(RN_NUMERALS_UNIT_EXPRESSIONS_SQUARE_UNIT_ATTRIBUTE_PATTERN);
	size_t nn, an;
	const uint32_t *noun = nx_grp(m, p, "noun", &nn);
	const uint32_t *abbr = nx_grp(m, p, "abbr", &an);
	const cmorphy3_form *np = first_pos(noun, nn, rn_g.NOUN);
	if (!np || !rn_has(np->tag, rn_g.inan))
		return nx_whole(m, o);
	uint16_t nt = np->tag;
	int c = rn_case(nt), num = rn_number(nt);
	if (c < 0 || num < 0)
		return nx_whole(m, o);
	nx_selq q = NX_SELQ;
	q.pos[0] = rn_g.ADJF;
	const cmorphy3_form *ap =
		nx_select8(nx_eq(abbr, an, U"кв") ? "квадратный" : "кубический", &q);
	if (!ap)
		return nx_whole(m, o);
	cmorphy3_gset tags = rn_gset_or(rn_gset1(c), rn_gset1(num));
	if (num == rn_g.sing) {
		if (rn_gender(nt) < 0)
			return nx_whole(m, o);
		tags = rn_gset_or(tags, rn_gset1(rn_gender(nt)));
	}
	rn_str w = {0};
	if (!nx_inflect(&w, ap, tags)) {
		rn_str_free(&w);
		return nx_whole(m, o);
	}
	nx_putg(o, m, p, "space");
	nx_put(o, w.s, w.n);
	nx_putc(o, ' ');
	nx_put(o, noun, nn);
	rn_str_free(&w);
	return 0;
}

int rn_n_square_unit_attributes(rn_str *t)
{
	if (rn_py_find(t->s, t->n, RN_U("кв"), 2, 0) < 0 &&
	    rn_py_find(t->s, t->n, RN_U("куб"), 3, 0) < 0)
		return 0;
	nx_resub(rn_pat(RN_NUMERALS_UNIT_EXPRESSIONS_SQUARE_UNIT_ATTRIBUTE_PATTERN), t, square_repl,
	         NULL);
	return rn_mx_failed ? -1 : 0;
}

/* ---- versions.py ---- */

static void component(rn_str *o, const uint32_t *s, size_t n)
{
	size_t z = 0;
	while (z < n && s[z] == '0')
		z++;
	for (size_t i = 0; i < z; i++) {
		if (i)
			nx_putc(o, ' ');
		nx_lit(o, U"ноль");
	}
	if (z < n) {
		if (z)
			nx_putc(o, ' ');
		nx_numeral(o, s + z, n - z, rn_g.nomn, -1, -1);
	}
}

static int version_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = rn_pat(RN_NUMERALS_VERSIONS_VERSION_PATTERN);
	size_t nmn, vn;
	const uint32_t *name = nx_grp(m, p, "name", &nmn);
	const uint32_t *v = nx_grp(m, p, "version", &vn);
	size_t parts = 1;
	for (size_t i = 0; i < vn; i++)
		parts += v[i] == '.';
	if (parts < 3) {
		int keep = !name;
		if (!keep) {
			rn_match um;
			int r = rn_match_at(rn_pat(RN_NUMERALS_VERSIONS_TRAILING_UNIT_PATTERN), m->s, m->n,
			                    m->end[0], &um);
			if (r < 0)
				nx_fail();
			if (r > 0) {
				size_t un;
				const uint32_t *u =
					nx_grp(&um, rn_pat(RN_NUMERALS_VERSIONS_TRAILING_UNIT_PATTERN), "unit", &un);
				keep = rn_unit_resolve(u, un) != NULL;
			}
		}
		if (keep)
			return nx_whole(m, o);
	}
	if (name) {
		nx_put(o, name, nmn);
		nx_putc(o, ' ');
	}
	size_t a = 0;
	for (size_t i = 0; i <= vn; i++)
		if (i == vn || v[i] == '.') {
			if (a)
				nx_lit(o, U" точка ");
			component(o, v + a, i - a);
			a = i + 1;
		}
	return 0;
}

int rn_n_versions(rn_str *t)
{
	nx_resub(rn_pat(RN_NUMERALS_VERSIONS_VERSION_PATTERN), t, version_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}
