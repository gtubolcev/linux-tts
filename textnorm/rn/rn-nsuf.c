/* numerals/numeric_suffixes.py: "2-м", "21-го", "5-ти" */
#include "rn-npass.h"
#include "rn-nx.h"
#include "rn-pat.h"
#include "rn-tables.h"

#define P(x) rn_pat(RN_NUMERALS_NUMERIC_SUFFIXES_##x)
#define PUNCT U".,:;!\"«»()[]{}"

static int CASES[6];

static void init_cases(void)
{
	CASES[0] = rn_g.nomn;
	CASES[1] = rn_g.gent;
	CASES[2] = rn_g.datv;
	CASES[3] = rn_g.accs;
	CASES[4] = rn_g.ablt;
	CASES[5] = rn_g.loct;
}

static int case_order(int c)
{
	for (int i = 0; i < 6; i++)
		if (CASES[i] == c)
			return i;
	return 6;
}

/* token.strip(set) bounds */
static void strip_set(const rn_tok *t, const char32_t *set, size_t *a, size_t *b)
{
	*a = 0;
	*b = t->n;
	while (*a < *b && nx_inset(t->s[*a], set))
		(*a)++;
	while (*b > *a && nx_inset(t->s[*b - 1], set))
		(*b)--;
}

/* token.strip(set).casefold() into k */
static void clean_fold(const rn_tok *t, const char32_t *set, rn_str *k)
{
	size_t a, b;
	strip_set(t, set, &a, &b);
	k->n = 0;
	if (rn_py_casefold(t->s + a, b - a, k) < 0)
		nx_fail();
}

static int has_any(const rn_tok *t, const char32_t *set)
{
	for (size_t i = 0; i < t->n; i++)
		if (nx_inset(t->s[i], set))
			return 1;
	return 0;
}

static const char32_t *const CONJUNCTIONS[] = {U"и", U"или", U"ни", U"а", U"но", U"да",
                                               U"либо", U"же", NULL};
static const char32_t *const COORDINATORS[] = {U"и", U"или", U"ни", NULL};
static const char32_t *const M_ENDING[] = {U"м", U"ом", U"ем", U"ым", U"им", NULL};
static const char32_t *const BARE_ORDINAL[] = {U"го", U"ого", U"е", U"ое", NULL};
static const char *const MONTHS[] = {"январь", "февраль", "март",    "апрель",
                                     "май",    "июнь",    "июль",    "август",
                                     "сентябрь", "октябрь", "ноябрь", "декабрь", NULL};

static int ordinal_only(const uint32_t *s, size_t n)
{
	return rn_n_is_ordinal_suffix(s, n) && !nx_eq(s, n, U"ми");
}

/* ---- candidates ---- */

typedef struct {
	rn_str r;
	char kind;   /* 'c'ardinal, 'o'rdinal */
	int gcase, gender, plural, animate, exact;
} cand;

typedef struct {
	cand *v;
	size_t n;
} cands;

static void cands_free(cands *c)
{
	for (size_t i = 0; i < c->n; i++)
		rn_str_free(&c->v[i].r);
	free(c->v);
	c->v = NULL;
	c->n = 0;
}

static int rendered(const rn_str *r)
{
	if (!r->n)
		return 0;
	for (size_t i = 0; i < r->n; i++)
		if (rn_py_isdecimal(r->s[i]))
			return 0;
	return 1;
}

/* _last_word(reading).endswith(suffix) with suffix casefolded */
static int last_ends(const rn_str *r, const rn_str *suf)
{
	rn_str f = {0};
	if (rn_py_casefold(r->s, r->n, &f) < 0)
		nx_fail();
	size_t b = f.n;
	while (b > 0 && rn_py_isspace(f.s[b - 1]))
		b--;
	size_t a = b;
	while (a > 0 && !rn_py_isspace(f.s[a - 1]))
		a--;
	int ok = suf->n <= b - a && !memcmp(f.s + b - suf->n, suf->s, suf->n * 4);
	rn_str_free(&f);
	return ok;
}

static void add(cands *c, rn_str *r, char kind, int gcase, int gender, int plural, int animate,
                const rn_str *suf)
{
	if (!rendered(r)) {
		rn_str_free(r);
		return;
	}
	cand *v = realloc(c->v, (c->n + 1) * sizeof *v);
	if (!v) {
		nx_fail();
		rn_str_free(r);
		return;
	}
	c->v = v;
	v[c->n++] = (cand){*r, kind, gcase, gender, plural, animate, last_ends(r, suf)};
	*r = (rn_str){0};
}

/* generate_numeric_suffix_candidates(int(num), suffix): num is str(int) */
static void generate(cands *c, const char *num, const uint32_t *suffix, size_t sn)
{
	rn_str suf = {0};
	if (rn_py_casefold(suffix, sn, &suf) < 0)
		nx_fail();
	const int G[4] = {-1, rn_g.masc, rn_g.femn, rn_g.neut};
	for (int ci = 0; ci < 6; ci++) {
		int k = CASES[ci];
		for (int g = 0; g < 4; g++)
			for (int a = -1; a <= 1; a++) {
				rn_str r = {0};
				nx_numeral_a(&r, num, k, G[g], a);
				add(c, &r, 'c', k, G[g], 0, a, &suf);
			}
		for (int g = 1; g < 4; g++)
			for (int inan = 0; inan <= 1; inan++) {
				rn_str r = {0};
				nx_ordinal(&r, num, k, G[g], 0, inan);
				add(c, &r, 'o', k, G[g], 0, !inan, &suf);
			}
		for (int inan = 0; inan <= 1; inan++) {
			rn_str r = {0};
			nx_ordinal(&r, num, k, -1, 1, inan);
			add(c, &r, 'o', k, -1, 1, !inan, &suf);
		}
	}
	rn_str_free(&suf);
}

/* ---- noun features ---- */

typedef struct {
	int gcase, gender, plural, animate;
	const char *lemma;
} feat;

#define MAXF 64
typedef struct {
	feat v[MAXF];
	size_t n;
} feats;

static int is_name(uint16_t tag)
{
	return rn_has(tag, rn_g.Name) || rn_has(tag, rn_g.Surn) || rn_has(tag, rn_g.Patr);
}

static void noun_features(feats *out, const rn_tok *t, size_t tn, int reverse, int names_only,
                          int names_excluded)
{
	out->n = 0;
	size_t cnt = tn < 4 ? tn : 4;
	rn_str k = {0};
	for (size_t w = 0; w < cnt; w++) {
		const rn_tok *tok = reverse ? &t[tn - 1 - w] : &t[w];
		if (has_any(tok, U".!?…"))
			break;
		size_t a, b;
		strip_set(tok, U".,!?;:«»\"'()[]{}", &a, &b);
		if (a == b)
			continue;
		k.n = 0;
		if (rn_py_casefold(tok->s + a, b - a, &k) < 0)
			nx_fail();
		if (rn_in32(PREP_CASE, k.s, k.n) || nx_oneof(k.s, k.n, CONJUNCTIONS))
			continue;
		const cmorphy3_form *f;
		int np = rn_mparse(tok->s + a, b - a, &f);
		int p0 = rn_pos(f[0].tag);
		if (p0 == rn_g.VERB || p0 == rn_g.INFN || p0 == rn_g.PRTS || p0 == rn_g.GRND) {
			int known_noun = 0;
			for (int i = 0; i < np && !known_noun; i++)
				known_noun = rn_has(f[i].tag, rn_g.NOUN) &&
				             cmorphy3_word_is_known(rn_morph, f[i].word);
			if (!known_noun)
				break;
		}
		int any = 0;
		for (int i = 0; i < np; i++) {
			uint16_t tg = f[i].tag;
			if (!rn_has(tg, rn_g.NOUN) || (names_only && !is_name(tg)) ||
			    (names_excluded && is_name(tg)))
				continue;
			any = 1;
			if (out->n < MAXF) {
				int an = rn_animacy(tg);
				out->v[out->n++] = (feat){rn_canon_case(rn_case(tg)), rn_gender(tg),
				                          rn_has(tg, rn_g.plur),
				                          an == rn_g.anim ? 1 : an == rn_g.inan ? 0 : -1,
				                          f[i].normal_form};
			}
		}
		if (any)
			break;
	}
	rn_str_free(&k);
}

static int has_copula_before(const rn_tok *t, size_t tn)
{
	size_t lo = tn > 6 ? tn - 6 : 0;
	for (size_t i = tn; i-- > lo;) {
		size_t a, b;
		strip_set(&t[i], PUNCT, &a, &b);
		if (a == b)
			continue;
		if (has_any(&t[i], U".!?;:"))
			break;
		nx_selq q = NX_SELQ;
		q.pos[0] = rn_g.VERB;
		q.pos[1] = rn_g.INFN;
		const cmorphy3_form *f = nx_select_w(t[i].s + a, b - a, &q);
		if (f && !strcmp(f->normal_form, "быть"))
			return 1;
	}
	return 0;
}

static int cardinal_government(unsigned v100, const char *num, const cand *c, const feat *n)
{
	if (c->kind != 'c' || (c->gcase != rn_g.nomn && c->gcase != rn_g.accs))
		return 0;
	unsigned d = v100 % 10;
	int ok;
	if (c->gcase == rn_g.accs && c->animate == 1)
		ok = (n->gcase == rn_g.accs || n->gcase == rn_g.gent) &&
		     n->plural == (strcmp(num, "1") != 0);
	else if (d == 1 && v100 != 11)
		ok = !n->plural && (n->gcase == rn_g.nomn || n->gcase == rn_g.accs || n->gcase == rn_g.gent);
	else if (d >= 2 && d <= 4 && !(v100 >= 12 && v100 <= 14))
		ok = !n->plural && n->gcase == rn_g.gent;
	else
		ok = n->plural && n->gcase == rn_g.gent;
	return ok ? 4 : -4;
}

static int ordinal_penalty(const cand *c, const feat *n)
{
	if (c->kind != 'o')
		return 0;
	if (c->plural != n->plural)
		return -3;
	if (!c->plural && c->gender >= 0 && n->gender >= 0 && c->gender != n->gender)
		return -2;
	return 0;
}

static int candidate_score(const cand *c, unsigned v100, const char *num, int ctx_case,
                           int ctx_explicit, const feats *nf)
{
	int score = c->gcase == ctx_case ? (ctx_explicit ? 8 : 2) : 0;
	int best = 0;
	for (size_t i = 0; i < nf->n; i++) {
		const feat *n = &nf->v[i];
		int s = c->gcase == n->gcase ? 5 : 0;
		s += cardinal_government(v100, num, c, n);
		s += ordinal_penalty(c, n);
		if (c->gcase == rn_g.accs && n->animate >= 0 && c->animate == n->animate)
			s += 1;
		if (s > best)
			best = s;
	}
	return score + best;
}

static int agreement_score(const cand *c, const feats *nf)
{
	if (c->kind != 'o')
		return 0;
	int best = 0;
	for (size_t i = 0; i < nf->n; i++) {
		int s = c->plural == nf->v[i].plural ? 3 : 0;
		if (!c->plural && c->gender == nf->v[i].gender)
			s += 2;
		if (s > best)
			best = s;
	}
	return best;
}

static int month_follows(const rn_tok *t, size_t tn)
{
	for (size_t i = 0; i < tn && i < 2; i++) {
		size_t a, b;
		strip_set(&t[i], PUNCT, &a, &b);
		if (a == b)
			continue;
		nx_selq q = NX_SELQ;
		q.pos[0] = rn_g.NOUN;
		const cmorphy3_form *f = nx_select_w(t[i].s + a, b - a, &q);
		if (!f)
			return 0;
		for (const char *const *m = MONTHS; *m; m++)
			if (!strcmp(f->normal_form, *m))
				return 1;
		return 0;
	}
	return 0;
}

/* the sibling's number across a coordinator, as str(int): 1 and *num (malloc'ed) */
static int sibling(const rn_tok *t, size_t a, size_t b, const rn_str *suf, char **num)
{
	rn_str j = {0};
	for (size_t i = a; i < b; i++) {
		size_t x, y;
		strip_set(&t[i], PUNCT, &x, &y);
		nx_put(&j, t[i].s + x, y - x);
	}
	if (!j.s)
		nx_put(&j, U"", 0);
	int ok = 0;
	rn_match m;
	const rn_re *p = P(SUFFIXED_NUMBER_TOKEN);
	int r = rn_match_at(p, j.s, j.n, 0, &m);
	if (r < 0)
		nx_fail();
	if (r > 0) {
		size_t sn, nn;
		const uint32_t *s = nx_grp(&m, p, "suffix", &sn);
		const uint32_t *n = nx_grp(&m, p, "number", &nn);
		rn_str f = {0};
		rn_py_casefold(s, sn, &f);
		if (f.n == suf->n && !memcmp(f.s, suf->s, f.n * 4)) {
			*num = malloc(nn + 3);
			if (*num && nx_pyint(n, nn, *num, nn + 3) >= 0)
				ok = 1;
			else {
				free(*num);
				*num = NULL;
			}
		}
		rn_str_free(&f);
	}
	rn_str_free(&j);
	return ok;
}

/* _kind_forced_by_coordination: 'c', 'o' or 0 */
static char kind_forced(const uint32_t *suffix, size_t sn, int ctx_case, const rn_tok *L,
                        size_t ln, const rn_tok *R, size_t rn)
{
	rn_str suf = {0}, k = {0};
	rn_py_casefold(suffix, sn, &suf);
	size_t ranges[2][2];
	int nr = 0;
	if (rn) {
		clean_fold(&R[0], PUNCT, &k);
		if (nx_oneof(k.s, k.n, COORDINATORS)) {
			ranges[nr][0] = 1;
			ranges[nr][1] = rn < 4 ? rn : 4;
			nr++;
		}
	}
	const rn_tok *srcs[2];
	srcs[0] = nr ? R : NULL;
	if (ln) {
		clean_fold(&L[ln - 1], PUNCT, &k);
		if (nx_oneof(k.s, k.n, COORDINATORS)) {
			ranges[nr][0] = ln > 4 ? ln - 4 : 0;
			ranges[nr][1] = ln - 1;
			srcs[nr] = L;
			nr++;
		}
	}
	char res = 0;
	for (int i = 0; i < nr && !res; i++) {
		char *num = NULL;
		if (ranges[i][0] > ranges[i][1] ||
		    !sibling(srcs[i], ranges[i][0], ranges[i][1], &suf, &num))
			continue;
		cands c = {0};
		generate(&c, num, suffix, sn);
		int kinds = 0;
		for (size_t j = 0; j < c.n; j++)
			if (c.v[j].exact && c.v[j].gcase == ctx_case)
				kinds |= c.v[j].kind == 'c' ? 1 : 2;
		if (kinds == 1)
			res = 'c';
		else if (kinds == 2)
			res = 'o';
		cands_free(&c);
		free(num);
	}
	rn_str_free(&suf);
	rn_str_free(&k);
	return res;
}

static int plural_modifier_left(const rn_tok *t, size_t tn)
{
	size_t lo = tn > 4 ? tn - 4 : 0;
	rn_str k = {0};
	int r = 0;
	for (size_t i = tn; i-- > lo;) {
		size_t a, b;
		strip_set(&t[i], PUNCT, &a, &b);
		if (a == b || rn_py_str_isdigit(t[i].s + a, b - a))
			continue;
		clean_fold(&t[i], PUNCT, &k);
		if (rn_in32(PREP_CASE, k.s, k.n) || rn_n_is_numeric_suffix(t[i].s + a, b - a))
			continue;
		const cmorphy3_form *f;
		int np = rn_mparse(t[i].s + a, b - a, &f);
		for (int j = 0; j < np && !r; j++) {
			int pos = rn_pos(f[j].tag);
			r = rn_has(f[j].tag, rn_g.plur) && (pos == rn_g.ADJF || pos == rn_g.PRTF);
		}
		break;
	}
	rn_str_free(&k);
	return r;
}

static int is_unit_lemma(const char *lemma)
{
	rn_str u = {0};
	nx_put8(&u, lemma);
	int r = rn_unit_resolve(u.s ? u.s : (const uint32_t *)U"", u.n) != NULL;
	rn_str_free(&u);
	return r;
}

static int sort_less(const cand *a, const cand *b)
{
	int ka = a->kind == 'o', kb = b->kind == 'o';
	if (ka != kb)
		return ka < kb;
	int ca = case_order(a->gcase), cb = case_order(b->gcase);
	if (ca != cb)
		return ca < cb;
	int ga = a->gender >= 0, gb = b->gender >= 0;
	if (ga != gb)
		return ga < gb;
	size_t n = a->r.n < b->r.n ? a->r.n : b->r.n;
	for (size_t i = 0; i < n; i++)
		if (a->r.s[i] != b->r.s[i])
			return a->r.s[i] < b->r.s[i];
	return a->r.n < b->r.n;
}

/* resolve_numeric_suffix: the reading appended */
static void resolve(rn_str *o, const uint32_t *nt, size_t nn, const uint32_t *suffix, size_t sn,
                    const rn_tok *L, size_t ln, const rn_tok *R, size_t rn)
{
	char *num = malloc(nn + 3);
	if (!num || nx_pyint(nt, nn, num, nn + 3) < 0) {
		free(num);
		nx_put(o, nt, nn);
		return;
	}
	rn_tok *all = malloc((ln + rn + 1) * sizeof *all);
	if (!all) {
		nx_fail();
		free(num);
		return;
	}
	if (ln)
		memcpy(all, L, ln * sizeof *all);
	all[ln] = (rn_tok){nt, nn};
	if (rn)
		memcpy(all + ln + 1, R, rn * sizeof *all);
	rn_ncase *nc = rn_ncase_new(all, ln + rn + 1);
	int ctx_explicit = 0;
	int ctx_case = nc ? rn_ncase_get(nc, ln, SIZE_MAX, &ctx_explicit) : -1;
	rn_ncase_free(nc);
	free(all);
	if (ctx_case < 0) {
		nx_fail();
		ctx_case = rn_g.nomn;
	}
	rn_str suf = {0}, k = {0};
	rn_py_casefold(suffix, sn, &suf);
	int m_end = nx_oneof(suf.s, suf.n, M_ENDING);
	if (m_end && has_copula_before(L, ln)) {
		ctx_case = rn_g.ablt;
		ctx_explicit = 1;
	}
	feats rf, lf;
	if (ordinal_only(suf.s, suf.n) && ctx_case == rn_g.accs && ln) {
		k.n = 0;
		rn_str lw = {0};
		nx_lower(L[ln - 1].s, L[ln - 1].n, &lw);
		rn_tok lt = {lw.s, lw.n};
		clean_fold(&lt, PUNCT, &k);
		rn_str_free(&lw);
		int prep = nx_eq(k.s, k.n, U"в") || nx_eq(k.s, k.n, U"во") ||
		           (nx_eq(k.s, k.n, U"на") && m_end);
		if (prep) {
			noun_features(&rf, R, rn, 0, 0, 0);
			if (!rf.n) {
				ctx_case = rn_g.loct;
				ctx_explicit = 1;
			}
		}
	}
	noun_features(&rf, R, rn, 0, 0, 1);
	noun_features(&lf, L, ln, 1, 1, 0);
	const feats *nf = rf.n ? &rf : &lf;
	if (!rf.n && lf.n) {
		ctx_case = lf.v[0].gcase;
		ctx_explicit = 1;
	}
	cands all_c = {0};
	generate(&all_c, num, suffix, sn);
	size_t ne = 0;
	for (size_t i = 0; i < all_c.n; i++)
		if (all_c.v[i].exact)
			ne++;
	if (!ne) {
		/* _fallback_cardinal */
		const feat *n = NULL;
		for (size_t i = 0; i < nf->n && !n; i++)
			if (nf->v[i].gcase == ctx_case)
				n = &nf->v[i];
		if (!n && nf->n)
			n = &nf->v[0];
		nx_numeral(o, nt, nn, ctx_case, n && !n->plural ? n->gender : -1,
		           n && ctx_case == rn_g.accs ? n->animate : -1);
		goto out;
	}
	unsigned v100 = nx_dmod(num, 100);
	const cand **best = malloc(ne * sizeof *best);
	int *sc = malloc(ne * sizeof *sc);
	if (!best || !sc) {
		nx_fail();
		free(best);
		free(sc);
		goto out;
	}
	size_t nb = 0;
	int top = 0;
	for (size_t i = 0; i < all_c.n; i++) {
		if (!all_c.v[i].exact)
			continue;
		int s = candidate_score(&all_c.v[i], v100, num, ctx_case, ctx_explicit, nf);
		if (!nb || s > top)
			top = s;
		sc[nb] = s;
		best[nb++] = &all_c.v[i];
	}
	size_t w = 0;
	for (size_t i = 0; i < nb; i++)
		if (sc[i] == top)
			best[w++] = best[i];
	nb = w;
#define KINDS(mask)                                                                            \
	do {                                                                                       \
		mask = 0;                                                                              \
		for (size_t i_ = 0; i_ < nb; i_++)                                                     \
			mask |= best[i_]->kind == 'c' ? 1 : 2;                                             \
	} while (0)
#define KEEP(cond)                                                                             \
	do {                                                                                       \
		size_t w_ = 0;                                                                         \
		for (size_t i_ = 0; i_ < nb; i_++) {                                                   \
			const cand *c = best[i_];                                                          \
			if (cond)                                                                          \
				best[w_++] = c;                                                                \
		}                                                                                      \
		nb = w_;                                                                               \
	} while (0)
	int kinds;
	KINDS(kinds);
	if (kinds == 3) {
		char fk = kind_forced(suffix, sn, ctx_case, L, ln, R, rn);
		if (fk)
			KEEP(c->kind == fk);
	}
	KINDS(kinds);
	if ((kinds & 2) && month_follows(R, rn))
		KEEP(c->kind == 'o');
	else if (!nf->n && nx_oneof(suf.s, suf.n, BARE_ORDINAL) && (kinds & 2))
		KEEP(c->kind == 'o');
	else if (nf->n && ordinal_only(suf.s, suf.n) && (kinds & 2) && ({
		         int unit = 0;
		         for (size_t i = 0; i < nf->n && !unit; i++)
			         unit = is_unit_lemma(nf->v[i].lemma) && (nf->v[i].plural || !m_end);
		         !unit;
	         }))
		KEEP(c->kind == 'o');
	else if (kinds & 1)
		KEEP(c->kind == 'c');
	if (!nf->n && !plural_modifier_left(L, ln)) {
		int anysing = 0;
		for (size_t i = 0; i < nb; i++)
			anysing |= !best[i]->plural;
		if (anysing)
			KEEP(!c->plural);
	}
	int ba = 0;
	for (size_t i = 0; i < nb; i++) {
		int a = agreement_score(best[i], nf);
		if (!i || a > ba)
			ba = a;
	}
	KEEP(agreement_score(c, nf) == ba);
	const cand *pick = best[0];
	for (size_t i = 1; i < nb; i++)
		if (sort_less(best[i], pick))
			pick = best[i];
	/* one distinct reading: that one (min() picks it too) */
	nx_put(o, pick->r.s, pick->r.n);
	free(best);
	free(sc);
out:
	cands_free(&all_c);
	rn_str_free(&suf);
	rn_str_free(&k);
	free(num);
}

/* ---- _spread_ordinal_over_enumeration ---- */

static int enum_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(ORDINAL_ENUMERATION_PATTERN);
	size_t sn, nn;
	const uint32_t *s = nx_grp(m, p, "suffix", &sn);
	rn_str suf = {0};
	rn_py_casefold(s, sn, &suf);
	int ok = ordinal_only(suf.s, suf.n);
	rn_str_free(&suf);
	if (!ok)
		return nx_whole(m, o);
	const uint32_t *noun = nx_grp(m, p, "noun", &nn);
	rn_str nf = {0};
	rn_py_casefold(noun, nn, &nf);
	const cmorphy3_form *f = rn_mfirst(nf.s, nf.n);
	rn_str_free(&nf);
	int c0 = rn_case(f->tag);
	if (!rn_has(f->tag, rn_g.NOUN) || c0 < 0)
		return nx_whole(m, o);
	int ncase = rn_has(f->tag, rn_g.loc2) ? rn_g.loct : c0;
	int gcase;
	if (ncase == rn_g.datv || ncase == rn_g.ablt || ncase == rn_g.loct)
		gcase = ncase;
	else {
		const uint32_t *T = m->s;
		size_t st = m->start[0], ne = nx_gend(m, p, "noun"), a = nx_left(st, 60);
		size_t re = ne + 60 < m->n ? ne + 60 : m->n;
		rn_tok *L = NULL, *B = NULL, *R = NULL;
		long ln = rn_simple_tokenize(T + a, st - a, &L);
		long bn = rn_simple_tokenize(T + st, m->end[0] - st, &B);
		long rn = rn_simple_tokenize(T + ne, re - ne, &R);
		gcase = rn_g.nomn;
		if (ln < 0 || bn < 0 || rn < 0)
			nx_fail();
		else {
			rn_tok *all = malloc((size_t)(ln + bn + rn + 1) * sizeof *all);
			if (!all)
				nx_fail();
			else {
				memcpy(all, L, (size_t)ln * sizeof *all);
				memcpy(all + ln, B, (size_t)bn * sizeof *all);
				memcpy(all + ln + bn, R, (size_t)rn * sizeof *all);
				rn_ncase *nc = rn_ncase_new(all, (size_t)(ln + bn + rn));
				int g = nc ? rn_ncase_get(nc, (size_t)ln, SIZE_MAX, NULL) : -1;
				rn_ncase_free(nc);
				if (g < 0)
					nx_fail();
				else
					gcase = g;
				free(all);
			}
		}
		free(L);
		free(B);
		free(R);
	}
	int gender = rn_gender(f->tag), inan = !rn_has(f->tag, rn_g.anim);
	size_t hn;
	const uint32_t *h = nx_grp(m, p, "head", &hn);
	for (size_t i = 0; i < hn;) {
		size_t j = i;
		int sep = h[i] == ',' || rn_py_isspace(h[i]);
		while (j < hn && (h[j] == ',' || rn_py_isspace(h[j])) == sep)
			j++;
		if (sep)
			nx_put(o, h + i, j - i);
		else {
			char d[64], *dp = d;
			if (j - i + 3 > sizeof d)
				dp = malloc(j - i + 3);
			if (dp && nx_pyint(h + i, j - i, dp, j - i + 3) >= 0)
				nx_ordinal(o, dp, gcase, gender, 0, inan);
			else
				nx_fail();
			if (dp != d)
				free(dp);
		}
		i = j;
	}
	nx_putg(o, m, p, "conj");
	size_t ltn;
	const uint32_t *lt = nx_grp(m, p, "last", &ltn);
	char *ld = malloc(ltn + 3);
	rn_str last = {0};
	if (ld && nx_pyint(lt, ltn, ld, ltn + 3) >= 0)
		nx_ordinal(&last, ld, gcase, gender, 0, inan);
	else
		nx_fail();
	free(ld);
	if (nx_grp(m, p, "hard", &(size_t){0}) && last.n &&
	    !nx_inset(last.s[last.n - 1], U"аеёиоуыэюяьъ"))
		nx_putc(&last, U'ъ');
	nx_put(o, last.s, last.n);
	rn_str_free(&last);
	nx_putg(o, m, p, "gap");
	nx_put(o, noun, nn);
	return 0;
}

/* ---- normalize_numeric_suffixes ---- */

typedef struct {
	size_t text_start_end;
	int caps;
} sctx;

static int suffix_repl(const rn_match *m, rn_str *o, void *ctx)
{
	const sctx *c = ctx;
	const rn_re *p = P(NUMERIC_SUFFIX_PATTERN);
	const uint32_t *T = m->s;
	size_t nn, sn, st = m->start[0], en = m->end[0];
	const uint32_t *nt = nx_grp(m, p, "number", &nn);
	const uint32_t *s = nx_grp(m, p, "suffix", &sn);
	rn_str suf = {0};
	rn_py_casefold(s, sn, &suf);
	size_t a = nx_left(st, 60), b = en + 60 < m->n ? en + 60 : m->n;
	rn_tok *L = NULL, *R = NULL;
	long ln = rn_simple_tokenize(T + a, st - a, &L);
	long rn = rn_simple_tokenize(T + en, b - en, &R);
	rn_str r = {0};
	if (ln < 0 || rn < 0) {
		nx_fail();
		nx_whole(m, o);
		goto out;
	}
	resolve(&r, nt, nn, suf.s ? suf.s : (const uint32_t *)U"", suf.n, L, (size_t)ln, R,
	        (size_t)rn);
	if (nx_grp(m, p, "hard", &(size_t){0}) && r.n && !nx_inset(r.s[r.n - 1], U"аеёиоуыэюяьъ"))
		nx_putc(&r, U'ъ');
	if (nx_grp(m, p, "sign", &(size_t){0})) {
		rn_str x = {0};
		nx_lit(&x, U"минус ");
		nx_put(&x, r.s, r.n);
		rn_str_free(&r);
		r = x;
	}
	if (c->caps && st <= c->text_start_end && r.n && rn_py_isalpha(r.s[0])) {
		rn_str u = {0};
		rn_py_upper(r.s, 1, &u);
		nx_put(o, u.s, u.n);
		nx_put(o, r.s + 1, r.n - 1);
		rn_str_free(&u);
	} else
		nx_put(o, r.s, r.n);
out:
	free(L);
	free(R);
	rn_str_free(&r);
	rn_str_free(&suf);
	return 0;
}

static int ob_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	nx_putc(o, rn_py_isupper(m->s[m->start[0]]) ? U'О' : U'о');
	return 0;
}

int rn_n_numeric_suffixes_ex(rn_str *t, int caps)
{
	init_cases();
	nx_resub(P(ORDINAL_ENUMERATION_PATTERN), t, enum_repl, NULL);
	sctx c = {0, caps};
	while (c.text_start_end < t->n && nx_inset(t->s[c.text_start_end], U" \t\r\n\"'«„“([{"))
		c.text_start_end++;
	rn_str before = {0};
	nx_put(&before, t->s, t->n);
	nx_resub(P(NUMERIC_SUFFIX_PATTERN), t, suffix_repl, &c);
	if (t->n != before.n || (t->n && memcmp(t->s, before.s, t->n * 4)))
		nx_resub(P(OB_BEFORE_CONSONANT_PATTERN), t, ob_repl, NULL);
	rn_str_free(&before);
	return rn_mx_failed ? -1 : 0;
}

int rn_n_numeric_suffixes(rn_str *t)
{
	return rn_n_numeric_suffixes_ex(t, 1);
}
