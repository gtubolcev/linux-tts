/* normalizer.py run_numerals and numerals/__init__.py normalize_numerals: the numerals
 * stage, the passes of rn-npass.h in the oracle's order */
#include "rn-npass.h"
#include "rn-nx.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-ph.h"

/* _contains_numeral_candidates */
static int numeral_candidates(const rn_str *t)
{
	static const char32_t TRIGGERS[] =
	    U"αβγδεϵζηθϑικϰλμµνξοπϖρϱσςτφϕχψωΔΣΩΠΦΓΛΘ"   /* GREEK_LETTERS */
	    U"×÷±∓≈≠≤≥∞√∑∏∫∂∈∉∪∩⇒№"                     /* MATH_SYMBOLS */
	    U"$€£¥₽₴₸₺₹¢";                                /* CURRENCY_STANDALONE */
	for (size_t i = 0; i < t->n; i++)
		if (rn_py_isnumeric(t->s[i]) || nx_inset(t->s[i], TRIGGERS))
			return 1;
	static const char32_t *const HINTS[] = {U"тыс", U"млн", U"млрд", U"трлн", U"руб",
	                                        U"долл", U"дол", U"др.", NULL};
	rn_str f = {0};
	if (rn_py_casefold(t->s, t->n, &f) < 0)
		nx_fail();
	int r = 0;
	for (const char32_t *const *h = HINTS; *h && !r; h++) {
		size_t k = 0;
		while ((*h)[k])
			k++;
		r = f.n && rn_py_find(f.s, f.n, (const uint32_t *)*h, k, 0) >= 0;
	}
	rn_str_free(&f);
	return r;
}

/* normalize_numerals(text) in place */
static int normalize_numerals(rn_str *t)
{
	if (!numeral_candidates(t))
		return 0;
	rn_n_heading_ranges(t);
	rn_n_heading_numbers(t);
	rn_n_numeric_unit_ranges(t);
	rn_n_math_symbols(t);
	rn_n_cardinal_numerals(t);
	rn_n_remaining_post_numeral_abbreviations(t);
	rn_n_greek_letters(t);
	rn_n_standalone_currency(t);
	rn_n_all_digits_everywhere(t);
	return rn_mx_failed ? -1 : 0;
}

static int protect_repl(const rn_match *m, rn_str *out, void *ctx)
{
	size_t n;
	const uint32_t *s = rn_group(m, 0, &n);
	long p = rn_ph_alloc(ctx, s, n);
	if (p < 0)
		return -1;
	return p ? rn_str_putc(out, (uint32_t)p) : rn_str_put(out, s, n);
}

int rn_run_numerals(const rn_str *in, rn_str *out)
{
	rn_mx_trim();
	rn_mx_failed = 0;
	rn_str t = {0};
	if (rn_str_put(&t, in->s ? in->s : (const uint32_t *)U"", in->n) < 0)
		return -1;
	if (!numeral_candidates(&t)) {
		int r = rn_str_put(out, t.s, t.n);
		rn_str_free(&t);
		return r < 0 || rn_mx_failed ? -1 : 0;
	}
	rn_ph ph = {0};
	int err = rn_ph_init(&ph, RN_PH_NUMERAL_TOKENS, t.s, t.n) < 0;
	if (!err) {
		rn_str p = {0};
		if (rn_sub(rn_pat(RN_IDENTIFIERS_STRUCTURED_NUMERIC_SUFFIX_PATTERN), t.s, t.n,
		           protect_repl, &ph, 0, &p) < 0)
			err = 1;
		rn_str_free(&t);
		t = p;
		if (!t.s)
			rn_str_put(&t, (const uint32_t *)U"", 0);
	}
	if (!err) {
		rn_n_math_symbols(&t);
		rn_n_spaced_numeric_hyphen_words(&t);
		rn_n_thousands_abbreviated_attributes(&t);
		rn_n_edition_ordinals(&t);
		rn_n_prefixed_ordinal_adverbs(&t);
		rn_n_numeric_unit_hyphen_links(&t);
		rn_n_compound_numeric_adjectives(&t);
		rn_n_square_unit_attributes(&t);
		rn_n_versions(&t);
		rn_n_decimals(&t);
		rn_n_fractions(&t);
		rn_n_numeric_suffixes_ex(&t, 1);   /* enable_caps_normalization of tts() */
		rn_n_hyphenated_words(&t);         /* include_numeric_suffixes=False */
		normalize_numerals(&t);
		if (rn_ph_restore(&ph, t.s, t.n, out) < 0)
			err = 1;
	}
	rn_ph_free(&ph);
	rn_str_free(&t);
	return err || rn_mx_failed ? -1 : 0;
}
