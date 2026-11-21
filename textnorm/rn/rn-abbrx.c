/* abbreviations.py expand_abbreviations with the tts() options: language origin, geographic
 * classifiers, declinable nouns, "т. д.", adjectives, the literal rules, person initials,
 * letter abbreviations and "&" */
#include <pthread.h>
#include <stdio.h>

#include "rn-num.h"
#include "rn-nx.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-re.h"
#include "rn-tables.h"

#define P(id) rn_pat(RN_ABBREVIATIONS_##id)
#define PUNCT_STRIP U".,:;!\"«»()[]{}"

static int gram(int *cache, const char *name)
{
	if (*cache == -2)
		*cache = nx_gram(name);
	return *cache;
}
static int g_Subx = -2, g_PNCT = -2;
#define SUBX gram(&g_Subx, "Subx")
#define PNCT gram(&g_PNCT, "PNCT")

/* ---- strings ---- */

/* s with s[i] uppercased appended (s[:i] + s[i].upper() + s[i+1:]) */
static void put_upper_at(rn_str *o, const uint32_t *s, size_t n, size_t i)
{
	if (i >= n) {
		nx_put(o, s, n);
		return;
	}
	nx_put(o, s, i);
	if (rn_py_upper(s + i, 1, o) < 0)
		nx_fail();
	nx_put(o, s + i + 1, n - i - 1);
}

/* the index of the first isalpha() character, n if none */
static size_t first_alpha(const uint32_t *s, size_t n)
{
	size_t i = 0;
	while (i < n && !rn_py_isalpha(s[i]))
		i++;
	return i;
}

/* t = s.lower().strip(PUNCT_STRIP) */
static void lower_strip(const uint32_t *s, size_t n, rn_str *t)
{
	nx_lower(s, n, t);
	size_t a = 0, b = t->n;
	while (a < b && nx_inset(t->s[a], PUNCT_STRIP))
		a++;
	while (b > a && nx_inset(t->s[b - 1], PUNCT_STRIP))
		b--;
	memmove(t->s, t->s + a, (b - a) * sizeof *t->s);
	t->n = b - a;
}

static const rn_kv *get_lower(enum rn_table t, const uint32_t *s, size_t n)
{
	rn_str l = {0};
	nx_lower(s, n, &l);
	const rn_kv *e = rn_table_get32(t, l.s ? l.s : (const uint32_t *)U"", l.n);
	rn_str_free(&l);
	return e;
}

static int is_cyr(uint32_t c)
{
	return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451;
}

static int is_ascii_letter(uint32_t c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int isupper_at(const uint32_t *T, size_t n, size_t i)
{
	return i < n && rn_py_isupper(T[i]);
}

/* the last k tokens of _LEFT_CONTEXT_TOKEN_PATTERN.findall(T[a:b]) into tok, in order;
 * their count */
static int left_tokens(const uint32_t *T, size_t a, size_t b, rn_tok *tok, int k)
{
	rn_iter it = rn_iter_init(P(LEFT_CONTEXT_TOKEN_PATTERN), T + a, b - a, 0);
	rn_match m;
	int n = 0, r;
	while ((r = rn_iter_next(&it, &m)) == 1) {
		if (n == k) {
			memmove(tok, tok + 1, (k - 1) * sizeof *tok);
			n--;
		}
		tok[n++] = (rn_tok){T + a + m.start[0], m.end[0] - m.start[0]};
	}
	if (r < 0)
		nx_fail();
	return n;
}

static int word_known8(const char *w)
{
	return cmorphy3_word_is_known(rn_morph, w);
}

/* the first parse of the UTF-8 lemma with POS ADJF/PRTF (or_first: else parses[0]) */
static const cmorphy3_form *adj_parse8(const char *lemma, int or_first)
{
	const cmorphy3_form *f;
	int np = rn_mparse8(lemma, &f);
	for (int i = 0; i < np; i++) {
		int p = rn_pos(f[i].tag);
		if (p == rn_g.ADJF || p == rn_g.PRTF)
			return &f[i];
	}
	return or_first && np ? &f[0] : NULL;
}

/* next(p for p in parse_word(lemma) if "NOUN" in p.tag and p.tag.case == "nomn") */
static const cmorphy3_form *noun_nomn8(const char *lemma)
{
	const cmorphy3_form *f;
	int np = rn_mparse8(lemma, &f);
	for (int i = 0; i < np; i++)
		if (rn_has(f[i].tag, rn_g.NOUN) && rn_case(f[i].tag) == rn_g.nomn)
			return &f[i];
	return NULL;
}

static cmorphy3_gset gs2(int a, int b)
{
	return rn_gset_or(rn_gset1(a), rn_gset1(b));
}

/* ---- _expand_language_origin_abbreviations ---- */

static int lang_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(LANGUAGE_ORIGIN_PATTERN);
	size_t wn, an;
	const uint32_t *w = nx_grp(m, p, "word", &wn);
	int alpha = 0, cyr = 0;
	for (size_t i = 0; i < wn; i++)
		if (rn_py_isalpha(w[i])) {
			alpha = 1;
			cyr |= is_cyr(w[i]);
		}
	if (!alpha || cyr)
		return nx_whole(m, o);
	const uint32_t *a = nx_grp(m, p, "abbr", &an);
	const rn_kv *e = get_lower(RN_T_ADJECTIVE_ABBREVIATION_EXPANSIONS, a, an);
	if (!e) {
		nx_fail();
		return nx_whole(m, o);
	}
	nx_putg(o, m, p, "prep");
	nx_putc(o, ' ');
	const cmorphy3_form *f = adj_parse8(e->value, 0);
	if (f)
		nx_safe_inflect(o, f, rn_gset_or(gs2(rn_g.gent, rn_g.sing), rn_gset1(rn_g.masc)),
		                e->value);
	else
		nx_put8(o, e->value);
	nx_putg(o, m, p, "space");
	nx_put(o, w, wn);
	return 0;
}

/* ---- _expand_geo_classifier_abbreviations ---- */

static int prep_case(const rn_str *w)
{
	const rn_kv *e = rn_table_get32(RN_T_PREP_CASE, w->s, w->n);
	return e ? nx_gram(e->value) : -1;
}

static int motion_verb_precedes(const rn_tok *L, long n)
{
	size_t ns;
	const rn_kv *suf = rn_motion_suffixes(&ns);
	rn_str w = {0};
	int r = 0;
	for (long i = n - 1; i >= 0 && i >= n - 4 && !r; i--) {
		lower_strip(L[i].s, L[i].n, &w);
		if (!w.n)
			continue;
		const cmorphy3_form *f = rn_mfirst(w.s, w.n);
		int p = rn_pos(f->tag);
		if (p != rn_g.VERB && p != rn_g.INFN && p != rn_g.GRND && p != rn_g.PRTF &&
		    p != rn_g.PRTS)
			continue;
		size_t l = strlen(f->normal_form);
		for (size_t k = 0; k < ns && !r; k++) {
			size_t sl = strlen(suf[k].key);
			r = sl <= l && !memcmp(f->normal_form + l - sl, suf[k].key, sl);
		}
		if (!r && (rn_fixes & RN_FIX_GEO_MOTION)) {
			static const char ez[] = "ездить";
			r = l >= sizeof ez - 1 && !strcmp(f->normal_form + l - (sizeof ez - 1), ez);
		}
	}
	rn_str_free(&w);
	return r;
}

/* the reading of a classifier abbr at T[st] before name, appended with its space; 0 if it
 * stays. name_required 2: R21, the case also from the name */
static int geo_expand(rn_str *o, const uint32_t *T, size_t st, int upper, const char *lemma,
                      int name_required, const uint32_t *name, size_t nn)
{
	size_t a = nx_left(st, 40);
	rn_tok *L = NULL;
	long n = rn_simple_tokenize(T + a, st - a, &L);
	if (n < 0) {
		nx_fail();
		n = 0;
	}
	int gcase = rn_g.nomn;
	rn_str prev = {0}, w = {0};
	if (n)
		lower_strip(L[n - 1].s, L[n - 1].n, &prev);
	if (prev.n) {
		static const char32_t *const STATIC[][2] = {
		    {U"в", U"loct"},   {U"во", U"loct"},  {U"на", U"loct"},    {U"за", U"ablt"},
		    {U"под", U"ablt"}, {U"над", U"ablt"}, {U"перед", U"ablt"},
		};
		size_t k = 0;
		while (k < sizeof STATIC / sizeof *STATIC && !nx_eq(prev.s, prev.n, STATIC[k][0]))
			k++;
		if (k < sizeof STATIC / sizeof *STATIC) {
			gcase = STATIC[k][1][1] == 'o' ? rn_g.loct : rn_g.ablt;
			if (k < 3 && motion_verb_precedes(L, n - 1))
				gcase = rn_g.accs;
		} else if (prep_case(&prev) >= 0) {
			gcase = prep_case(&prev);
		} else {
			const cmorphy3_form *f = rn_mfirst(prev.s, prev.n);
			if (rn_has(f->tag, rn_g.NOUN) && !rn_has(f->tag, rn_g.Fixd))
				gcase = rn_g.gent;
		}
	}
	int keep = 0;
	if (name_required) {
		/* _reads_as_place_name */
		int place = 0;
		if (rn_word_is_known(name, nn)) {
			const cmorphy3_form *f;
			int np = rn_mparse(name, nn, &f);
			for (int i = 0; i < np && !place; i++) {
				int c = rn_case(f[i].tag);
				place = rn_has(f[i].tag, rn_g.Geox) &&
				        (c == gcase || c == rn_g.nomn || c == rn_g.accs ||
				         name_required == 2);
			}
			if (name_required == 2) {
				/* R21: "в р. Неву" (direction, which "в"/"на" do not tell) and "через
				 * р. Березину" (no context case) take the accusative of the name, and
				 * without a context case the name's case is taken */
				int has_g = 0, has_accs = 0, has_nomn = 0, first = -1;
				for (int i = 0; i < np; i++) {
					if (!rn_has(f[i].tag, rn_g.Geox))
						continue;
					int c = rn_canon_case(rn_case(f[i].tag));
					has_g |= c == gcase;
					has_accs |= c == rn_g.accs;
					has_nomn |= c == rn_g.nomn;
					if (first < 0)
						first = c;
				}
				if (!has_g && has_accs && (gcase == rn_g.loct || gcase == rn_g.nomn))
					gcase = rn_g.accs;
				else if (!has_g && !has_nomn && gcase == rn_g.nomn && first >= 0)
					gcase = first;   /* "берегъ р. Одера" */
			}
		}
		keep = !place;
		for (long i = n - 1; i >= 0 && i >= n - 2 && !keep; i--) {
			size_t x = 0, y = L[i].n;
			while (x < y && nx_inset(L[i].s[x], PUNCT_STRIP))
				x++;
			while (y > x && nx_inset(L[i].s[y - 1], PUNCT_STRIP))
				y--;
			if (x == y)
				continue;
			lower_strip(L[i].s, L[i].n, &w);
			const cmorphy3_form *f = rn_mfirst(w.s ? w.s : (const uint32_t *)U"", w.n);
			keep = rn_in(RELIEF_CONTEXT_LEMMAS, f->normal_form);
		}
	}
	free(L);
	rn_str_free(&prev);
	rn_str_free(&w);
	if (keep)
		return 0;
	rn_str x = {0};
	const cmorphy3_form *pl = gcase != rn_g.nomn ? noun_nomn8(lemma) : NULL;
	if (pl)
		nx_safe_inflect(&x, pl, gs2(gcase, rn_g.sing), lemma);
	else
		nx_put8(&x, lemma);
	if (upper)
		put_upper_at(o, x.s, x.n, 0);
	else
		nx_put(o, x.s, x.n);
	rn_str_free(&x);
	nx_putc(o, ' ');
	return 1;
}

static int geo_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(GEO_CLASSIFIER_PATTERN);
	size_t an, nn;
	const uint32_t *abbr = nx_grp(m, p, "abbr", &an);
	const uint32_t *name = nx_grp(m, p, "name", &nn);
	const rn_kv *e = get_lower(RN_T_GEO_CLASSIFIER_ABBREVIATIONS, abbr, an);
	if (!e) {
		nx_fail();
		return nx_whole(m, o);
	}
	int req = get_lower(RN_T_GEO_NAME_REQUIRED_ABBREVIATIONS, abbr, an) != NULL;
	if (!geo_expand(o, m->s, m->start[0], an && rn_py_isupper(abbr[0]), e->value, req, name, nn))
		return nx_whole(m, o);
	return 0;
}

static int is_word_char(uint32_t c)
{
	return rn_py_isalnum(c) || c == '_';
}

/* R21: "р. Волга" (река) like the name-required "г.": (?<!\w)[рР]\.\s+(?=[А-ЯЁ][а-яё-]+) */
static void geo_river(rn_str *t)
{
	rn_str out = {0};
	size_t pos = 0;
	for (size_t i = 0; i + 1 < t->n; i++) {
		const uint32_t *T = t->s;
		if ((T[i] != U'р' && T[i] != U'Р') || T[i + 1] != '.' || (i && is_word_char(T[i - 1])))
			continue;
		size_t j = i + 2;
		while (j < t->n && rn_py_isspace(T[j]))
			j++;
		if (j == i + 2 || j >= t->n || !(is_cyr(T[j]) && rn_py_isupper(T[j])))
			continue;
		size_t k = j + 1;
		while (k < t->n && ((is_cyr(T[k]) && rn_py_islower(T[k])) || T[k] == '-'))
			k++;
		if (k == j + 1)
			continue;
		/* not "5 р." / "пять р." (рубль), which the numerals stage reads first */
		size_t b = i;
		while (b && rn_py_isspace(T[b - 1]))
			b--;
		if (b && rn_py_isdigit(T[b - 1]))
			continue;
		size_t a = b;
		while (a && rn_py_isalpha(T[a - 1]))
			a--;
		if (a < b) {
			const cmorphy3_form *f;
			int np = rn_mparse(T + a, b - a, &f), num = 0;
			for (int q = 0; q < np && !num; q++)
				num = rn_pos(f[q].tag) == rn_g.NUMR;
			if (num)
				continue;
		}
		rn_str r = {0};
		if (geo_expand(&r, T, i, T[i] == U'Р', "река", 2, T + j, k - j)) {
			nx_put(&out, T + pos, i - pos);
			nx_put(&out, r.s, r.n);
			pos = j;
		}
		rn_str_free(&r);
	}
	if (pos) {
		nx_put(&out, t->s + pos, t->n - pos);
		rn_str tmp = *t;
		*t = out;
		out = tmp;
	}
	rn_str_free(&out);
}

/* ---- _expand_declinable_noun_abbreviations ---- */

static int noun_abbr_case(const uint32_t *T, size_t st)
{
	size_t a = nx_left(st, 48);
	rn_tok *L = NULL;
	long n = rn_simple_tokenize(T + a, st - a, &L);
	if (n < 0) {
		nx_fail();
		n = 0;
	}
	static const char32_t *const STATIC[][2] = {
	    {U"в", U"loct"},  {U"во", U"loct"},  {U"на", U"loct"},  {U"о", U"loct"},
	    {U"об", U"loct"}, {U"при", U"loct"}, {U"за", U"ablt"},  {U"под", U"ablt"},
	    {U"над", U"ablt"}, {U"перед", U"ablt"},
	};
	int r = rn_g.nomn;
	rn_str w = {0};
	for (long i = n - 1; i >= 0 && i >= n - 3; i--) {
		lower_strip(L[i].s, L[i].n, &w);
		if (!w.n)
			continue;
		size_t k = 0;
		while (k < sizeof STATIC / sizeof *STATIC && !nx_eq(w.s, w.n, STATIC[k][0]))
			k++;
		if (k < sizeof STATIC / sizeof *STATIC) {
			r = STATIC[k][1][1] == 'o' ? rn_g.loct : rn_g.ablt;
			break;
		}
		int c = prep_case(&w);
		if (c >= 0) {
			r = c;
			break;
		}
		int pos = rn_pos(rn_mfirst(w.s, w.n)->tag);
		if (pos != rn_g.ADJF && pos != rn_g.PRTF)
			break;
	}
	rn_str_free(&w);
	free(L);
	return r;
}

static int noun_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(DECLINABLE_NOUN_ABBREVIATION_PATTERN);
	size_t an;
	const uint32_t *abbr = nx_grp(m, p, "abbr", &an);
	rn_str f = {0};
	if (rn_py_casefold(abbr, an, &f) < 0)
		nx_fail();
	const rn_kv *e = rn_table_get32(RN_T_DECLINABLE_NOUN_ABBREVIATIONS,
	                                f.s ? f.s : (const uint32_t *)U"", f.n);
	rn_str_free(&f);
	if (!e) {
		nx_fail();
		return nx_whole(m, o);
	}
	/* "lemma number" */
	char lemma[64];
	const char *sp = strchr(e->value, ' ');
	size_t ll = (size_t)(sp - e->value);
	if (!sp || ll >= sizeof lemma) {
		nx_fail();
		return nx_whole(m, o);
	}
	memcpy(lemma, e->value, ll);
	lemma[ll] = 0;
	int number = nx_gram(sp + 1);
	int gcase = noun_abbr_case(m->s, m->start[0]);
	if (gcase == rn_g.nomn && number == rn_g.sing)
		return nx_whole(m, o);
	const cmorphy3_form *pl = noun_nomn8(lemma);
	if (!pl)
		return nx_whole(m, o);
	rn_str x = {0};
	nx_safe_inflect(&x, pl, gs2(gcase, number), lemma);
	if (an && rn_py_isupper(abbr[0]))
		put_upper_at(o, x.s, x.n, 0);
	else
		nx_put(o, x.s, x.n);
	rn_str_free(&x);
	if (rn_keep_terminal_abbr_dot(m->s, m->n, m->end[0]))
		nx_putc(o, '.');
	return 0;
}

/* ---- _expand_contextual_etc_abbreviations ---- */

static int etc_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(ETC_ABBREVIATION_PATTERN);
	const uint32_t *T = m->s;
	size_t n = m->n, st = m->start[0], en = m->end[0], an, tn;
	const uint32_t *raw = nx_grp(m, p, "abbr", &an);
	const uint32_t *tail = nx_grp(m, p, "tail", &tn);
	int d = 0;
	for (size_t i = 0; i < an; i++)
		d |= raw[i] == U'д' || raw[i] == U'Д';
	if (an >= 2 && rn_py_isupper(raw[an - 2])) {
		rn_tok t[1];
		int k = left_tokens(T, nx_left(st, 40), st, t, 1);
		rn_str c = {0};
		if (k && rn_py_casefold(t[0].s, t[0].n, &c) < 0)
			nx_fail();
		int conj = k && nx_eq(c.s, c.n, U"и");
		rn_str_free(&c);
		if (!conj)
			return nx_whole(m, o);
	}
	uint32_t next_char = en < n ? T[en] : 0;
	int nl;
	size_t ni = rn_first_non_ws(T, n, en, &nl);
	int linebreak = nl;
	for (size_t i = 0; i < tn; i++)
		linebreak |= tail[i] == '\n';
	int keep = ni >= n || linebreak || rn_py_isupper(T[ni]);
	if (next_char && nx_inset(next_char, U"\"»”)]}") && isupper_at(T, n, ni + 1))
		keep = 1;
	nx_lit(o, d ? U"так далее" : U"тому подобное");
	if (keep)
		nx_putc(o, '.');
	nx_put(o, tail, tn);
	return 0;
}

/* ---- _expand_contextual_adjective_abbreviations ---- */

typedef const cmorphy3_form *form;

static int agree(form mod, form head)
{
	int mc = rn_canon_case(rn_case(mod->tag)), hc = rn_canon_case(rn_case(head->tag));
	int hn = rn_number(head->tag);
	if (mc != hc || rn_number(mod->tag) != hn)
		return 0;
	if (hn == rn_g.sing && rn_gender(mod->tag) != rn_gender(head->tag))
		return 0;
	if (hc == rn_g.accs && (hn == rn_g.plur || rn_gender(head->tag) == rn_g.masc)) {
		int ma = rn_animacy(mod->tag), ha = rn_animacy(head->tag);
		if (ma >= 0 && ha >= 0 && ma != ha)
			return 0;
	}
	return 1;
}

/* math.isclose(a, b): rel_tol 1e-9, abs_tol 0 */
static int isclose(double a, double b)
{
	double d = a > b ? a - b : b - a, x = a < 0 ? -a : a, y = b < 0 ? -b : b;
	return a == b || d <= 1e-9 * (x > y ? x : y);
}

static int is_barrier_pos(int p)
{
	return p == rn_g.ADJF || p == nx_gram("ADJS") || p == rn_g.COMP || p == rn_g.CONJ ||
	       p == rn_g.GRND || p == rn_g.NOUN || p == rn_g.NPRO || p == rn_g.NUMR ||
	       p == nx_gram("PRED") || p == rn_g.PREP || p == rn_g.PRTF || p == rn_g.PRTS;
}

static int infer_case(const uint32_t *T, size_t a, size_t st)
{
	static const char32_t *const UNAMB[][2] = {
	    {U"без", U"gent"},   {U"для", U"gent"},  {U"до", U"gent"},    {U"из", U"gent"},
	    {U"к", U"datv"},     {U"ко", U"datv"},   {U"между", U"ablt"}, {U"над", U"ablt"},
	    {U"о", U"loct"},     {U"об", U"loct"},   {U"обо", U"loct"},   {U"около", U"gent"},
	    {U"от", U"gent"},    {U"перед", U"ablt"}, {U"при", U"loct"},  {U"у", U"gent"},
	};
	rn_tok t[6];
	int k = left_tokens(T, a, st, t, 6), r = -1;
	rn_str w = {0};
	for (int i = k - 1; i >= 0; i--) {
		if (t[i].n == 1 && nx_inset(t[i].s[0], U".!?"))
			break;
		nx_lower(t[i].s, t[i].n, &w);
		size_t u = 0;
		while (u < sizeof UNAMB / sizeof *UNAMB && !nx_eq(w.s, w.n, UNAMB[u][0]))
			u++;
		if (u < sizeof UNAMB / sizeof *UNAMB) {
			char name[5];
			for (int j = 0; j < 4; j++)
				name[j] = (char)UNAMB[u][1][j];
			name[4] = 0;
			r = nx_gram(name);
			break;
		}
		const cmorphy3_form *f;
		int np = rn_mparse(w.s, w.n, &f);
		form best = NULL, verb = NULL;
		for (int j = 0; j < np; j++) {
			if (!best || f[j].score > best->score)
				best = &f[j];
			int p = rn_pos(f[j].tag);
			if ((p == rn_g.VERB || p == rn_g.INFN || p == rn_g.GRND) &&
			    (!verb || f[j].score > verb->score))
				verb = &f[j];
		}
		if (!best || !verb || !isclose(verb->score, best->score)) {
			if (best && is_barrier_pos(rn_pos(best->tag)))
				break;
			continue;
		}
		const rn_kv *vc = rn_table_get(RN_T_VERB_CASE, verb->normal_form);
		if (vc) {
			r = nx_gram(vc->value);
			break;
		}
		if (rn_has(verb->tag, rn_g.tran)) {
			r = rn_g.accs;
			break;
		}
	}
	rn_str_free(&w);
	return r;
}

/* compatibility() / substantive_score() keys, compared as tuples */
typedef struct {
	int a, b, c;
	double d;
} key4;

static int key_gt(key4 x, key4 y)
{
	if (x.a != y.a)
		return x.a > y.a;
	if (x.b != y.b)
		return x.b > y.b;
	if (x.c != y.c)
		return x.c > y.c;
	return x.d > y.d;
}

typedef struct {
	form *v;
	int n, cap;
} forms;

static void forms_add(forms *s, form f)
{
	if (s->n == s->cap) {
		int c = s->cap ? 2 * s->cap : 16;
		form *v = realloc(s->v, c * sizeof *v);
		if (!v) {
			nx_fail();
			return;
		}
		s->v = v;
		s->cap = c;
	}
	s->v[s->n++] = f;
}

static int is_adj(form f)
{
	int p = rn_pos(f->tag);
	return p == rn_g.ADJF || p == rn_g.PRTF;
}

static form choose_head(const rn_tok *words, int nw, int expected)
{
	const cmorphy3_form *pf[8];
	int pn[8];
	for (int i = 0; i < nw; i++)
		pn[i] = rn_mparse(words[i].s, words[i].n, &pf[i]);
	forms mods = {0};
	form fallback = NULL, result = NULL;
	for (int wi = 0; wi < nw && !result; wi++) {
		const cmorphy3_form *f = pf[wi];
		int np = pn[wi], has_noun = 0, has_adj = 0, has_subst = 0;
		int later_noun = 0, later_subst = 0, agrees_later = 0;
		for (int j = 0; j < np; j++) {
			has_noun |= rn_has(f[j].tag, rn_g.NOUN);
			has_adj |= is_adj(&f[j]);
			has_subst |= is_adj(&f[j]) && rn_has(f[j].tag, SUBX);
		}
		for (int l = wi + 1; l < nw; l++)
			for (int j = 0; j < pn[l]; j++) {
				const cmorphy3_form *g = &pf[l][j];
				if (rn_pos(g->tag) == rn_g.NOUN) {
					later_noun = 1;
					for (int q = 0; q < np && !agrees_later; q++)
						agrees_later = is_adj(&f[q]) && agree(&f[q], g);
				}
				later_subst |= is_adj(g) && rn_has(g->tag, SUBX);
			}
		if (has_adj && agrees_later) {
			for (int j = 0; j < np; j++)
				if (is_adj(&f[j]))
					forms_add(&mods, &f[j]);
			continue;
		}
		if (has_noun) {
			form best = NULL;
			key4 bk = {0, 0, 0, 0};
			int any_agree = 0;
			for (int j = 0; j < np; j++) {
				if (!rn_has(f[j].tag, rn_g.NOUN))
					continue;
				int nc = rn_canon_case(rn_case(f[j].tag)), nn = rn_number(f[j].tag);
				key4 k = {0, rn_has(f[j].tag, rn_g.inan), 0, 0};
				for (int q = 0; q < mods.n; q++) {
					form md = mods.v[q];
					if (rn_canon_case(rn_case(md->tag)) != nc || rn_number(md->tag) != nn)
						continue;
					if (nn == rn_g.sing && rn_gender(md->tag) != rn_gender(f[j].tag))
						continue;
					k.a++;
				}
				for (int q = 0; q < mods.n && !any_agree; q++)
					any_agree = agree(mods.v[q], &f[j]);
				if (expected >= 0) {
					if (nc == expected)
						k.c = 2;
					else if ((expected == rn_g.nomn || expected == rn_g.accs) &&
					         (nc == rn_g.nomn || nc == rn_g.accs) && nn == rn_g.plur &&
					         rn_has(f[j].tag, rn_g.inan))
						k.c = 1;
				}
				if (!best || key_gt(k, bk)) {
					best = &f[j];
					bk = k;
				}
			}
			if (!fallback)
				fallback = best;
			if (!mods.n || any_agree)
				result = best;
			continue;
		}
		if (has_subst && !agrees_later) {
			form best = NULL;
			key4 bk = {0, 0, 0, 0};
			for (int j = 0; j < np; j++) {
				if (!is_adj(&f[j]) || !rn_has(f[j].tag, SUBX))
					continue;
				key4 k = {expected >= 0 && rn_canon_case(rn_case(f[j].tag)) == expected, 0, 0,
				          f[j].score};
				for (int q = 0; q < mods.n; q++)
					k.b += agree(mods.v[q], &f[j]);
				if (!best || key_gt(k, bk)) {
					best = &f[j];
					bk = k;
				}
			}
			result = best;
			continue;
		}
		if (has_adj && (later_noun || later_subst))
			for (int j = 0; j < np; j++)
				if (is_adj(&f[j]))
					forms_add(&mods, &f[j]);
	}
	free(mods.v);
	return result ? result : fallback;
}

static void inflect_adj(rn_str *o, const char *lemma, form noun, int counted)
{
	const cmorphy3_form *a = adj_parse8(lemma, 1);
	if (!a) {
		nx_put8(o, lemma);
		return;
	}
	int nc = rn_canon_case(rn_case(noun->tag));
	int plural = rn_has(noun->tag, rn_g.plur), gender = rn_gender(noun->tag);
	cmorphy3_gset tags;
	if (counted) {
		tags = gs2(rn_g.gent, rn_g.plur);
	} else {
		tags = gs2(nc, plural ? rn_g.plur : rn_g.sing);
		if (!plural && gender >= 0)
			tags = rn_gset_or(tags, rn_gset1(gender));
		if (nc == rn_g.accs && (plural || gender == rn_g.masc)) {
			if (rn_has(noun->tag, rn_g.anim))
				tags = rn_gset_or(tags, rn_gset1(rn_g.anim));
			else if (rn_has(noun->tag, rn_g.inan))
				tags = rn_gset_or(tags, rn_gset1(rn_g.inan));
		}
	}
	nx_safe_inflect(o, a, tags, lemma);
}

/* _has_non_one_cardinal_before_abbreviation(text[a:st]) */
static int non_one_cardinal(const uint32_t *T, size_t st)
{
	rn_tok t[1];
	if (!left_tokens(T, nx_left(st, 40), st, t, 1))
		return 0;
	rn_str c = {0};
	if (rn_py_casefold(t[0].s, t[0].n, &c) < 0)
		nx_fail();
	static const char32_t *const ONE[] = {U"один", U"одна", U"одно", U"одну", NULL};
	int r = 0;
	if (!nx_oneof(c.s, c.n, ONE)) {
		const cmorphy3_form *f;
		int np = rn_mparse(c.s ? c.s : (const uint32_t *)U"", c.n, &f);
		for (int i = 0; i < np && !r; i++)
			r = rn_pos(f[i].tag) == rn_g.NUMR;
	}
	rn_str_free(&c);
	return r;
}

static int adj_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(ADJECTIVE_ABBREVIATION_PATTERN);
	const uint32_t *T = m->s;
	size_t st = m->start[0], an, phn;
	const uint32_t *abbr = nx_grp(m, p, "abbr", &an);
	const uint32_t *ph = nx_grp(m, p, "phrase", &phn);
	rn_str al = {0};
	nx_lower(abbr, an, &al);
	static const char *const LANG[] = {"англ.",  "руск.", "рус.",   "немецк.", "нем.",
	                                   "франц.", "греч.", "латин.", "лат.",    NULL};
	size_t kc = 0;
	char *k8 = rn_to_utf8(al.s ? al.s : (const uint32_t *)U"", al.n, NULL, &kc);
	rn_str_free(&al);
	if (!k8) {
		nx_fail();
		return nx_whole(m, o);
	}
	int lang = 0;
	for (const char *const *l = LANG; *l && !lang; l++)
		lang = !strcmp(k8, *l);
	int heading = !strcmp(k8, "гл.");
	const rn_kv *e = rn_table_get(RN_T_ADJECTIVE_ABBREVIATION_EXPANSIONS, k8);
	free(k8);
	if (lang && phn && rn_py_isupper(ph[0]))
		return nx_whole(m, o);
	/* phrase.split(): words of [А-ЯЁа-яё-]+ between \s+, at most 6 */
	rn_tok words[8];
	int nw = 0;
	for (size_t i = 0; i < phn;) {
		while (i < phn && rn_py_isspace(ph[i]))
			i++;
		size_t j = i;
		while (j < phn && !rn_py_isspace(ph[j]))
			j++;
		if (j > i && nw < 8)
			words[nw++] = (rn_tok){ph + i, j - i};
		i = j;
	}
	if (!nw || !e) {
		nx_fail();
		return nx_whole(m, o);
	}
	const cmorphy3_form *f0 = rn_mfirst(words[0].s, words[0].n);
	int p0 = rn_pos(f0->tag);
	if (p0 == rn_g.PREP || p0 == rn_g.CONJ || p0 == rn_g.PRCL || p0 == rn_g.INTJ ||
	    p0 == rn_g.NUMR || rn_has(f0->tag, rn_g.Anum))
		return nx_whole(m, o);
	size_t a = nx_left(st, 160);
	int expected = infer_case(T, a, st);
	form noun = choose_head(words, nw, expected);
	if (!noun)
		return nx_whole(m, o);
	if (!word_known8(noun->word) || !rn_word_is_known(words[0].s, words[0].n))
		return nx_whole(m, o);
	if (heading) {
		/* _governs_genitive_whole */
		const cmorphy3_form *f;
		int np = rn_mparse8(noun->word, &f), g = 0;
		for (int i = 0; i < np && !g; i++)
			g = rn_pos(f[i].tag) == rn_g.NOUN && rn_case(f[i].tag) == rn_g.gent &&
			    rn_has(f[i].tag, rn_g.inan);
		if (g)
			return nx_whole(m, o);
	}
	rn_str x = {0};
	inflect_adj(&x, e->value, noun, non_one_cardinal(T, st));
	if (an && rn_py_isupper(abbr[0]))
		put_upper_at(o, x.s, x.n, 0);
	else
		nx_put(o, x.s, x.n);
	rn_str_free(&x);
	nx_putg(o, m, p, "space");
	nx_put(o, ph, phn);
	return 0;
}

/* ---- the literal rules (_expand_rule_preserving_sentence_case) ---- */

static int starts_sentence(const uint32_t *T, size_t start)
{
	size_t i = start;
	while (i > 0 && nx_inset(T[i - 1], U" \t\r\"'«„“([{"))
		i--;
	return i == 0 || nx_inset(T[i - 1], U".!?…\n");
}

static int rule_repl(const rn_match *m, rn_str *o, void *ctx)
{
	const char *repl = ctx;
	rn_str x = {0};
	nx_put8(&x, repl);
	const uint32_t *s = m->s + m->start[0];
	size_t n = m->end[0] - m->start[0];
	/* re.findall(r"[^\W\d_]+", match.group(0)) */
	rn_iter it = rn_iter_init(P(L565), s, n, 0);
	rn_match w;
	int nwords = 0, single = 1, r;
	while ((r = rn_iter_next(&it, &w)) == 1) {
		nwords++;
		single &= w.end[0] - w.start[0] == 1;
	}
	if (r < 0)
		nx_fail();
	size_t fa = first_alpha(s, n);
	size_t xa = first_alpha(x.s, x.n);
	if (nwords >= 2 && single && fa < n && rn_py_isupper(s[fa]) &&
	    starts_sentence(m->s, m->start[0]) && xa < x.n)
		put_upper_at(o, x.s, x.n, xa);
	else
		nx_put(o, x.s, x.n);
	rn_str_free(&x);
	return 0;
}

static void literal_rules(rn_str *t)
{
	size_t nr;
	const rn_kv *rules = rn_abbr_rules(&nr);
	rn_str folded = {0}, hint = {0}, out = {0};
	if (rn_py_casefold(t->s, t->n, &folded) < 0)
		nx_fail();
	for (size_t i = 0; i < nr; i++) {
		if (rules[i].key) {
			hint.n = 0;
			nx_put8(&hint, rules[i].key);
			if (rn_py_find(folded.s ? folded.s : (const uint32_t *)U"", folded.n, hint.s,
			               hint.n, 0) < 0)
				continue;
		}
		out.n = 0;
		if (rn_sub(rn_pat(RN_ABBREVIATION_RULES_ABBREVIATION_PATTERNS_0_0 + (int)i), t->s,
		           t->n, rule_repl, (void *)rules[i].value, 0, &out) < 0) {
			nx_fail();
			continue;
		}
		if (!out.s)
			nx_put(&out, NULL, 0);
		if (!rn_str_eq(&out, t)) {
			rn_str tmp = *t;
			*t = out;
			out = tmp;
			folded.n = 0;
			if (rn_py_casefold(t->s, t->n, &folded) < 0)
				nx_fail();
		}
	}
	rn_str_free(&folded);
	rn_str_free(&hint);
	rn_str_free(&out);
}

/* ---- expand_person_initials (vowel_mode double, pause_mode comma) ---- */

static void put_initial(rn_str *o, uint32_t c)
{
	const char *name = rn_letter_name(c, 1);
	if (name && *name)
		nx_put8(o, name);
	else if (rn_py_lower(&c, 1, o) < 0)
		nx_fail();
}

static int name_marks(const uint32_t *s, size_t n, int confident)
{
	const cmorphy3_form *f;
	int np = rn_mparse(s, n, &f), nm = 0, geox = 1, abbr = 1;
	for (int i = 0; i < np; i++) {
		if (rn_has(f[i].tag, PNCT))
			continue;
		if (rn_has(f[i].tag, rn_g.Surn) || rn_has(f[i].tag, rn_g.Name) ||
		    rn_has(f[i].tag, rn_g.Patr))
			return 1;
		if (nm < 3) {
			geox &= rn_has(f[i].tag, rn_g.Geox);
			abbr &= rn_has(f[i].tag, rn_g.Abbr);
		}
		nm++;
	}
	if (confident)
		return 0;
	if (!nm)
		return 1;
	if (geox)
		return 0;
	return !abbr;
}

static int following_name(const uint32_t *T, size_t n, size_t end)
{
	const rn_re *p = P(FOLLOWING_NAME_TOKEN_PATTERN);
	rn_match m;
	int k = rn_match_at(p, T, n, end, &m);
	if (k < 0)
		nx_fail();
	if (k <= 0)
		return 0;
	size_t tn;
	const uint32_t *t = nx_grp(&m, p, "token", &tn);
	return name_marks(t, tn, 1);
}

/* choose_tail: *final, *clause */
static void choose_tail(const uint32_t *T, size_t n, size_t end, int *final, int *clause)
{
	*final = 1;
	*clause = 0;
	size_t i = rn_first_non_ws(T, n, end, NULL);
	if (i >= n)
		return;
	if (nx_inset(T[i], U"\"'«“„‟‚‘‹([{")) {
		*final = 0;
		return;
	}
	while (i < n && nx_inset(T[i], U"\"'»”’›)]}"))
		i = rn_first_non_ws(T, n, i + 1, NULL);
	if (i >= n || nx_inset(T[i], U".!?…"))
		return;
	if (nx_inset(T[i], U",;:—–-")) {
		*final = 0;
		*clause = 1;
		return;
	}
	if (rn_py_islower(T[i]) || rn_py_isdigit(T[i]))
		*final = 0;
}

static void put_terminal(rn_str *o, const uint32_t *T, size_t n, size_t end)
{
	size_t i = rn_first_non_ws(T, n, end, NULL);
	if (!(i < n && nx_inset(T[i], U".!?…")))
		nx_putc(o, '.');
}

enum { SURNAME_INITIALS, INITIALS_SURNAME, SURNAME_SINGLE, SINGLE_SURNAME };

static int initials_repl(const rn_match *m, rn_str *o, void *ctx)
{
	int kind = *(const int *)ctx;
	static const int PAT[] = {RN_ABBREVIATIONS_PERSON_SURNAME_INITIALS_PATTERN,
	                          RN_ABBREVIATIONS_PERSON_INITIALS_SURNAME_PATTERN,
	                          RN_ABBREVIATIONS_PERSON_SURNAME_SINGLE_INITIAL_PATTERN,
	                          RN_ABBREVIATIONS_PERSON_SINGLE_INITIAL_SURNAME_PATTERN};
	const rn_re *p = rn_pat(PAT[kind]);
	const uint32_t *T = m->s;
	size_t n = m->n, en = m->end[0], sn, l1, l2 = 0;
	const uint32_t *sur = nx_grp(m, p, "surname", &sn);
	const uint32_t *i1 = nx_grp(m, p, "i1", &l1);
	const uint32_t *i2 = kind <= INITIALS_SURNAME ? nx_grp(m, p, "i2", &l2) : NULL;
	int surname_first = kind == SURNAME_INITIALS || kind == SURNAME_SINGLE;
	if (surname_first && following_name(T, n, en))
		return nx_whole(m, o);
	if (kind >= SURNAME_SINGLE && !name_marks(sur, sn, 0))
		return nx_whole(m, o);
	int final, clause;
	choose_tail(T, n, en, &final, &clause);
	if (surname_first) {
		const char32_t *sep = final ? U" " : U", ";
		nx_put(o, sur, sn);
		nx_lit(o, sep);
		put_initial(o, i1[0]);
		if (i2) {
			nx_lit(o, sep);
			put_initial(o, i2[0]);
		}
		if (final)
			put_terminal(o, T, n, en);
		else if (!clause)
			nx_putc(o, ',');
	} else {
		put_initial(o, i1[0]);
		nx_putc(o, ' ');
		if (i2) {
			put_initial(o, i2[0]);
			nx_putc(o, ' ');
		}
		nx_put(o, sur, sn);
		if (final)
			put_terminal(o, T, n, en);
	}
	(void)l1;
	(void)l2;
	return 0;
}

/* ---- expand_letter_abbreviations (vowel_mode double) ---- */

static int sentence_continues(const uint32_t *T, size_t n, size_t end)
{
	if (end >= n)
		return 0;
	if (T[end] == '.')
		end++;
	if (end >= n || !rn_py_isspace(T[end]))
		return 0;
	size_t i = rn_first_non_ws(T, n, end, NULL);
	return i < n && (rn_py_isalpha(T[i]) || rn_py_isdigit(T[i]));
}

/* restore_sentence_case(token, reading, start) appended */
static void put_sentence_case(rn_str *o, const uint32_t *T, size_t n, size_t start, size_t tn,
                              const rn_str *reading)
{
	size_t fa = first_alpha(T + start, tn), ra = first_alpha(reading->s, reading->n);
	if (fa < tn && rn_py_isupper(T[start + fa]) && starts_sentence(T, start) &&
	    sentence_continues(T, n, start + tn) && ra < reading->n)
		put_upper_at(o, reading->s, reading->n, ra);
	else
		nx_put(o, reading->s, reading->n);
}

/* apply_letter_vowel_mode(reading, "double") */
static int vowel_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const uint32_t *s = m->s + m->start[0];
	size_t n = m->end[0] - m->start[0];
	static const struct {
		enum rn_table single, dbl;
	} SETS[] = {{RN_T_EN_LETTER_NAMES, RN_T_EN_DOUBLED_VOWEL_LETTER_NAMES},
	            {RN_T_RU_LETTER_NAMES, RN_T_RU_DOUBLED_VOWEL_LETTER_NAMES}};
	/* {**RU, **EN}: EN wins on a clash, so it is looked at first */
	rn_str v = {0};
	for (size_t k = 0; k < 2; k++) {
		size_t nd;
		const rn_kv *d = rn_table_all(SETS[k].dbl, &nd);
		for (size_t i = 0; i < nd; i++) {
			const rn_kv *single = rn_table_get(SETS[k].single, d[i].key);
			if (!single)
				continue;
			v.n = 0;
			nx_put8(&v, single->value);
			if (v.n == n && !memcmp(v.s, s, n * sizeof *s)) {
				rn_str_free(&v);
				nx_put8(o, d[i].value);
				return 0;
			}
		}
	}
	rn_str_free(&v);
	return nx_whole(m, o);
}

typedef struct {
	int dotted;
} tokctx;

static int known_abbr(const rn_str *key, const char **value)
{
	const rn_kv *e = rn_table_get32(RN_T_KNOWN_ABBREVIATIONS, key->s, key->n);
	*value = e ? e->value : NULL;
	return e != NULL;
}

static int is_real_word_pos(int p)
{
	static int set[12] = {-2};
	if (set[0] == -2) {
		static const char *const N[] = {"NOUN", "VERB", "ADJF", "ADJS", "ADVB", "INFN",
		                                "PRTF", "PRTS", "GRND", "PREP", "CONJ", "PRCL"};
		for (int i = 0; i < 12; i++)
			set[i] = nx_gram(N[i]);
	}
	for (int i = 0; i < 12; i++)
		if (p == set[i])
			return 1;
	return p == rn_g.NPRO;
}

static int token_repl(const rn_match *m, rn_str *o, void *ctx)
{
	int dotted = ((const tokctx *)ctx)->dotted;
	const uint32_t *T = m->s;
	size_t n = m->n, st = m->start[0], en = m->end[0], tn = en - st;
	const uint32_t *tok = T + st;
	int keep_dot = 1;
	if (dotted) {
		/* keep_dotted_tail_dot */
		int nl;
		size_t i = rn_first_non_ws(T, n, en, &nl);
		if (i >= n || nl || nx_inset(T[i], U".!?…"))
			keep_dot = 1;
		else if (nx_inset(T[i], U"\"»”)]}")) {
			size_t j = rn_first_non_ws(T, n, i + 1, NULL);
			keep_dot = j >= n || rn_py_isupper(T[j]);
		} else
			keep_dot = rn_py_isupper(T[i]);
	}
	int tail_dot = tn && tok[tn - 1] == '.' && keep_dot;
	rn_str key = {0}, reading = {0};
	for (size_t i = 0; i < tn; i++)
		if (rn_py_isalpha(tok[i]) && rn_py_upper(tok + i, 1, &key) < 0)
			nx_fail();
	size_t nl = 0;
	for (size_t i = 0; i < tn; i++)
		nl += rn_py_isalpha(tok[i]);
	int whole = 0;
	const char *custom;
	if (nl < 2) {
		whole = 1;
	} else if (known_abbr(&key, &custom)) {
		nx_put8(&reading, custom);
		if (!nx_eq(key.s, key.n, U"UTC") && !nx_eq(key.s, key.n, U"ISO")) {
			nx_resub(rn_pat(RN_CONSTANTS_STANDALONE_VOWEL_NAME_PATTERN), &reading, vowel_repl,
			         NULL);
		}
	} else {
		int ru = 1, en_ = 1, vowels = 0;
		for (size_t i = 0; i < key.n; i++) {
			ru &= rn_in32(RU_LETTER_NAMES, key.s + i, 1);
			en_ &= rn_in32(EN_LETTER_NAMES, key.s + i, 1);
			vowels += nx_inset(key.s[i], U"АЕЁИОУЫЭЮЯ");
		}
		if (ru) {
			if (!dotted && vowels) {
				rn_str low = {0};
				nx_lower(key.s, key.n, &low);
				if (rn_word_is_known(low.s, low.n)) {
					const cmorphy3_form *f = rn_mfirst(low.s, low.n);
					if (is_real_word_pos(rn_pos(f->tag)) && !rn_has(f->tag, rn_g.Abbr))
						whole = 1;
				}
				rn_str_free(&low);
			}
			if (!dotted && (key.n > 5 || vowels > 1))
				whole = 1;
		} else if (en_) {
			if (!dotted && key.n > 5)
				whole = 1;
		} else {
			whole = 1;
		}
		for (size_t i = 0; i < key.n && !whole; i++) {
			if (i)
				nx_putc(&reading, ' ');
			const char *name = rn_letter_name(key.s[i], 1);
			if (name && *name)
				nx_put8(&reading, name);
			else
				nx_putc(&reading, key.s[i]);
		}
	}
	if (whole) {
		nx_put(o, tok, tn);
	} else {
		put_sentence_case(o, T, n, st, tn, &reading);
		if (tail_dot)
			nx_putc(o, '.');
	}
	rn_str_free(&key);
	rn_str_free(&reading);
	return 0;
}

/* ---- _normalize_contextual_ampersands (enable_latinization) ---- */

static int en_letter_name(const uint32_t *s, size_t n)
{
	rn_str l = {0}, v = {0}, vl = {0};
	nx_lower(s, n, &l);
	static const enum rn_table TS[] = {RN_T_EN_LETTER_NAMES, RN_T_EN_DOUBLED_VOWEL_LETTER_NAMES};
	int r = 0;
	for (size_t k = 0; k < 2 && !r; k++) {
		size_t nt;
		const rn_kv *t = rn_table_all(TS[k], &nt);
		for (size_t i = 0; i < nt && !r; i++) {
			if (strchr(t[i].value, ' '))
				continue;
			v.n = 0;
			nx_put8(&v, t[i].value);
			nx_lower(v.s ? v.s : (const uint32_t *)U"", v.n, &vl);
			r = vl.n == l.n && !memcmp(vl.s, l.s, l.n * sizeof *l.s);
		}
	}
	rn_str_free(&l);
	rn_str_free(&v);
	rn_str_free(&vl);
	return r;
}

static int amp_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = P(WORD_AMPERSAND_PATTERN);
	size_t ln, rn;
	const uint32_t *l = nx_grp(m, p, "left", &ln), *r = nx_grp(m, p, "right", &rn);
	int la = 0, ra = 0, lc = 0, rc = 0;
	for (size_t i = 0; i < ln; i++) {
		la |= is_ascii_letter(l[i]);
		lc |= is_cyr(l[i]);
	}
	for (size_t i = 0; i < rn; i++) {
		ra |= is_ascii_letter(r[i]);
		rc |= is_cyr(r[i]);
	}
	int le = en_letter_name(l, ln), re_ = en_letter_name(r, rn);
	const char32_t *join;
	if ((la && ra) || (le && re_) || (le && ra) || (la && re_))
		join = U" энд ";
	else if (lc || rc || le || re_)
		join = U" и ";
	else
		return nx_whole(m, o);
	nx_put(o, l, ln);
	nx_lit(o, join);
	nx_put(o, r, rn);
	return 0;
}

/* ---- fixes ---- */

/* R18: "гг. депутаты" → "господа депутаты", in the case of the following plural animate noun
 * or name */
static void gg_gospoda(rn_str *t)
{
	rn_str out = {0};
	size_t pos = 0;
	for (size_t i = 0; i + 3 < t->n; i++) {
		const uint32_t *T = t->s;
		if ((T[i] != U'г' && T[i] != U'Г') || (T[i + 1] != U'г' && T[i + 1] != U'Г') ||
		    T[i + 2] != '.' || (i && is_word_char(T[i - 1])))
			continue;
		size_t j = i + 3;
		while (j < t->n && rn_py_isspace(T[j]))
			j++;
		size_t k = j;
		while (k < t->n && is_cyr(T[k]))
			k++;
		if (j == i + 3 || k == j || (k < t->n && is_word_char(T[k])))
			continue;
		const cmorphy3_form *f;
		int np = rn_mparse(T + j, k - j, &f), c = -1;
		for (int q = 0; q < np && c < 0; q++)
			if (rn_has(f[q].tag, rn_g.NOUN) && rn_has(f[q].tag, rn_g.anim) &&
			    rn_has(f[q].tag, rn_g.plur))
				c = rn_case(f[q].tag);
		for (int q = 0; q < np && c != rn_g.nomn && rn_py_isupper(T[j]); q++)
			if ((rn_has(f[q].tag, rn_g.Surn) || rn_has(f[q].tag, rn_g.Name)) &&
			    (c < 0 || rn_case(f[q].tag) == rn_g.nomn))
				c = rn_case(f[q].tag);
		if (c < 0)
			continue;
		rn_str x = {0};
		const cmorphy3_form *g = noun_nomn8("господин");
		if (g)
			nx_safe_inflect(&x, g, gs2(rn_canon_case(c), rn_g.plur), "господа");
		else
			nx_lit(&x, U"господа");
		nx_put(&out, T + pos, i - pos);
		if (T[i] == U'Г')
			put_upper_at(&out, x.s, x.n, 0);
		else
			nx_put(&out, x.s, x.n);
		rn_str_free(&x);
		pos = i + 3;
	}
	if (pos) {
		nx_put(&out, t->s + pos, t->n - pos);
		rn_str tmp = *t;
		*t = out;
		out = tmp;
	}
	rn_str_free(&out);
}

static int is_cyr_upper(uint32_t c)
{
	return is_cyr(c) && rn_py_isupper(c);
}

/* R23: "СПбГУ": capitals with single lower-case consonants between them are read like the
 * all-capitals "СПБГУ" (the lookarounds of ABBREV_SOLID_PATTERN do not see а-яё, so "СП" and
 * "ГУ" were read apart around the "б") */
static void mixed_abbr(rn_str *t)
{
	rn_str out = {0}, up = {0}, r = {0};
	size_t pos = 0;
	for (size_t i = 0; i < t->n; i++) {
		const uint32_t *T = t->s;
		if (!is_cyr_upper(T[i]) || (i && rn_py_isalpha(T[i - 1])))
			continue;
		size_t k = i, lows = 0;
		for (;;) {
			while (k < t->n && is_cyr_upper(T[k]))
				k++;
			if (k + 1 < t->n && nx_inset(T[k], U"бвгджзклмнпрстфхцчшщ") &&
			    is_cyr_upper(T[k + 1])) {
				k++;
				lows++;
				continue;
			}
			break;
		}
		/* "СПбГУ", not the names "ЫнГ+ен", "ЮнМи": two capitals first, a word end after */
		if (!lows || k - i < 4 || !is_cyr_upper(T[i + 1]) ||
		    (k < t->n && (rn_py_isalpha(T[k]) || T[k] == '+'))) {
			i = k ? k - 1 : i;
			continue;
		}
		up.n = 0;
		if (rn_py_upper(T + i, k - i, &up) < 0)
			nx_fail();
		/* KNOWN_ABBREVIATIONS or letter by letter, never as a word */
		r.n = 0;
		const char *custom;
		if (known_abbr(&up, &custom)) {
			nx_put8(&r, custom);
			nx_resub(rn_pat(RN_CONSTANTS_STANDALONE_VOWEL_NAME_PATTERN), &r, vowel_repl, NULL);
		} else
			for (size_t q = 0; q < up.n; q++) {
				if (q)
					nx_putc(&r, ' ');
				const char *name = rn_letter_name(up.s[q], 1);
				if (name && *name)
					nx_put8(&r, name);
				else
					nx_putc(&r, up.s[q]);
			}
		nx_put(&out, T + pos, i - pos);
		nx_put(&out, r.s, r.n);
		pos = k;
		i = k - 1;
	}
	if (pos) {
		nx_put(&out, t->s + pos, t->n - pos);
		rn_str tmp = *t;
		*t = out;
		out = tmp;
	}
	rn_str_free(&out);
	rn_str_free(&up);
	rn_str_free(&r);
}

/* R24: abbreviations the original has no rules for */
#include "rn-abbr-more.h"

static rn_re *xp_cache[XP_N];
static pthread_mutex_t xp_lock = PTHREAD_MUTEX_INITIALIZER;

static const rn_re *xp(int i)
{
	rn_re *re = __atomic_load_n(&xp_cache[i], __ATOMIC_ACQUIRE);
	if (re)
		return re;
	pthread_mutex_lock(&xp_lock);
	re = xp_cache[i];
	if (!re) {
		char err[256];
		re = calloc(1, sizeof *re);
		if (re && rn_re_compile(re, XP_SRC[i], err, sizeof err) < 0) {
			fprintf(stderr, "rn-abbrx: pattern %d: %s\n", i, err);
			free(re);
			re = NULL;
		}
		if (re)
			__atomic_store_n(&xp_cache[i], re, __ATOMIC_RELEASE);
		else
			nx_fail();
	}
	pthread_mutex_unlock(&xp_lock);
	return re;
}

static void xp_resub(int i, rn_str *t, int (*repl)(const rn_match *, rn_str *, void *))
{
	const rn_re *re = xp(i);
	if (re)
		nx_resub(re, t, repl, NULL);
}

/* the case the left context gives a word at T[st]: the preposition right before it, else
 * infer_case(), -1; *prev_noun: the word right before is a declinable noun ("статья проф.") */
static int ctx_case(const uint32_t *T, size_t st, int *prev_noun)
{
	size_t a = nx_left(st, 40);
	rn_tok *L = NULL;
	long n = rn_simple_tokenize(T + a, st - a, &L);
	if (n < 0) {
		nx_fail();
		n = 0;
	}
	int c = -1;
	*prev_noun = 0;
	rn_str w = {0};
	if (n) {
		const rn_tok *l = &L[n - 1];
		lower_strip(l->s, l->n, &w);
		int punct = nx_inset(l->s[l->n - 1], U".,:;!?…)»\"–—");
		if (w.n && !punct) {
			c = prep_case(&w);
			if (c < 0) {
				const cmorphy3_form *f = rn_mfirst(w.s, w.n);
				*prev_noun = rn_has(f->tag, rn_g.NOUN) && !rn_has(f->tag, rn_g.Fixd);
			}
		}
	}
	free(L);
	rn_str_free(&w);
	if (c < 0 && !*prev_noun)
		c = infer_case(T, nx_left(st, 160), st);
	return c;
}

static void put_cap(rn_str *o, const rn_str *x, int upper)
{
	if (upper)
		put_upper_at(o, x->s, x->n, 0);
	else
		nx_put(o, x->s, x->n);
}

static int vtch_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	nx_lit(o, m->s[m->start[0]] == U'В' ? U"В том числе" : U"в том числе");
	return 0;
}

static int spb_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	(void)m;
	nx_lit(o, U"Санкт-");
	return 0;
}

/* "проф. Иванову" → "профессору Иванову": the case of the name the context allows */
static int title_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = xp(XP_TITLE);
	size_t an, nn;
	const uint32_t *abbr = nx_grp(m, p, "abbr", &an);
	const uint32_t *name = nx_grp(m, p, "name", &nn);
	uint32_t c0 = abbr[0] | 0x20;   /* п а д, lower-case in this range */
	const char *lemma = c0 == U'п' ? "профессор" : c0 == U'а' ? "академик" : "доцент";
	const cmorphy3_form *f;
	int np = rn_mparse(name, nn, &f);
	int cases[24], nc = 0;
	/* names first, singular first ("Иванов" is also the plural genitive) */
	for (int pass = 0; pass < 6 && !nc; pass++)
		for (int i = 0; i < np && nc < 24; i++) {
			int kind = pass % 3, sing = pass < 3;
			int ok = kind == 0   ? rn_has(f[i].tag, rn_g.Surn) || rn_has(f[i].tag, rn_g.Name)
			         : kind == 1 ? rn_has(f[i].tag, rn_g.NOUN)
			                     : 1;
			ok = ok && (!sing || !rn_has(f[i].tag, rn_g.plur));
			int c = rn_case(f[i].tag);
			if (!ok || c < 0)
				continue;
			c = rn_canon_case(c);
			int seen = 0;
			for (int k = 0; k < nc && !seen; k++)
				seen = cases[k] == c;
			if (!seen)
				cases[nc++] = c;
		}
	int pn, cx = ctx_case(m->s, m->start[0], &pn), r = -1;
	int has_nomn = 0, has_gent = 0;
	for (int k = 0; k < nc; k++) {
		if (cx >= 0 && cases[k] == rn_canon_case(cx))
			r = cases[k];
		has_nomn |= cases[k] == rn_g.nomn;
		has_gent |= cases[k] == rn_g.gent;
	}
	if (r < 0)
		r = nc == 1 ? cases[0] : pn && has_gent ? rn_g.gent : has_nomn || !nc ? rn_g.nomn : cases[0];
	rn_str x = {0};
	const cmorphy3_form *g = noun_nomn8(lemma);
	if (g && r != rn_g.nomn)
		nx_safe_inflect(&x, g, gs2(r, rn_g.sing), lemma);
	else
		nx_put8(&x, lemma);
	put_cap(o, &x, rn_py_isupper(abbr[0]));
	rn_str_free(&x);
	nx_putg(o, m, p, "space");
	return 0;
}

/* the words of an agreed phrase after T[from]: an opening quote, then up to 6 words of
 * [А-ЯЁа-яё-] split by spaces, the last one before any other character */
static int phrase_words(const uint32_t *T, size_t n, size_t from, rn_tok *w, int max)
{
	size_t i = from;
	if (i < n && nx_inset(T[i], U"«\"„“"))
		i++;
	int nw = 0;
	while (nw < max && i < n) {
		size_t j = i;
		while (j < n && (is_cyr(T[j]) || T[j] == '-'))
			j++;
		if (j == i)
			break;
		w[nw++] = (rn_tok){T + i, j - i};
		size_t k = j;
		while (k < n && rn_py_isspace(T[k]))
			k++;
		if (k == j)
			break;
		i = k;
	}
	return nw;
}

/* the adjective lemma agreed with the head noun of the phrase at T[from], as adj_repl does;
 * 0 if no head */
static int agreed_adj(rn_str *x, const uint32_t *T, size_t n, size_t st, size_t from,
                      const char *lemma)
{
	rn_tok words[6];
	int nw = phrase_words(T, n, from, words, 6);
	if (!nw)
		return 0;
	const cmorphy3_form *f0 = rn_mfirst(words[0].s, words[0].n);
	int p0 = rn_pos(f0->tag);
	if (p0 == rn_g.PREP || p0 == rn_g.CONJ || p0 == rn_g.PRCL || p0 == rn_g.INTJ ||
	    p0 == rn_g.NUMR || p0 == rn_g.VERB || p0 == rn_g.INFN)
		return 0;
	form noun = choose_head(words, nw, infer_case(T, nx_left(st, 160), st));
	if (!noun || !word_known8(noun->word))
		return 0;
	inflect_adj(x, lemma, noun, 0);
	return 1;
}

/* "т. н. эксперты" → "так называемые эксперты" */
static int tn_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	rn_str x = {0};
	nx_lit(&x, U"так ");
	if (!agreed_adj(&x, m->s, m->n, m->start[0], m->end[0], "называемый"))
		nx_lit(&x, U"называемый");
	put_cap(o, &x, m->s[m->start[0]] == U'Т');
	rn_str_free(&x);
	nx_putg(o, m, xp(XP_TN), "space");
	return 0;
}

/* "ок. ста", "ок. Москвы" → "около": before a numeral or a word with a genitive form */
static int ok_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const uint32_t *T = m->s;
	size_t i = m->end[0], j = i;
	while (j < m->n && is_cyr(T[j]))
		j++;
	const cmorphy3_form *f;
	int np = j > i ? rn_mparse(T + i, j - i, &f) : 0, ok = 0;
	for (int k = 0; k < np && !ok; k++)
		ok = rn_pos(f[k].tag) == rn_g.NUMR ||
		     (rn_pos(f[k].tag) != rn_g.PREP && rn_case(f[k].tag) == rn_g.gent);
	if (!ok)
		return nx_whole(m, o);
	nx_lit(o, T[m->start[0]] == U'О' ? U"Около" : U"около");
	nx_putg(o, m, xp(XP_OK), "space");
	return 0;
}

/* "ж. д. вокзал" → "железнодорожный вокзал", else "по ж. д." → "по железной дороге" */
static int zd_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const uint32_t *T = m->s;
	size_t st = m->start[0], en = m->end[0];
	int upper = T[st] == U'Ж';
	rn_str x = {0};
	if (en < m->n && is_cyr(T[en]) && rn_py_islower(T[en]) &&
	    agreed_adj(&x, T, m->n, st, en, "железнодорожный")) {
		put_cap(o, &x, upper);
		rn_str_free(&x);
		nx_putg(o, m, xp(XP_ZD), "space");
		return 0;
	}
	x.n = 0;
	int pn, c = ctx_case(T, st, &pn);
	if (c < 0)
		c = pn ? rn_g.gent : rn_g.nomn;
	c = rn_canon_case(c);
	const cmorphy3_form *a = adj_parse8("железный", 0), *d = noun_nomn8("дорога");
	if (a && d && c != rn_g.nomn) {
		nx_safe_inflect(&x, a, rn_gset_or(gs2(c, rn_g.sing), rn_gset1(rn_g.femn)), "железная");
		nx_putc(&x, ' ');
		nx_safe_inflect(&x, d, gs2(c, rn_g.sing), "дорога");
	} else
		nx_lit(&x, U"железная дорога");
	put_cap(o, &x, upper);
	rn_str_free(&x);
	nx_putg(o, m, xp(XP_ZD), "space");
	return 0;
}

static void abbr_more(rn_str *t)
{
	xp_resub(XP_SPB, t, spb_repl);
	xp_resub(XP_VTCH, t, vtch_repl);
	xp_resub(XP_TITLE, t, title_repl);
	xp_resub(XP_TN, t, tn_repl);
	xp_resub(XP_OK, t, ok_repl);
	xp_resub(XP_ZD, t, zd_repl);
}

/* ---- expand_abbreviations ---- */

int rn_run_abbreviations(const rn_str *in, rn_str *out)
{
	rn_mx_trim();
	rn_mx_failed = 0;
	rn_str t = {0};
	if (rn_str_put(&t, in->s ? in->s : (const uint32_t *)U"", in->n) < 0)
		return -1;
	nx_resub(P(LANGUAGE_ORIGIN_PATTERN), &t, lang_repl, NULL);
	nx_resub(P(GEO_CLASSIFIER_PATTERN), &t, geo_repl, NULL);
	if (rn_fixes & RN_FIX_GEO_RIVER)
		geo_river(&t);
	nx_resub(P(DECLINABLE_NOUN_ABBREVIATION_PATTERN), &t, noun_repl, NULL);
	nx_resub(P(ETC_ABBREVIATION_PATTERN), &t, etc_repl, NULL);
	nx_resub(P(ADJECTIVE_ABBREVIATION_PATTERN), &t, adj_repl, NULL);
	if (rn_fixes & RN_FIX_ABBR_GG)
		gg_gospoda(&t);
	if (rn_fixes & RN_FIX_ABBR_MORE)
		abbr_more(&t);
	literal_rules(&t);
	static const int KINDS[] = {SURNAME_INITIALS, INITIALS_SURNAME, SURNAME_SINGLE,
	                            SINGLE_SURNAME};
	static const int PAT[] = {RN_ABBREVIATIONS_PERSON_SURNAME_INITIALS_PATTERN,
	                          RN_ABBREVIATIONS_PERSON_INITIALS_SURNAME_PATTERN,
	                          RN_ABBREVIATIONS_PERSON_SURNAME_SINGLE_INITIAL_PATTERN,
	                          RN_ABBREVIATIONS_PERSON_SINGLE_INITIAL_SURNAME_PATTERN};
	for (int k = 0; k < 4; k++)
		nx_resub(rn_pat(PAT[k]), &t, initials_repl, (void *)&KINDS[k]);
	if (rn_resub_tmpl(P(L1173), &t, (const uint32_t *)U" ") < 0)
		nx_fail();
	if (rn_fixes & RN_FIX_ABBR_MIXED)
		mixed_abbr(&t);
	tokctx dotted = {1}, solid = {0};
	nx_resub(P(ABBREV_DOTTED_PATTERN), &t, token_repl, &dotted);
	nx_resub(P(ABBREV_SOLID_PATTERN), &t, token_repl, &solid);
	int amp = 0;
	for (size_t i = 0; i < t.n && !amp; i++)
		amp = t.s[i] == '&';
	if (amp)
		nx_resub(P(WORD_AMPERSAND_PATTERN), &t, amp_repl, NULL);
	int r = rn_str_put(out, t.s, t.n);
	rn_str_free(&t);
	return r < 0 || rn_mx_failed ? -1 : 0;
}
