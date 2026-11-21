/* numerals/cardinals.py normalize_cardinal_numerals: a number token and its neighbours
 * ("5 книг", "2 основные задачи", "5 кг", "в 9 часу", "главе восьмой и 9") */
#include <stdio.h>
#include "rn-npass.h"
#include "rn-nx.h"
#include "rn-pat.h"
#include "rn-ph.h"
#include "rn-tables.h"

#define PUNCT_STRIP U".,:;!\"«»()[]{}"
#define MAX_UNIT_TOKENS 4
#define MAX_CLOCK_HOUR 24

static const char *const MULTIPLIERS[] = {"тысяча", "миллион", "миллиард", "триллион", NULL};
static const char *const DAY_PARTS[] = {"утро", "день", "вечер", "ночь", "полдень", "полночь",
                                        NULL};
static const char *const NUMBER_LABELS[] = {
    "номер",    "пункт",  "статья",   "параграф", "глава",   "раздел", "том",
    "страница", "приложение", "рейс", "дом",      "квартира", "комната", "палата",
    "вагон",    NULL};

static int in8(const char *s, const char *const *l)
{
	for (; *l; l++)
		if (!strcmp(s, *l))
			return 1;
	return 0;
}

static int gram_of(const char *name)
{
	return name ? nx_gram(name) : -1;
}

/* ---- token helpers ---- */

typedef struct {
	const rn_tok *t;
	size_t n;
} toks;

static int tok_is(const rn_tok *t, const char32_t *lit)
{
	return nx_eq(t->s, t->n, lit);
}

static void strip_b(const uint32_t *s, size_t n, const char32_t *set, size_t *a, size_t *b)
{
	*a = 0;
	*b = n;
	while (*a < *b && nx_inset(s[*a], set))
		(*a)++;
	while (*b > *a && nx_inset(s[*b - 1], set))
		(*b)--;
}

/* token.lower().strip(PUNCT_STRIP) (normalize_context_token) into o */
static void clean_of(const uint32_t *s, size_t n, rn_str *o)
{
	o->n = 0;
	rn_str l = {0};
	nx_lower(s, n, &l);
	size_t a, b;
	strip_b(l.s, l.n, PUNCT_STRIP, &a, &b);
	nx_put(o, l.s + a, b - a);
	if (!o->s)
		nx_put(o, U"", 0);
	rn_str_free(&l);
}

static int is_word_char(uint32_t c)
{
	return rn_py_isalnum(c) && !rn_py_isdecimal(c) && c != '_';
}

static int is_word_token(const rn_tok *t)
{
	if (!t->n)
		return 0;
	for (size_t i = 0; i < t->n; i++)
		if (!is_word_char(t->s[i]))
			return 0;
	return 1;
}

static int has_char(const uint32_t *s, size_t n, uint32_t c)
{
	for (size_t i = 0; i < n; i++)
		if (s[i] == c)
			return 1;
	return 0;
}

static int u8eq(const uint32_t *s, size_t n, const char *u)
{
	rn_str x = {0};
	nx_put8(&x, u);
	int r = x.n == n && (!n || !memcmp(x.s, s, n * 4));
	rn_str_free(&x);
	return r;
}

/* _is_ambiguous_preposition_token */
static int ambiguous_prep(const uint32_t *s, size_t n)
{
	rn_str c = {0};
	clean_of(s, n, &c);
	int r = c.n && !has_char(c.s, c.n, ' ') && rn_in32(PREP_CASE, c.s, c.n);
	rn_str_free(&c);
	return r;
}

static const cmorphy3_form *sel_pos(const uint32_t *s, size_t n, int p0, int p1, int p2, int p3)
{
	nx_selq q = NX_SELQ;
	q.pos[0] = p0;
	q.pos[1] = p1;
	q.pos[2] = p2;
	q.pos[3] = p3;
	return nx_select_w(s, n, &q);
}

static const cmorphy3_form *sel_noun(const uint32_t *s, size_t n)
{
	return sel_pos(s, n, rn_g.NOUN, -1, -1, -1);
}

/* _could_be_prepositional_object */
static int could_be_object(const uint32_t *s, size_t n)
{
	rn_str c = {0};
	clean_of(s, n, &c);
	int r = 0;
	if (c.n)
		r = rn_py_isdigit(c.s[0]) || sel_pos(c.s, c.n, rn_g.NOUN, rn_g.ADJF, rn_g.PRTF, rn_g.NPRO) ||
		    sel_pos(c.s, c.n, rn_g.NUMR, -1, -1, -1);
	rn_str_free(&c);
	return r;
}

/* _should_skip_unit_candidate(tokens, start, span, unit_raw) */
static int skip_unit(const toks *T, size_t start, size_t span, const uint32_t *raw, size_t rn)
{
	if (rn && rn_py_isupper(raw[0])) {
		rn_str l = {0};
		nx_lower(raw, rn, &l);
		int differs = l.n != rn || memcmp(l.s, raw, rn * 4);
		rn_str_free(&l);
		if (differs) {
			size_t f = start + span;
			if (f < T->n) {
				size_t a, b;
				rn_py_strip(T->t[f].s, T->t[f].n, &a, &b);
				if (nx_eq(T->t[f].s + a, b - a, U"."))
					f++;
			}
			if (f < T->n) {
				size_t a, b;
				rn_py_strip(T->t[f].s, T->t[f].n, &a, &b);
				if (nx_eq(T->t[f].s + a, b - a, U":"))
					return 1;
			}
		}
	}
	if (span == 1 && start + 1 < T->n) {
		size_t a, b;
		strip_b(raw, rn, PUNCT_STRIP, &a, &b);
		if (a == 0 && b == rn && ambiguous_prep(raw, rn)) {
			const rn_tok *nx = &T->t[start + 1];
			strip_b(nx->s, nx->n, PUNCT_STRIP, &a, &b);
			if (a < b && could_be_object(nx->s + a, b - a))
				return 1;
		}
	}
	if (span > 1) {
		for (size_t i = start; i < start + span && i < T->n; i++)
			if (!is_word_token(&T->t[i]))
				return 0;
		for (size_t i = start + 1; i < start + span && i < T->n; i++)
			if (ambiguous_prep(T->t[i].s, T->t[i].n))
				return 1;
	}
	return 0;
}

/* match_unit_tokens(tokens, start): the descriptor, alias (normalized), source, consumed */
typedef struct {
	const rn_unit *d;
	rn_str alias, source;
	size_t consumed;
} umatch;

static void umatch_free(umatch *m)
{
	rn_str_free(&m->alias);
	rn_str_free(&m->source);
}

static int match_unit_tokens(const toks *T, size_t start, umatch *best)
{
	memset(best, 0, sizeof *best);
	size_t maxe = start + MAX_UNIT_TOKENS < T->n ? start + MAX_UNIT_TOKENS : T->n;
	rn_str cand = {0}, norm = {0};
	for (size_t end = start + 1; end <= maxe; end++) {
		const rn_tok *t = &T->t[end - 1];
		if (has_char(t->s, t->n, '\n'))
			break;
		nx_put(&cand, t->s, t->n);
		size_t a, b;
		rn_py_strip(cand.s, cand.n, &a, &b);
		norm.n = 0;
		if (rn_sub_tmpl(rn_pat(RN_NUMERALS_UNIT_EXPRESSIONS_SEPARATOR_SPACING), cand.s + a,
		                b - a, (const uint32_t *)U"\1", 0, &norm) < 0) {
			nx_fail();
			break;
		}
		const rn_unit *d = rn_unit_resolve(norm.s ? norm.s : (const uint32_t *)U"", norm.n);
		if (d) {
			best->d = d;
			best->alias.n = 0;
			nx_put(&best->alias, norm.s, norm.n);
			best->source.n = 0;
			nx_put(&best->source, cand.s, cand.n);
			best->consumed = end - start;
		}
	}
	rn_str_free(&cand);
	rn_str_free(&norm);
	if (!best->d)
		umatch_free(best);
	return best->d != NULL;
}

/* re.search(re.escape(pat), s, re.IGNORECASE): the index or -1 */
static uint32_t lower1(uint32_t c)
{
	rn_str l = {0};
	uint32_t r = c;
	if (rn_py_lower(&c, 1, &l) == 0 && l.n == 1)
		r = l.s[0];
	rn_str_free(&l);
	return r;
}

static long ci_find(const uint32_t *s, size_t n, const uint32_t *p, size_t pn)
{
	for (size_t i = 0; i + pn <= n; i++) {
		size_t k = 0;
		while (k < pn && (s[i + k] == p[k] || lower1(s[i + k]) == lower1(p[k])))
			k++;
		if (k == pn)
			return (long)i;
	}
	return -1;
}

/* ---- the number site ---- */

typedef struct {
	const toks *T;
	size_t idx;
	const rn_tok *token;
	rn_str clean;     /* clean_token */
	char *d;          /* str(int(clean_token)) */
	uint64_t v;       /* value, saturated */
	int neg, gcase;
	rn_str num_words;
	rn_ncase *nc;
} site;

static int nform(const site *s)
{
	return nx_nform(s->d);
}

/* build_number_token(token, clean_token, replacement, is_negative) */
static void build_number_token(rn_str *o, const site *s, const rn_str *rep)
{
	const rn_tok *t = s->token;
	rn_str src = {0}, res = {0};
	if (s->neg)
		nx_putc(&src, RN_NEGATIVE_NUMBER);
	nx_put(&src, s->clean.s, s->clean.n);
	long at = src.n ? rn_py_find(t->s, t->n, src.s, src.n, 0) : 0;
	int changed = 0;
	if (at >= 0) {
		nx_put(&res, t->s, (size_t)at);
		nx_put(&res, rep->s, rep->n);
		nx_put(&res, t->s + at + src.n, t->n - (size_t)at - src.n);
		changed = res.n != t->n || memcmp(res.s, t->s, t->n * 4);
	}
	if (!changed) {
		res.n = 0;
		size_t a, b;
		strip_b(t->s, t->n, PUNCT_STRIP, &a, &b);
		if (a < b) {
			long k = rn_py_find(t->s, t->n, t->s + a, b - a, 0);
			nx_put(&res, t->s, (size_t)k);
			nx_put(&res, rep->s, rep->n);
			nx_put(&res, t->s + k + (b - a), t->n - (size_t)k - (b - a));
		} else
			nx_put(&res, rep->s, rep->n);
	}
	if (s->neg)
		nx_lit(o, U"минус ");
	nx_put(o, res.s, res.n);
	rn_str_free(&src);
	rn_str_free(&res);
}

/* _negated_one(tokens, idx): «не 1000 лет» keeps «одну» */
static int negated_one(const toks *T, size_t idx)
{
	if (!idx)
		return 0;
	rn_str l = {0};
	nx_lower(T->t[idx - 1].s, T->t[idx - 1].n, &l);
	int r = nx_eq(l.s, l.n, U"не");
	rn_str_free(&l);
	return r;
}

/* _bare_thousand: a round 1000 drops the leading «одна» (in place) */
static void bare_thousand(const site *s, rn_str *w)
{
	if (s->neg || !nx_eq(s->clean.s, s->clean.n, U"1000"))
		return;
	static const char32_t *const ONE[] = {U"одна", U"одну", U"одной", U"одною", NULL};
	for (const char32_t *const *o = ONE; *o; o++) {
		size_t k = 0;
		while ((*o)[k])
			k++;
		if (!nx_starts(w->s, w->n, *o))
			continue;
		size_t e = k;
		while (e < w->n && rn_py_isspace(w->s[e]))
			e++;
		if (e == k || !nx_starts(w->s + e, w->n - e, U"тысяч"))
			return;
		memmove(w->s, w->s + e, (w->n - e) * sizeof *w->s);
		w->n -= e;
		return;
	}
}

static void rendered_k(rn_str *o, const site *s, int gcase, int gender, int animate, int keep_one)
{
	rn_str r = {0};
	nx_numeral(&r, s->clean.s, s->clean.n, gcase, gender, animate);
	if (!keep_one && !negated_one(s->T, s->idx))
		bare_thousand(s, &r);
	build_number_token(o, s, &r);
	rn_str_free(&r);
}

/* _is_money_lemma */
static int money_lemma(const char *lemma)
{
	rn_str w = {0};
	nx_put8(&w, lemma);
	const rn_unit *u = rn_unit_resolve(w.s ? w.s : (const uint32_t *)U"", w.n);
	rn_str_free(&w);
	return u && !strcmp(u->category, "money");
}

/* paucal_accusative_case(value, case, animate) */
static int paucal(const site *s, int gcase, int animate)
{
	if (gcase != rn_g.accs)
		return gcase;
	unsigned v10 = nx_dmod(s->d, 10), v100 = nx_dmod(s->d, 100);
	if (v10 < 2 || v10 > 4 || (v100 >= 12 && v100 <= 14))
		return gcase;
	return animate && s->v >= 2 && s->v <= 4 ? rn_g.gent : rn_g.nomn;
}

/* ---- segments ---- */

typedef struct {
	size_t first, last;
	int has;
	rn_str rep;
} seg;

typedef struct {
	seg *v;
	size_t n, cap;
} segs;

static seg *seg_add(segs *g, size_t first, size_t last)
{
	if (g->n == g->cap) {
		size_t c = g->cap ? g->cap * 2 : 64;
		seg *v = realloc(g->v, c * sizeof *v);
		if (!v) {
			nx_fail();
			return NULL;
		}
		g->v = v;
		g->cap = c;
	}
	seg *s = &g->v[g->n++];
	*s = (seg){first, last, 0, {0}};
	return s;
}

static void seg_keep(segs *g, size_t first, size_t last)
{
	seg_add(g, first, last);
}

/* a replacement segment; takes over r */
static void seg_rep(segs *g, size_t first, size_t last, rn_str *r)
{
	seg *s = seg_add(g, first, last);
	if (s) {
		s->has = 1;
		s->rep = *r;
		if (!s->rep.s)
			nx_put(&s->rep, U"", 0);
	} else
		rn_str_free(r);
	*r = (rn_str){0};
}

/* ---- handlers ---- */

/* _counted_one_form_agrees */
static int one_form_agrees(const site *s, const uint32_t *w, size_t n)
{
	if (!rn_word_is_known(w, n))
		return 1;
	int e1 = s->gcase, e2 = s->gcase == rn_g.accs ? rn_g.nomn : s->gcase;
	int e3 = s->gcase == rn_g.nomn ? rn_g.accs : -2;
	const cmorphy3_form *f;
	int np = rn_mparse(w, n, &f);
	for (int i = 0; i < np; i++) {
		if (!rn_has(f[i].tag, rn_g.NOUN) || !rn_has(f[i].tag, rn_g.sing))
			continue;
		int c = rn_case(f[i].tag);
		if (c >= 0 && (c == e1 || c == e2 || c == e3))
			return 1;
	}
	return 0;
}

static int one_group_genitive(const site *s, const rn_str *alias);
static int paucal_group_genitive(const site *s, const rn_str *alias);

static int handle_adjective_and_noun(site *s, segs *g, size_t *consumed)
{
	const toks *T = s->T;
	size_t i = s->idx;
	if (i + 2 >= T->n)
		return 0;
	const rn_tok *adj = &T->t[i + 1], *noun = &T->t[i + 2];
	rn_str ca = {0}, cn = {0};
	clean_of(adj->s, adj->n, &ca);
	clean_of(noun->s, noun->n, &cn);
	int ok = 0;
	const cmorphy3_form *pa = sel_pos(ca.s, ca.n, rn_g.ADJF, rn_g.PRTF, -1, -1);
	if (!pa && (!rn_py_str_isalpha(ca.s, ca.n) || has_char(adj->s, adj->n, ',') ||
	            rn_word_is_known(ca.s, ca.n)))
		goto out;
	const cmorphy3_form *pn = ambiguous_prep(cn.s, cn.n) ? NULL : sel_noun(cn.s, cn.n);
	if (!pn || rn_unit_resolve(adj->s, adj->n))
		goto out;
	int ggroup = one_group_genitive(s, &cn) || paucal_group_genitive(s, &cn);
	if (nform(s) == 'o' && !ggroup && !one_form_agrees(s, cn.s, cn.n))
		goto out;
	int anim = rn_has(pn->tag, rn_g.anim);
	rn_str r = {0};
	rendered_k(&r, s, paucal(s, ggroup ? rn_g.gent : s->gcase, anim), rn_gender(pn->tag), anim,
	           money_lemma(pn->normal_form));
	seg_rep(g, i, i + 1, &r);
	seg_keep(g, i + 1, i + 3);
	*consumed = 3;
	ok = 1;
out:
	rn_str_free(&ca);
	rn_str_free(&cn);
	return ok;
}

/* _UnitAtSite */
typedef struct {
	rn_str alias, noun_token, unit_raw;
	size_t span;
	const rn_unit *info;
} unitat;

static void unitat_free(unitat *u)
{
	rn_str_free(&u->alias);
	rn_str_free(&u->noun_token);
	rn_str_free(&u->unit_raw);
}

static const rn_unit *resolve_lit(const char32_t *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return rn_unit_resolve((const uint32_t *)s, n);
}

/* is_coordinate_marker(tokens, index) */
static int coordinate_marker(const toks *T, size_t i)
{
	if (i + 3 >= T->n)
		return 0;
	rn_str h = {0}, a = {0};
	clean_of(T->t[i].s, T->t[i].n, &h);
	clean_of(T->t[i + 2].s, T->t[i + 2].n, &a);
	static const char32_t *const HEMI[] = {U"с", U"ю", U"в", U"з", NULL};
	static const char32_t *const AXIS[] = {U"ш", U"д", NULL};
	int r = nx_oneof(h.s, h.n, HEMI) && tok_is(&T->t[i + 1], U".") && nx_oneof(a.s, a.n, AXIS) &&
	        tok_is(&T->t[i + 3], U".");
	rn_str_free(&h);
	rn_str_free(&a);
	return r;
}

/* _surface_in_paradigm(token, descriptor) */
static int surface_in_paradigm(const uint32_t *s, size_t n, const rn_unit *u)
{
	const char *p = u->m.paradigm;
	if (!p || !*p)
		return 0;
	rn_str l = {0}, f = {0};
	nx_lower(s, n, &l);
	size_t a, b;
	strip_b(l.s, l.n, U".", &a, &b);
	int r = 0;
	while (*p && !r) {
		const char *e = strchr(p, ';');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		/* "case number word" */
		const char *w = p;
		int sp = 0;
		while (w < p + len && sp < 2)
			if (*w++ == ' ')
				sp++;
		char *tmp = strndup(w, (size_t)(p + len - w));
		if (!tmp) {
			nx_fail();
			break;
		}
		f.n = 0;
		nx_put8(&f, tmp);
		free(tmp);
		r = f.n == b - a && (!f.n || !memcmp(f.s, l.s + a, f.n * 4));
		p += len;
		if (*p == ';')
			p++;
	}
	rn_str_free(&l);
	rn_str_free(&f);
	return r;
}

/* _animate_counted_noun(tokens, index) */
static int animate_counted_noun(const toks *T, size_t i)
{
	if (i >= T->n)
		return 0;
	const rn_unit *u = rn_unit_resolve(T->t[i].s, T->t[i].n);
	if (u)
		return !strcmp(u->lemma, "человек");
	rn_str c = {0};
	clean_of(T->t[i].s, T->t[i].n, &c);
	int r = 0;
	if (c.n) {
		const cmorphy3_form *f = sel_noun(c.s, c.n);
		r = f && rn_has(f->tag, rn_g.anim);
	}
	rn_str_free(&c);
	return r;
}

/* _money_amount_precedes(tokens, index) */
static int money_amount_precedes(const toks *T, size_t index)
{
	long j = (long)index - 1;
	while (j >= 0 && tok_is(&T->t[j], U"."))
		j--;
	if (j <= 0)
		return 0;
	size_t a, b;
	strip_b(T->t[j].s, T->t[j].n, PUNCT_STRIP, &a, &b);
	const rn_unit *u = rn_unit_resolve(T->t[j].s + a, b - a);
	if (!u) {
		rn_str w = {0};
		nx_put(&w, T->t[j].s + a, b - a);
		nx_putc(&w, '.');
		u = rn_unit_resolve(w.s, w.n);
		rn_str_free(&w);
	}
	if (!u || (strcmp(u->lemma, "рубль") && strcmp(u->lemma, "гривна")))
		return 0;
	j--;
	while (j >= 0 && tok_is(&T->t[j], U"."))
		j--;
	if (j < 0)
		return 0;
	strip_b(T->t[j].s, T->t[j].n, PUNCT_STRIP, &a, &b);
	return rn_py_str_isdigit(T->t[j].s + a, b - a);
}

static void resolve_unit_at(const site *s, unitat *u)
{
	const toks *T = s->T;
	size_t i = s->idx;
	memset(u, 0, sizeof *u);
	const rn_tok *nt = &T->t[i + 1];
	nx_put(&u->noun_token, nt->s, nt->n);
	clean_of(nt->s, nt->n, &u->alias);
	umatch m;
	int have = match_unit_tokens(T, i + 1, &m);
	if (have && skip_unit(T, i + 1, m.consumed, m.source.s, m.source.n)) {
		umatch_free(&m);
		have = 0;
	}
	u->span = 1;
	nx_put(&u->unit_raw, nt->s, nt->n);
	if (nx_eq(u->alias.s, u->alias.n, U"°") && i + 2 < T->n) {
		size_t a, b;
		strip_b(T->t[i + 2].s, T->t[i + 2].n, PUNCT_STRIP, &a, &b);
		rn_str merged = {0};
		nx_putc(&merged, U'°');
		nx_put(&merged, T->t[i + 2].s + a, b - a);
		if (a < b && rn_unit_resolve(merged.s, merged.n) && !coordinate_marker(T, i + 2)) {
			nx_lower(merged.s, merged.n, &u->alias);
			nx_put(&u->noun_token, T->t[i + 2].s, T->t[i + 2].n);
			u->unit_raw.n = 0;
			nx_put(&u->unit_raw, u->noun_token.s, u->noun_token.n);
			u->span = 2;
		}
		rn_str_free(&merged);
	} else if (have) {
		/* match.alias.casefold().strip(".") */
		rn_str f = {0};
		rn_py_casefold(m.alias.s, m.alias.n, &f);
		size_t a, b;
		strip_b(f.s, f.n, U".", &a, &b);
		u->alias.n = 0;
		nx_put(&u->alias, f.s + a, b - a);
		if (!u->alias.s)
			nx_put(&u->alias, U"", 0);
		rn_str_free(&f);
		u->unit_raw.n = 0;
		nx_put(&u->unit_raw, m.source.s, m.source.n);
		u->span = m.consumed;
	}
	if (have)
		umatch_free(&m);
	const uint32_t *raw = u->unit_raw.s;
	size_t rn = u->unit_raw.n;
	if (skip_unit(T, i + 1, u->span, raw, rn))
		return;
	const rn_unit *info = rn_unit_resolve(raw, rn);
	if (!info && u->span == 1) {
		const rn_unit *f = rn_unit_resolve_form(raw, rn);
		if (f && (surface_in_paradigm(raw, rn, f) || f->m.indeclinable))
			info = f;
	}
	size_t a, b;
	strip_b(raw, rn, PUNCT_STRIP, &a, &b);
	if (info && !strcmp(info->lemma, "тонна")) {
		size_t b2 = b;
		while (b2 > a && raw[b2 - 1] == '.')
			b2--;
		if (b2 - a == 1 && (raw[a] == U'т' || raw[a] == U'Т') &&
		    animate_counted_noun(T, i + 1 + u->span))
			info = resolve_lit(U"тыс.");
	}
	if (info && !strcmp(info->lemma, "век") && s->v > 21)
		info = resolve_lit(U"верста");
	if (!info && u->span == 1 && nx_eq(raw + a, b - a, U"к") && i + 2 < T->n &&
	    tok_is(&T->t[i + 2], U".") && money_amount_precedes(T, i))
		info = resolve_lit(U"коп.");
	u->info = info;
}

static int landmark_follows_unit(const site *s, const unitat *u)
{
	const toks *T = s->T;
	size_t after = s->idx + 1 + u->span;
	if (after < T->n && tok_is(&T->t[after], U"."))
		after++;
	if (after >= T->n)
		return 0;
	rn_str c = {0};
	clean_of(T->t[after].s, T->t[after].n, &c);
	int r = nx_eq(c.s, c.n, U"от") || nx_eq(c.s, c.n, U"до");
	rn_str_free(&c);
	return r;
}

static int unit_case_at_site(const site *s, const unitat *u, const rn_unit *info)
{
	int c = s->gcase;
	if (s->v >= 1000 && s->v <= 2100 && s->idx > 0) {
		rn_str l = {0};
		const rn_tok *t = &s->T->t[s->idx - 1];
		clean_of(t->s, t->n, &l);
		if (nx_eq(l.s, l.n, U"в") && !landmark_follows_unit(s, u))
			c = rn_g.accs;
		rn_str_free(&l);
	}
	if (info->m.indeclinable) {
		size_t a, b;
		strip_b(u->alias.s, u->alias.n, U".", &a, &b);
		if (u8eq(u->alias.s + a, b - a, info->lemma)) {
			int g = rn_ncase_get(s->nc, s->idx, s->idx + 1, NULL);
			if (g < 0)
				nx_fail();
			else
				c = g;
		}
	}
	return c;
}

/* _is_redundant_unit_token(tokens, index, lemma) */
static int redundant_unit(const toks *T, size_t i, const char *lemma)
{
	if (i >= T->n)
		return 0;
	rn_str c = {0};
	clean_of(T->t[i].s, T->t[i].n, &c);
	int r = 0;
	if (c.n) {
		const cmorphy3_form *f = rn_mfirst(c.s, c.n);
		if (f && rn_has(f->tag, rn_g.NOUN) && !strcmp(f->normal_form, lemma)) {
			const rn_tok *t = &T->t[i];
			size_t a = 0;
			while (a < t->n && nx_inset(t->s[a], PUNCT_STRIP))
				a++;
			r = !(a < t->n && rn_py_isupper(t->s[a])) && rn_case(f->tag) == rn_g.gent;
		}
	}
	rn_str_free(&c);
	return r;
}

static int is_currency_symbol(const rn_str *alias)
{
	if (alias->n != 1 || rn_py_isalnum(alias->s[0]))
		return 0;
	const rn_unit *u = rn_unit_resolve(alias->s, 1);
	return u && !strcmp(u->category, "money");
}

static int handle_money_multiplier(site *s, const unitat *u, const rn_unit *info, int gcase,
                                   segs *g, size_t *consumed)
{
	const toks *T = s->T;
	size_t i = s->idx;
	if (strcmp(info->category, "money") || !is_currency_symbol(&u->alias) || i + 2 >= T->n)
		return 0;
	const rn_tok *mt = &T->t[i + 2];
	rn_str ml = {0};
	clean_of(mt->s, mt->n, &ml);
	const cmorphy3_form *p = rn_mfirst(ml.s, ml.n);
	rn_str_free(&ml);
	if (!p || !rn_has(p->tag, rn_g.NOUN) || !in8(p->normal_form, MULTIPLIERS))
		return 0;
	int gen = rn_gender(p->tag);
	rn_str r = {0};
	rendered_k(&r, s, nx_icase(s->d, gcase), gen >= 0 ? gen : rn_g.masc, 0, 1);
	size_t c = redundant_unit(T, i + 3, info->lemma) ? 4 : 3;
	nx_putc(&r, ' ');
	nx_put(&r, mt->s, mt->n);
	nx_putc(&r, ' ');
	nx_unit_lemma(&r, info, nx_gs("gent plur"), 0);
	seg_rep(g, i, i + c, &r);
	*consumed = c;
	return 1;
}

static void render_counted_unit(rn_str *o, const site *s, const rn_unit *info, int gcase)
{
	if (!strcmp(info->lemma, "человек") && (gcase == rn_g.nomn || gcase == rn_g.accs)) {
		int f = nform(s);
		nx_lit(o, gcase == rn_g.accs && f == 'f' ? U"человек" : f == 'f' ? U"человека" : U"человек");
		if (info->suffix) {
			nx_putc(o, ' ');
			nx_put8(o, info->suffix);
		}
		return;
	}
	nx_render_unit(o, info, nx_ttags(s->d, gcase), nform(s) == 'f');
}

/* should_consume_abbreviation_dot(tokens, dot_index) */
static int consume_dot(const toks *T, size_t di)
{
	if (di >= T->n || !tok_is(&T->t[di], U"."))
		return 0;
	if (di + 1 >= T->n)
		return 0;
	const rn_tok *nx = &T->t[di + 1];
	if (has_char(nx->s, nx->n, '\n'))
		return 0;
	static const char32_t *const CLOSERS[] = {U",", U";", U":", U")", U"]", U"}",
	                                          U"»", U"\"", U"”", NULL};
	if (nx_oneof(nx->s, nx->n, CLOSERS))
		return 1;
	size_t a, b;
	strip_b(nx->s, nx->n, PUNCT_STRIP, &a, &b);
	if (a == b)
		return 0;
	return rn_py_islower(nx->s[a]) || rn_py_isdigit(nx->s[a]);
}

/* should_keep_tokenized_decimal_unit_dot(tokens, start) */
static int keep_unit_dot(const toks *T, size_t start)
{
	for (size_t i = start; i < T->n; i++) {
		const rn_tok *t = &T->t[i];
		if (!t->n)
			continue;
		if (has_char(t->s, t->n, '\n'))
			return 1;
		uint32_t c = t->s[0];
		if (nx_inset(c, U".!?…"))
			return 1;
		if (c == ',')
			return 0;
		return !(rn_py_islower(c) || rn_py_isdigit(c));
	}
	return 1;
}

static void handle_unit(site *s, const unitat *u, const rn_unit *info, int gcase, segs *g,
                        size_t *consumed)
{
	const toks *T = s->T;
	size_t i = s->idx;
	rn_str num = {0}, full = {0}, r = {0};
	rendered_k(&num, s, nx_icase(s->d, gcase), gram_of(info->gender), 0,
	           !strcmp(info->category, "money"));
	render_counted_unit(&full, s, info, gcase);
	if (u->span == 1) {
		long at = ci_find(u->noun_token.s, u->noun_token.n, u->alias.s, u->alias.n);
		if (at >= 0) {
			rn_str x = {0};
			nx_put(&x, u->noun_token.s, (size_t)at);
			nx_put(&x, full.s, full.n);
			size_t e = (size_t)at + u->alias.n;
			nx_put(&x, u->noun_token.s + e, u->noun_token.n - e);
			rn_str_free(&full);
			full = x;
		}
	}
	int preserve = u->unit_raw.n && u->unit_raw.s[u->unit_raw.n - 1] == '.' &&
	               !consume_dot(T, i + u->span);
	if (preserve && keep_unit_dot(T, i + 1 + u->span))
		nx_putc(&full, '.');
	size_t step = 1 + u->span;
	if (u->span == 1 && u->alias.n == 2 && u->alias.s[0] == U'°')
		step = 3;
	if (i + step < T->n && consume_dot(T, i + step))
		step++;
	if (redundant_unit(T, i + step, info->lemma))
		step++;
	nx_put(&r, num.s, num.n);
	nx_putc(&r, ' ');
	nx_put(&r, full.s, full.n);
	size_t c = step;
	umatch m;
	if (in8(info->lemma, MULTIPLIERS) && i + step < T->n && match_unit_tokens(T, i + step, &m)) {
		size_t nc = m.consumed;
		size_t last = i + step + nc - 1;
		if (tok_is(&T->t[last], U".") && !consume_dot(T, last))
			nc--;
		c = step + nc;
		nx_putc(&r, ' ');
		nx_render_unit(&r, m.d, nx_gs("gent plur"), 0);
		umatch_free(&m);
	}
	seg_rep(g, i, i + c, &r);
	*consumed = c;
	rn_str_free(&num);
	rn_str_free(&full);
}

/* _dependent_genitive_follows */
static int dependent_genitive(const toks *T, size_t i)
{
	if (i >= T->n)
		return 0;
	rn_str c = {0};
	clean_of(T->t[i].s, T->t[i].n, &c);
	int r = 0;
	if (c.n && rn_py_str_isalpha(c.s, c.n)) {
		const cmorphy3_form *f = rn_mfirst(c.s, c.n);
		r = f && rn_has(f->tag, rn_g.NOUN) && rn_case(f->tag) == rn_g.gent;
	}
	rn_str_free(&c);
	return r;
}

static int any_gent_plur_noun(const uint32_t *s, size_t n)
{
	const cmorphy3_form *f;
	int np = rn_mparse(s, n, &f);
	for (int i = 0; i < np; i++)
		if (rn_has(f[i].tag, rn_g.NOUN) && rn_case(f[i].tag) == rn_g.gent &&
		    rn_has(f[i].tag, rn_g.plur))
			return 1;
	return 0;
}

static int ordinal_for_mismatched(site *s, const rn_str *alias, const cmorphy3_form *pn,
                                  segs *g, size_t *consumed)
{
	const toks *T = s->T;
	size_t i = s->idx;
	if (s->neg || nform(s) == 'o')
		return 0;
	uint16_t tag = pn->tag;
	int tc = rn_case(tag);
	if (!rn_has(tag, rn_g.sing) || tc < 0 || rn_has(tag, rn_g.Fixd))
		return 0;
	if (!rn_reliable(rn_mfirst(alias->s, alias->n)))
		return 0;
	if (tc == rn_g.nomn)
		return 0;
	if ((tc == rn_g.gent || tc == rn_g.accs) && nform(s) == 'f')
		return 0;
	if (tc == rn_g.gent && in8(pn->normal_form, DAY_PARTS) && s->v > 0 &&
	    s->v <= MAX_CLOCK_HOUR && !dependent_genitive(T, i + 2))
		return 0;
	if (tc == rn_g.gent && s->v > 31)
		return 0;
	if (any_gent_plur_noun(alias->s, alias->n))
		return 0;
	if (i > 0) {
		const rn_tok *lr = &T->t[i - 1];
		const uint32_t *ls = lr->s;
		size_t ln = lr->n;
		if (tok_is(lr, U".") && i > 1) {
			const rn_tok *prev = &T->t[i - 2];
			if (rn_py_str_isalpha(prev->s, prev->n) && prev->n <= 3)
				return 0;
			ln = 0;
		}
		if (has_char(ls, ln, U'№'))
			return 0;
		rn_str l = {0};
		clean_of(ls, ln, &l);
		int stop = l.n && rn_reliable(rn_mfirst(l.s, l.n));
		rn_str_free(&l);
		if (stop)
			return 0;
	}
	/* render_ordinal_from_noun_parse(value, p_noun) */
	rn_str r = {0};
	const rn_tok *t = s->token;
	size_t a, b;
	strip_b(t->s, t->n, PUNCT_STRIP, &a, &b);
	if (a == t->n)
		b = a;
	nx_put(&r, t->s, a);
	int gen = rn_gender(tag);
	nx_ordinal(&r, s->d, rn_canon_case(tc), gen >= 0 ? gen : rn_g.masc, rn_has(tag, rn_g.plur),
	           rn_animacy(tag) == rn_g.inan);
	/* token[len(token.rstrip(P)):] */
	size_t e = t->n;
	while (e > 0 && nx_inset(t->s[e - 1], PUNCT_STRIP))
		e--;
	nx_put(&r, t->s + e, t->n - e);
	seg_rep(g, i, i + 1, &r);
	seg_keep(g, i + 1, i + 2);
	*consumed = 2;
	return 1;
}

/* _has_noun_head_on_the_left */
static int noun_head_left(const site *s)
{
	if (s->idx == 0)
		return 0;
	rn_str l = {0};
	const rn_tok *t = &s->T->t[s->idx - 1];
	clean_of(t->s, t->n, &l);
	int r = 0;
	if (l.n && rn_py_str_isalpha(l.s, l.n)) {
		const cmorphy3_form *p = rn_mfirst(l.s, l.n);
		r = p && !in8(p->normal_form, NUMBER_LABELS) && rn_reliable(p);
	}
	rn_str_free(&l);
	return r;
}

/* _paucal_group_is_genitive */
static int paucal_group_genitive(const site *s, const rn_str *alias)
{
	if (nform(s) != 'f' || s->gcase == rn_g.gent)
		return 0;
	if (!rn_word_is_known(alias->s, alias->n))
		return 0;
	const cmorphy3_form *f;
	int np = rn_mparse(alias->s, alias->n, &f);
	for (int i = 0; i < np; i++)
		if (rn_has(f[i].tag, rn_g.NOUN) && rn_case(f[i].tag) == rn_g.gent &&
		    rn_has(f[i].tag, rn_g.sing))
			return 0;
	if (!any_gent_plur_noun(alias->s, alias->n))
		return 0;
	return noun_head_left(s);
}

static const char *const SEQUENCE_NOUNS[] = {
    "армия", "батальон", "бригада", "век",   "выпуск", "глава",  "год",    "день",   "дивизия",
    "квартал", "класс",  "корпус",  "курс",  "номер",  "пункт",  "полк",   "раунд",  "рота",
    "сезон",  "созыв",   "столетие", "съезд", "том",    "тур",    "тысячелетие", "этаж", NULL};

/* _one_group_is_genitive */
static int one_group_genitive(const site *s, const rn_str *alias)
{
	if (nform(s) != 'o' || s->gcase == rn_g.gent)
		return 0;
	if (!rn_word_is_known(alias->s, alias->n))
		return 0;
	const cmorphy3_form *f;
	int np = rn_mparse(alias->s, alias->n, &f);
	int gs = 0;
	for (int i = 0; i < np; i++) {
		if (!rn_has(f[i].tag, rn_g.NOUN))
			continue;
		int c = rn_case(f[i].tag);
		if (rn_has(f[i].tag, rn_g.sing) && (c == rn_g.nomn || c == rn_g.accs))
			return 0;
		if (c == rn_g.gent && rn_has(f[i].tag, rn_g.sing))
			gs = 1;
	}
	if (!gs)
		return 0;
	for (int i = 0; i < np; i++)
		if (rn_has(f[i].tag, rn_g.NOUN) && in8(f[i].normal_form, SEQUENCE_NOUNS))
			return 0;
	return noun_head_left(s);
}

static int handle_counted_noun(site *s, const unitat *u, segs *g, size_t *consumed)
{
	const toks *T = s->T;
	size_t i = s->idx;
	const cmorphy3_form *pn =
	    ambiguous_prep(u->alias.s, u->alias.n) ? NULL : sel_noun(u->alias.s, u->alias.n);
	if (!pn)
		return 0;
	if (ordinal_for_mismatched(s, &u->alias, pn, g, consumed))
		return 1;
	int ggroup = one_group_genitive(s, &u->alias);
	if (nform(s) == 'o' && !ggroup && !one_form_agrees(s, u->alias.s, u->alias.n))
		return 0;
	int anim = rn_has(pn->tag, rn_g.anim);
	int rewrite = 1;
	if (anim && nform(s) == 'f') {
		const cmorphy3_form *f;
		int np = rn_mparse(u->alias.s, u->alias.n, &f);
		int gs = 0, ganim = 0;
		for (int k = 0; k < np; k++) {
			if (rn_pos(f[k].tag) != rn_g.NOUN || rn_case(f[k].tag) != rn_g.gent)
				continue;
			if (rn_number(f[k].tag) == rn_g.sing)
				gs = 1;
			if (rn_has(f[k].tag, rn_g.anim) && rn_number(f[k].tag) == rn_g.plur)
				ganim = 1;
		}
		if (!gs) {
			rewrite = 0;
			anim = ganim;
		}
	}
	if (anim && s->gcase == rn_g.accs && nform(s) == 'f' && rn_case(pn->tag) == rn_g.gent &&
	    rn_number(pn->tag) == rn_g.sing) {
		rn_str pw = {0}, aw = {0};
		if (i > 0)
			clean_of(T->t[i - 1].s, T->t[i - 1].n, &pw);
		const cmorphy3_form *ap = NULL;
		if (i + 2 < T->n) {
			clean_of(T->t[i + 2].s, T->t[i + 2].n, &aw);
			if (aw.n)
				ap = rn_mfirst(aw.s, aw.n);
		}
		if (nx_eq(pw.s, pw.n, U"по") ||
		    (ap && rn_case(ap->tag) == rn_g.gent && rn_number(ap->tag) == rn_g.plur))
			anim = 0;
		rn_str_free(&pw);
		rn_str_free(&aw);
	}
	int ccase = ggroup || paucal_group_genitive(s, &u->alias) ? rn_g.gent : s->gcase;
	const rn_kv *tw = rn_table_get32(RN_T_TIME_WORDS, u->alias.s, u->alias.n);
	int gender = tw ? gram_of(tw->value) : rn_gender(pn->tag);
	rn_str r = {0};
	rendered_k(&r, s, paucal(s, ccase, anim), gender, anim, money_lemma(pn->normal_form));
	seg_rep(g, i, i + 1, &r);
	const rn_str *nt = &u->noun_token;
	rn_str counted = {0};
	int changed = 0;
	if (rewrite && s->gcase == rn_g.accs && anim && s->v >= 2 && s->v <= 4) {
		rn_str inf = {0};
		if (!strcmp(pn->normal_form, "человек"))
			nx_lit(&inf, U"человек");
		else {
			size_t cap = 0;
			char *fb = rn_to_utf8(u->alias.s, u->alias.n, NULL, &cap);
			if (!fb)
				nx_fail();
			nx_safe_inflect(&inf, pn, nx_gs("gent plur"), fb ? fb : "");
			free(fb);
		}
		long at = ci_find(nt->s, nt->n, u->alias.s, u->alias.n);
		if (at >= 0) {
			nx_put(&counted, nt->s, (size_t)at);
			nx_put(&counted, inf.s, inf.n);
			size_t e = (size_t)at + u->alias.n;
			nx_put(&counted, nt->s + e, nt->n - e);
		} else
			nx_put(&counted, nt->s, nt->n);
		changed = counted.n != nt->n || (nt->n && memcmp(counted.s, nt->s, nt->n * 4));
		rn_str_free(&inf);
	}
	if (changed)
		seg_rep(g, i + 1, i + 2, &counted);
	else
		seg_keep(g, i + 1, i + 2);
	rn_str_free(&counted);
	*consumed = 2;
	return 1;
}

static int handle_token_on_the_right(site *s, segs *g, size_t *consumed)
{
	if (s->idx + 1 >= s->T->n)
		return 0;
	unitat u;
	resolve_unit_at(s, &u);
	int r;
	if (!u.info)
		r = handle_counted_noun(s, &u, g, consumed);
	else {
		int c = unit_case_at_site(s, &u, u.info);
		if (!handle_money_multiplier(s, &u, u.info, c, g, consumed))
			handle_unit(s, &u, u.info, c, g, consumed);
		r = 1;
	}
	unitat_free(&u);
	return r;
}

static int handle_coordinated_ordinal(site *s, segs *g, size_t *consumed)
{
	const toks *T = s->T;
	size_t i = s->idx;
	if (i < 2 || s->neg)
		return 0;
	rn_str w = {0};
	nx_lower(T->t[i - 1].s, T->t[i - 1].n, &w);
	int conj = nx_eq(w.s, w.n, U"и");
	rn_str_free(&w);
	if (!conj)
		return 0;
	rn_str left = {0};
	clean_of(T->t[i - 2].s, T->t[i - 2].n, &left);
	int ok = 0;
	if (!left.n)
		goto out;
	const cmorphy3_form *f;
	int np = rn_mparse(left.s, left.n, &f);
	const cmorphy3_form *anum[64];
	int na = 0;
	for (int k = 0; k < np && na < 64; k++)
		if (rn_has(f[k].tag, rn_g.Anum) && rn_case(f[k].tag) >= 0)
			anum[na++] = &f[k];
	const cmorphy3_form *first = rn_mfirst(left.s, left.n);
	if (!na || !first || !rn_has(first->tag, rn_g.Anum))
		goto out;
	const cmorphy3_form *pl = anum[0];
	if (i >= 3) {
		rn_str head = {0};
		clean_of(T->t[i - 3].s, T->t[i - 3].n, &head);
		const cmorphy3_form *ph = head.n ? sel_noun(head.s, head.n) : NULL;
		int hg = ph ? rn_gender(ph->tag) : -1;
		if (ph && hg >= 0) {
			int hc = rn_has(ph->tag, rn_g.loc2) ? rn_g.loct : rn_case(ph->tag);
			const cmorphy3_form *firstag = NULL, *withcase = NULL;
			for (int k = 0; k < na; k++)
				if (rn_gender(anum[k]->tag) == hg) {
					if (!firstag)
						firstag = anum[k];
					if (!withcase && rn_case(anum[k]->tag) == hc)
						withcase = anum[k];
				}
			if (firstag)
				pl = withcase ? withcase : firstag;
		}
		rn_str_free(&head);
	}
	int c = rn_has(pl->tag, rn_g.loc2) ? rn_g.loct : rn_case(pl->tag);
	rn_str r = {0};
	nx_ordinal(&r, s->d, c, rn_gender(pl->tag), rn_has(pl->tag, rn_g.plur),
	           !rn_has(pl->tag, rn_g.anim));
	seg_rep(g, i, i + 1, &r);
	*consumed = 1;
	ok = 1;
out:
	rn_str_free(&left);
	return ok;
}

/* _counted_noun_across_range: 1 and gender, animacy */
static int noun_across_range(const toks *T, size_t idx, int *gender, int *anim)
{
	if (idx + 3 >= T->n)
		return 0;
	size_t a, b;
	const rn_tok *dash = &T->t[idx + 1];
	strip_b(dash->s, dash->n, PUNCT_STRIP, &a, &b);
	if (!(nx_eq(dash->s + a, b - a, U"—") || nx_eq(dash->s + a, b - a, U"–") ||
	      nx_eq(dash->s + a, b - a, U"-")))
		return 0;
	const rn_tok *rt = &T->t[idx + 2];
	size_t ra, rb;
	strip_b(rt->s, rt->n, PUNCT_STRIP, &ra, &rb);
	if (!rn_py_str_isdigit(rt->s + ra, rb - ra))
		return 0;
	int r = 0;
	rn_str c = {0};
	for (size_t off = 3; off <= 4; off++) {
		if (idx + off >= T->n)
			break;
		clean_of(T->t[idx + off].s, T->t[idx + off].n, &c);
		if (!c.n || ambiguous_prep(c.s, c.n))
			break;
		const cmorphy3_form *mod = sel_pos(c.s, c.n, rn_g.ADJF, rn_g.PRTF, -1, -1);
		if (mod) {
			char d[64];
			int expect_sing = 0;
			if (rb - ra + 3 <= sizeof d && nx_pyint(rt->s + ra, rb - ra, d, sizeof d) >= 0)
				expect_sing = nx_nform(d) == 'o';
			else if (rb - ra + 3 > sizeof d) {
				char *big = malloc(rb - ra + 3);
				if (big && nx_pyint(rt->s + ra, rb - ra, big, rb - ra + 3) >= 0)
					expect_sing = nx_nform(big) == 'o';
				free(big);
			}
			int num = rn_number(mod->tag);
			if (num >= 0 && !rn_has(mod->tag, expect_sing ? rn_g.sing : rn_g.plur))
				goto out;
			continue;
		}
		const cmorphy3_form *pn = sel_noun(c.s, c.n);
		if (pn) {
			*gender = rn_gender(pn->tag);
			*anim = rn_has(pn->tag, rn_g.anim);
			r = 1;
		}
		break;
	}
out:
	rn_str_free(&c);
	return r;
}

static void site_free(site *s)
{
	rn_str_free(&s->clean);
	rn_str_free(&s->num_words);
	free(s->d);
}

/* _number_site: 1 and *s, or 0 (not a number) */
static int number_site(const toks *T, size_t idx, rn_ncase *nc, site *s)
{
	const rn_tok *t = &T->t[idx];
	if (!t->n)
		return 0;
	uint32_t c0 = t->s[0];
	int cand = rn_py_isnumeric(c0) || c0 == RN_NEGATIVE_NUMBER;
	if (!cand && nx_inset(c0, PUNCT_STRIP)) {
		size_t a, b;
		strip_b(t->s, t->n, PUNCT_STRIP, &a, &b);
		cand = a < b && (rn_py_isnumeric(t->s[a]) || t->s[a] == RN_NEGATIVE_NUMBER);
	}
	if (!cand)
		return 0;
	/* parse_integer_token */
	size_t a, b;
	strip_b(t->s, t->n, PUNCT_STRIP, &a, &b);
	int neg = a < b && t->s[a] == RN_NEGATIVE_NUMBER;
	if (neg)
		a++;
	if (a == b)
		return 0;
	memset(s, 0, sizeof *s);
	int ascii = 1;
	for (size_t i = a; i < b; i++)
		if (!(t->s[i] >= 0x30 && t->s[i] <= 0x39))
			ascii = 0;
	if (ascii)
		nx_put(&s->clean, t->s + a, b - a);
	else {
		size_t i = a;
		for (; i < b && rn_py_digit(t->s[i]) >= 0; i++)
			nx_putc(&s->clean, (uint32_t)('0' + rn_py_digit(t->s[i])));
		if (i < b) {
			uint64_t v;
			s->clean.n = 0;
			if (b - a != 1 || !rn_py_intnumeric(t->s[a], &v)) {
				rn_str_free(&s->clean);
				return 0;
			}
			char buf[24];
			int k = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
			for (int j = 0; j < k; j++)
				nx_putc(&s->clean, (uint32_t)buf[j]);
		}
	}
	s->T = T;
	s->idx = idx;
	s->token = t;
	s->neg = neg;
	s->nc = nc;
	s->d = malloc(s->clean.n + 2);
	if (!s->d) {
		nx_fail();
		rn_str_free(&s->clean);
		return 0;
	}
	size_t z = 0;
	while (z + 1 < s->clean.n && s->clean.s[z] == '0')
		z++;
	size_t k = 0;
	for (size_t i = z; i < s->clean.n; i++) {
		unsigned dg = s->clean.s[i] - '0';
		s->d[k++] = (char)s->clean.s[i];
		if (s->v != UINT64_MAX)
			s->v = s->v > (UINT64_MAX - dg) / 10 ? UINT64_MAX : s->v * 10 + dg;
	}
	s->d[k] = 0;
	int gc = rn_ncase_get(nc, idx, SIZE_MAX, NULL);
	if (gc < 0) {
		nx_fail();
		gc = rn_g.nomn;
	}
	s->gcase = gc;
	int rg, ra;
	rn_str num = {0};
	if (noun_across_range(T, idx, &rg, &ra))
		nx_numeral(&num, s->clean.s, s->clean.n, paucal(s, gc, ra), rg, ra);
	else
		nx_numeral(&num, s->clean.s, s->clean.n, nx_icase(s->d, gc), -1, -1);
	if (!negated_one(T, idx))
		bare_thousand(s, &num);
	build_number_token(&s->num_words, s, &num);
	rn_str_free(&num);
	return 1;
}

/* ---- _splice_segments ---- */

typedef struct {
	rn_str *v;
	size_t n, cap;
} chunks;

static void ch_insert(chunks *c, size_t at, const uint32_t *s, size_t n)
{
	if (c->n == c->cap) {
		size_t cap = c->cap ? c->cap * 2 : 64;
		rn_str *v = realloc(c->v, cap * sizeof *v);
		if (!v) {
			nx_fail();
			return;
		}
		c->v = v;
		c->cap = cap;
	}
	memmove(c->v + at + 1, c->v + at, (c->n - at) * sizeof *c->v);
	c->v[at] = (rn_str){0};
	c->n++;
	nx_put(&c->v[at], s, n);
}

static void ch_add(chunks *c, const uint32_t *s, size_t n)
{
	ch_insert(c, c->n, s, n);
}

static long last_nonempty(const chunks *c, size_t upto)
{
	for (size_t i = upto; i-- > 0;)
		if (c->v[i].n)
			return (long)i;
	return -1;
}

static int needs_space(uint32_t l, uint32_t r)
{
	return (rn_py_isalnum(l) || l == U'—' || l == '*') && (rn_py_isalnum(r) || r == U'—' || r == '*');
}

static void space_before_trailing_dash(chunks *c, size_t di)
{
	rn_str *ch = &c->v[di];
	if (ch->n > 1) {
		if (rn_py_isalnum(ch->s[ch->n - 2])) {
			uint32_t last = ch->s[ch->n - 1];
			ch->s[ch->n - 1] = ' ';
			nx_putc(ch, last);
		}
		return;
	}
	long p = last_nonempty(c, di);
	if (p >= 0 && rn_py_isalnum(c->v[p].s[c->v[p].n - 1]))
		ch_insert(c, di, U" ", 1);
}

static void splice(rn_str *out, const uint32_t *text, size_t tn, const toks *T, segs *g)
{
	chunks c = {0};
	size_t cursor = 0;
	for (size_t k = 0; k < g->n; k++) {
		seg *sg = &g->v[k];
		size_t start = (size_t)(T->t[sg->first].s - text);
		size_t end = (size_t)(T->t[sg->last - 1].s + T->t[sg->last - 1].n - text);
		if (!sg->has && k > 0 && g->v[k - 1].has && k + 1 < g->n && g->v[k + 1].has &&
		    (nx_eq(text + start, end - start, U"-") || nx_eq(text + start, end - start, U"–") ||
		     nx_eq(text + start, end - start, U"—"))) {
			sg->has = 1;
			nx_lit(&sg->rep, U"—");
		}
		size_t gap = start - cursor;
		ch_add(&c, text + cursor, gap);
		if (!sg->has)
			ch_add(&c, text + start, end - start);
		else {
			const rn_str *rep = &sg->rep;
			if (rep->n && !gap && c.n) {
				long pi = last_nonempty(&c, c.n);
				if (pi >= 0) {
					rn_str *pc = &c.v[pi];
					if (needs_space(pc->s[pc->n - 1], rep->s[0])) {
						if (pc->s[pc->n - 1] == U'—')
							space_before_trailing_dash(&c, (size_t)pi);
						ch_add(&c, U" ", 1);
					}
				}
			}
			ch_add(&c, rep->s, rep->n);
			if (rep->n && end < tn && needs_space(rep->s[rep->n - 1], text[end]))
				ch_add(&c, U" ", 1);
		}
		cursor = end;
	}
	ch_add(&c, text + cursor, tn - cursor);
	out->n = 0;
	for (size_t i = 0; i < c.n; i++) {
		nx_put(out, c.v[i].s, c.v[i].n);
		rn_str_free(&c.v[i]);
	}
	free(c.v);
}

int rn_n_cardinal_numerals(rn_str *t)
{
	rn_tok *tk = NULL;
	long n = rn_simple_tokenize(t->s, t->n, &tk);
	if (n < 0) {
		nx_fail();
		return -1;
	}
	toks T = {tk, (size_t)n};
	rn_ncase *nc = rn_ncase_new(tk, (size_t)n);
	if (!nc) {
		free(tk);
		nx_fail();
		return -1;
	}
	segs g = {0};
	for (size_t i = 0; i < T.n;) {
		site s;
		if (!number_site(&T, i, nc, &s)) {
			seg_keep(&g, i, i + 1);
			i++;
			continue;
		}
		size_t consumed = 0;
		if (handle_coordinated_ordinal(&s, &g, &consumed) ||
		    handle_adjective_and_noun(&s, &g, &consumed) ||
		    handle_token_on_the_right(&s, &g, &consumed))
			i += consumed;
		else {
			seg_rep(&g, i, i + 1, &s.num_words);
			i++;
		}
		site_free(&s);
	}
	if (g.n) {
		rn_str out = {0};
		splice(&out, t->s, t->n, &T, &g);
		rn_str tmp = *t;
		*t = out;
		rn_str_free(&tmp);
	}
	for (size_t k = 0; k < g.n; k++)
		rn_str_free(&g.v[k].rep);
	free(g.v);
	rn_ncase_free(nc);
	free(tk);
	return rn_mx_failed ? -1 : 0;
}
