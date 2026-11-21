/* numerals/cardinals.py: normalize_remaining_post_numeral_abbreviations ("5 тыс." left after
 * the cardinal pass) and normalize_all_digits_everywhere */
#include "rn-npass.h"
#include "rn-nx.h"
#include "rn-pat.h"

#define PUNCT_STRIP U".,:;!\"«»()[]{}"

static const char *const MULTIPLIERS[] = {"тысяча", "миллион", "миллиард", "триллион", NULL};

static const struct {
	const char32_t *key, *repl;
	int counted;   /* _COUNTED_POST_NUMERAL_ABBREVIATIONS */
} ABBR[] = {
	{U"тыс.", U"тысяч", 0},          {U"млн.", U"миллионов", 0}, {U"млрд.", U"миллиардов", 0},
	{U"трлн.", U"триллионов", 0},    {U"руб.", U"рублей", 1},    {U"долл.", U"долларов", 1},
	{U"дол.", U"долларов", 1},       {U"и др.", U"и другие", 0},
};

/* _counted_quantity_precedes(text, start) */
static int counted_quantity(const uint32_t *T, size_t start)
{
	size_t a = nx_left(start, 32);
	rn_tok *L = NULL;
	long n = rn_simple_tokenize(T + a, start - a, &L);
	if (n < 0) {
		nx_fail();
		return 0;
	}
	int r = 0;
	rn_str w = {0};
	for (long i = n - 1; i >= 0; i--) {
		nx_lower(L[i].s, L[i].n, &w);
		size_t x = 0, y = w.n;
		while (x < y && nx_inset(w.s[x], PUNCT_STRIP))
			x++;
		while (y > x && nx_inset(w.s[y - 1], PUNCT_STRIP))
			y--;
		if (x == y)
			continue;
		if (rn_py_str_isdigit(w.s + x, y - x)) {
			r = 1;
			break;
		}
		const cmorphy3_form *f;
		int np = rn_mparse(w.s + x, y - x, &f);
		int p0 = rn_pos(f[0].tag);
		if (p0 == rn_g.PREP || p0 == rn_g.CONJ || p0 == rn_g.PRCL)
			break;
		for (int j = 0; j < np && !r; j++) {
			r = rn_pos(f[j].tag) == rn_g.NUMR;
			for (const char *const *m = MULTIPLIERS; *m && !r; m++)
				r = !strcmp(f[j].normal_form, *m);
		}
		for (int j = 0; j < np && !r; j++) {
			int c = rn_case(f[j].tag);
			r = rn_has(f[j].tag, rn_g.NOUN) && rn_has(f[j].tag, rn_g.plur) &&
			    (c == rn_g.nomn || c == rn_g.accs || c == rn_g.gent);
		}
		break;
	}
	rn_str_free(&w);
	free(L);
	return r;
}

static int abbr_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const uint32_t *T = m->s;
	size_t st = m->start[0], en = m->end[0];
	rn_str f = {0}, key = {0};
	if (rn_py_casefold(T + st, en - st, &f) < 0)
		nx_fail();
	/* " ".join(s.split()) */
	for (size_t i = 0; i < f.n;) {
		while (i < f.n && rn_py_isspace(f.s[i]))
			i++;
		size_t j = i;
		while (j < f.n && !rn_py_isspace(f.s[j]))
			j++;
		if (j > i) {
			if (key.n)
				nx_putc(&key, ' ');
			nx_put(&key, f.s + i, j - i);
		}
		i = j;
	}
	size_t k = 0;
	while (k < sizeof ABBR / sizeof *ABBR && !nx_eq(key.s, key.n, ABBR[k].key))
		k++;
	rn_str_free(&f);
	rn_str_free(&key);
	if (k == sizeof ABBR / sizeof *ABBR) {
		/* the pattern only matches the keys */
		nx_fail();
		return nx_whole(m, o);
	}
	if (ABBR[k].counted && !counted_quantity(T, st))
		return nx_whole(m, o);
	nx_lit(o, ABBR[k].repl);
	size_t i = en;
	while (i < m->n && rn_py_isspace(T[i]))
		i++;
	if (i >= m->n || nx_inset(T[i], U"\n.!?…"))
		nx_putc(o, '.');
	return 0;
}

int rn_n_remaining_post_numeral_abbreviations(rn_str *t)
{
	nx_resub(rn_pat(RN_NUMERALS_CONSTANTS_POST_NUMERAL_ABBREVIATION_PATTERN), t, abbr_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

static int digits_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t n = m->end[0] - m->start[0];
	char *d = malloc(n + 3);
	rn_str r = {0};
	if (d && nx_pyint(m->s + m->start[0], n, d, n + 3) >= 0 && nx_n2w(&r, 0, d, N2W_DEFAULTS) == 0)
		nx_put(o, r.s, r.n);
	else
		nx_whole(m, o);
	if (!d)
		nx_fail();
	free(d);
	rn_str_free(&r);
	return 0;
}

int rn_n_all_digits_everywhere(rn_str *t)
{
	nx_resub(rn_pat(RN_NUMERALS_CONSTANTS_DIGIT_PATTERN), t, digits_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}
