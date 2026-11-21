/* _run_preprocess_steps of normalizer.py: the helpers of preprocess_utils.py and quotes.py.
 * Each rn_pre_* appends the result for in to out (0, -1 on error), as the Python function
 * of the same name returns it. */
#include <string.h>

#include "rn-morph.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define N(a) (sizeof a / sizeof *a)
#define PU(id) rn_pat(RN_PREPROCESS_UTILS_##id)
#define PQ(id) rn_pat(RN_QUOTES_##id)

static int copy(const rn_str *in, rn_str *out)
{
	return rn_str_put(out, in->s, in->n);
}

static int has(const rn_str *s, uint32_t c)
{
	for (size_t i = 0; i < s->n; i++)
		if (s->s[i] == c)
			return 1;
	return 0;
}

static int put_group(const rn_match *m, int g, rn_str *out)
{
	size_t n;
	const uint32_t *s = rn_group(m, g, &n);
	return rn_str_put(out, s, n);
}

/* out += pattern.sub(tmpl, in) */
static int sub_tmpl(const rn_re *re, const rn_str *in, const uint32_t *tmpl, rn_str *out)
{
	return rn_sub_tmpl(re, in->s, in->n, tmpl, 0, out) < 0 ? -1 : 0;
}

/* runs steps on a working copy: t = in; each step rewrites t; out += t */
#define WORK_BEGIN(in)                                                                          \
	rn_str t = {0};                                                                         \
	int r = -1;                                                                             \
	if (rn_str_put(&t, (in)->s, (in)->n) < 0)                                               \
		goto done;
#define WORK_END(out)                                                                           \
	r = rn_str_put(out, t.s, t.n);                                                          \
	done:                                                                                   \
	rn_str_free(&t);                                                                        \
	return r;

/* t = f(t) for a pass function */
static int apply(int (*f)(const rn_str *, rn_str *), rn_str *t)
{
	rn_str u = {0};
	if (f(t, &u) < 0) {
		rn_str_free(&u);
		return -1;
	}
	rn_str_free(t);
	*t = u;
	return 0;
}

/* ---- simple substitutions ---- */

int rn_pre_remove_decorative_separators(const rn_str *in, rn_str *out)
{
	return sub_tmpl(PU(DECORATIVE_SEPARATOR_PATTERN), in, RN_U(""), out);
}

int rn_pre_remove_zero_width_formatting(const rn_str *in, rn_str *out)
{
	return sub_tmpl(PQ(ZERO_WIDTH_FORMATTING_PATTERN), in, RN_U(""), out);
}

int rn_pre_normalize_cyrillic_combining_stress_marks(const rn_str *in, rn_str *out)
{
	return sub_tmpl(PQ(CYRILLIC_COMBINING_STRESS_PATTERN), in, RN_U("+\1"), out);
}

static int repl_drop_underscores(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	for (size_t i = m->start[0]; i < m->end[0]; i++)
		if (m->s[i] != '_' && rn_str_putc(out, m->s[i]) < 0)
			return -1;
	return 0;
}

int rn_pre_collapse_letterspaced_emphasis(const rn_str *in, rn_str *out)
{
	if (!has(in, '_'))
		return copy(in, out);
	return rn_sub(PQ(LETTERSPACED_UNDERSCORE_PATTERN), in->s, in->n, repl_drop_underscores, NULL,
	              0, out) < 0 ? -1 : 0;
}

int rn_pre_protect_letter_hyphens(const rn_str *in, rn_str *out)
{
	static const uint32_t ph[] = {RN_LETTER_HYPHEN, 0};
	return sub_tmpl(PU(LETTER_HYPHEN_PATTERN), in, ph, out);
}

int rn_pre_restore_letter_hyphens(const rn_str *in, rn_str *out)
{
	for (size_t i = 0; i < in->n; i++)
		if (rn_str_putc(out, in->s[i] == RN_LETTER_HYPHEN ? '-' : in->s[i]) < 0)
			return -1;
	return 0;
}

static int repl_binary_op(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	uint32_t c = m->s[m->start[0]];
	return rn_str_putc(out, c == '+' ? RN_BINARY_PLUS : c == '^' ? RN_BINARY_POWER
	                                                         : RN_BINARY_MINUS);
}

int rn_pre_protect_binary_math_operators(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(BINARY_MATH_OPERATOR_PATTERN), in->s, in->n, repl_binary_op, NULL, 0, out) < 0
	       ? -1 : 0;
}

int rn_pre_restore_binary_math_operators(const rn_str *in, rn_str *out)
{
	for (size_t i = 0; i < in->n; i++) {
		uint32_t c = in->s[i];
		c = c == RN_BINARY_PLUS ? '+' : c == RN_BINARY_MINUS ? 0x2212 : c == RN_BINARY_POWER ? '^' : c;
		if (rn_str_putc(out, c) < 0)
			return -1;
	}
	return 0;
}

int rn_pre_normalize_explicit_dashes(const rn_str *in, rn_str *out)
{
	for (size_t i = 0; i < in->n; i++) {
		uint32_t c = in->s[i];
		if (rn_str_putc(out, c == 0x2013 || c == 0x2015 ? 0x2014 : c) < 0)
			return -1;
	}
	return 0;
}

int rn_pre_space_abbreviation_dot_before_digit(const rn_str *in, rn_str *out)
{
	return sub_tmpl(PU(ABBREVIATION_DOT_BEFORE_DIGIT_PATTERN), in, RN_U("\1. "), out);
}

int rn_pre_normalize_unary_plus_numbers(const rn_str *in, rn_str *out)
{
	return sub_tmpl(PU(UNARY_PLUS_NUMBER_PATTERN), in, RN_U("\1плюс \2"), out);
}

int rn_pre_expand_na_prim_abbreviation(const rn_str *in, rn_str *out)
{
	return sub_tmpl(PU(NA_PRIM_ABBREVIATION_PATTERN), in, RN_U("\1пример"), out);
}

static int repl_new_style(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *d = rn_group(m, 1, &n);
	int v = 0;
	for (size_t i = 0; i < n; i++)
		v = v * 10 + rn_py_decimal(d[i]);
	if (v > 31)
		return put_group(m, 0, out);
	return rn_str_put(out, d, n) < 0 || rn_puts(out, RN_U("-го нового стиля")) < 0 ? -1 : 0;
}

int rn_pre_expand_new_style_date_abbreviation(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(NEW_STYLE_DATE_PATTERN), in->s, in->n, repl_new_style, NULL, 0, out) < 0
	       ? -1 : 0;
}

static int repl_years_ago(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t e = m->end[0];
	int dot = e >= m->n || m->s[e] == '\n' || rn_py_isalnum(m->s[e]);
	if (put_group(m, 1, out) < 0 || rn_puts(out, dot ? RN_U("лет назад.") : RN_U("лет назад")) < 0)
		return -1;
	return put_group(m, 2, out);
}

int rn_pre_expand_years_ago_abbreviation(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(YEARS_AGO_ABBREVIATION_PATTERN), in->s, in->n, repl_years_ago, NULL, 0, out) < 0
	       ? -1 : 0;
}

static int repl_arc(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t sn;
	const uint32_t *sec = rn_group(m, 2, &sn);
	if (rn_puts(out, RN_U("° ")) < 0 || put_group(m, 1, out) < 0 || rn_puts(out, RN_U(" мин")) < 0)
		return -1;
	if (sn && (rn_str_putc(out, ' ') < 0 || rn_str_put(out, sec, sn) < 0 ||
	           rn_puts(out, RN_U(" сек")) < 0))
		return -1;
	return 0;
}

int rn_pre_expand_arc_minutes(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(ARC_MINUTE_PATTERN), in->s, in->n, repl_arc, NULL, 0, out) < 0 ? -1 : 0;
}

static int repl_age(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	rn_match c;
	size_t a = m->start[0] > 64 ? m->start[0] - 64 : 0;
	int r = rn_search(PU(L349), m->s + a, m->start[0] - a, 0, &c);
	if (r < 0)
		return -1;
	if (!r)
		return put_group(m, 0, out);
	return rn_puts(out, RN_U("от ")) < 0 || put_group(m, 1, out) < 0 ||
	       rn_puts(out, RN_U(" до ")) < 0 || put_group(m, 2, out) < 0 ||
	       rn_str_putc(out, ' ') < 0 || put_group(m, 3, out) < 0 ? -1 : 0;
}

int rn_pre_normalize_age_ranges(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(AGE_RANGE_PATTERN), in->s, in->n, repl_age, NULL, 0, out) < 0 ? -1 : 0;
}

static int repl_negative(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t pn, sn;
	const uint32_t *prefix = rn_group(m, 1, &pn);
	const uint32_t *sign = rn_group(m, 2, &sn);
	if (rn_py_isspace(sign[sn - 1])) {
		size_t b = m->start[2];
		while (b > 0 && rn_py_isspace(m->s[b - 1]))
			b--;
		uint32_t last = b ? m->s[b - 1] : 0;
		if (b && (last == ',' || last == ';' || last == ':'))
			return put_group(m, 0, out);
		int prefix_space = pn > 0;
		for (size_t i = 0; i < pn; i++)
			prefix_space &= rn_py_isspace(prefix[i]);
		if (prefix_space && b && !(last == '(' || last == '=' || last == '[' || last == '{' ||
		                           last == 0xAB))
			return put_group(m, 0, out);
	}
	return rn_str_put(out, prefix, pn) < 0 || rn_str_putc(out, RN_NEGATIVE_NUMBER) < 0 ||
	       put_group(m, 3, out) < 0 ? -1 : 0;
}

int rn_pre_protect_negative_numbers(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(UNARY_MINUS_NUMBER_PATTERN), in->s, in->n, repl_negative, NULL, 0, out) < 0
	       ? -1 : 0;
}

/* ---- str.replace chains ---- */

int rn_pre_apply_cleanup_replacements(const rn_str *in, rn_str *out)
{
	static const struct {
		const char32_t *old, *new;
	} R[] = {
		{U"--", U"—"}, {U",,", U","}, {U".\n", U". \n"}, {U"©", U""}, {U"®", U""},
		{U"™", U""}, {U"−", U"-"}, {U"–", U"—"}, {U"‐", U"-"}, {U"‑", U"-"},
		{U"−", U"-"}, {U" ", U" "}, {U" ", U" "}, {U" ", U" "},
		{U" ", U" "}, {U" ", U" "}, {U" ", U" "}, {U" ", U" "},
		{U"\t", U" "},
	};
	WORK_BEGIN(in)
	for (size_t i = 0; i < N(R); i++)
		if (rn_py_rereplace(&t, (const uint32_t *)R[i].old, (const uint32_t *)R[i].new) < 0)
			goto done;
	WORK_END(out)
}

int rn_pre_normalize_unicode_fractions(const rn_str *in, rn_str *out)
{
	static const struct {
		uint32_t c;
		const char32_t *w;
	} F[] = {
		{0xBC, U"1/4"}, {0xBD, U"1/2"}, {0xBE, U"3/4"}, {0x2153, U"1/3"}, {0x2154, U"2/3"},
		{0x215B, U"1/8"}, {0x215C, U"3/8"}, {0x215D, U"5/8"}, {0x215E, U"7/8"},
	};
	WORK_BEGIN(in)
	for (size_t k = 0; k < N(F); k++) {
		if (!has(&t, F[k].c))
			continue;
		/* re.sub(rf"(?<=\d){char}", f" {repl}") then text.replace(char, repl): one pass,
		 * the look-behind sees the text before the pass */
		rn_str u = {0};
		for (size_t i = 0; i < t.n; i++) {
			int bad;
			if (t.s[i] != F[k].c)
				bad = rn_str_putc(&u, t.s[i]);
			else
				bad = (i && rn_py_isdecimal(t.s[i - 1]) && rn_str_putc(&u, ' ') < 0) ||
				      rn_puts(&u, (const uint32_t *)F[k].w) < 0;
			if (bad < 0 || bad == 1) {
				rn_str_free(&u);
				goto done;
			}
		}
		rn_str_free(&t);
		t = u;
	}
	WORK_END(out)
}

/* ---- line breaks ---- */

static int keeps_no_dot(uint32_t c)
{
	static const char32_t S[] = U"\n.!?…,:;\"'-–—―‐‑−";
	for (const char32_t *p = S; *p; p++)
		if ((uint32_t)*p == c)
			return 1;
	return 0;
}

static int repl_linebreak_run(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	int breaks = 0;
	for (size_t i = m->start[0]; i < m->end[0]; i++)
		breaks += m->s[i] == '\n';
	const uint32_t *lb = breaks >= 2 ? RN_U("\n\n") : RN_U("\n");
	uint32_t prev = m->start[0] ? m->s[m->start[0] - 1] : 0;
	int has_prev = m->start[0] > 0;
	int tail = m->end[0] < m->n;
	rn_match x;
	int legacy = rn_match_at(PU(LEGACY_LINE_START_PATTERN), m->s, m->n, m->end[0], &x);
	if (legacy < 0)
		return -1;
	if (has_prev && !keeps_no_dot(prev) && legacy)
		return rn_str_putc(out, '.') < 0 ? -1 : rn_puts(out, lb);
	if (breaks == 1 && has_prev && tail && !legacy)
		return rn_str_putc(out, ' ');
	return rn_puts(out, lb);
}

int rn_pre_normalize_linebreaks(const rn_str *in, rn_str *out)
{
	if (!has(in, '\n') && !has(in, '\r'))
		return copy(in, out);
	WORK_BEGIN(in)
	if (rn_py_rereplace(&t, RN_U("\r\n"), RN_U("\n")) < 0 ||
	    rn_py_rereplace(&t, RN_U("\r"), RN_U("\n")) < 0 ||
	    rn_resub_tmpl(PU(TRAILING_SPACE_BEFORE_NEWLINE_PATTERN), &t, RN_U("\n")) < 0 ||
	    rn_resub(PU(LINEBREAK_RUN_PATTERN), &t, repl_linebreak_run, NULL) < 0)
		goto done;
	WORK_END(out)
}

/* ---- morphology ---- */

/* word.lower() and word_is_known() of it, as rn's word_is_known does anyway */
static int known(const uint32_t *s, size_t n)
{
	return rn_word_is_known(s, n);
}

static int repl_wrapped(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t ln, rn_;
	const uint32_t *l = rn_group(m, 1, &ln);
	const uint32_t *r = rn_group(m, 2, &rn_);
	rn_str w = {0};
	int res = -1, hy = 0;
	if (rn_str_put(&w, l, ln) < 0 || rn_str_put(&w, r, rn_) < 0)
		goto done;
	if (!known(w.s, w.n)) {
		w.n = 0;
		if (rn_str_put(&w, l, ln) < 0 || rn_str_putc(&w, '-') < 0 || rn_str_put(&w, r, rn_) < 0)
			goto done;
		hy = known(w.s, w.n) || (known(l, ln) && known(r, rn_));
	}
	res = rn_str_put(out, l, ln) < 0 || (hy && rn_str_putc(out, '-') < 0) ||
	      rn_str_put(out, r, rn_) < 0 ? -1 : 0;
done:
	rn_str_free(&w);
	return res;
}

int rn_pre_join_hyphenated_line_breaks(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(WRAPPED_WORD_HYPHEN_PATTERN), in->s, in->n, repl_wrapped, NULL, 0, out) < 0
	       ? -1 : 0;
}

static int paren_letter(uint32_t c)
{
	return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451 || c == 0x462 || c == 0x463 ||
	       c == 0x406 || c == 0x456 || c == 0x472 || c == 0x473 || c == 0x474 || c == 0x475;
}

static int repl_paren(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	rn_match inner;
	int r = rn_match_at(PU(PAREN_INNER_WORD_PATTERN), m->s, m->n, m->end[0], &inner);
	if (r < 0)
		return -1;
	if (r) {
		size_t pos = m->start[0], start = pos;
		while (start && paren_letter(m->s[start - 1]))
			start--;
		size_t in_n;
		const uint32_t *w = rn_group(&inner, 1, &in_n);
		rn_str word = {0};
		if (rn_str_put(&word, m->s + start, pos - start) < 0 || rn_str_put(&word, w, in_n) < 0) {
			rn_str_free(&word);
			return -1;
		}
		int k = known(word.s, word.n);
		rn_str_free(&word);
		if (k)
			return put_group(m, 0, out);
	}
	return rn_puts(out, RN_U(" ("));
}

int rn_pre_insert_missing_space_before_paren(const rn_str *in, rn_str *out)
{
	WORK_BEGIN(in)
	if (rn_resub(PU(MISSING_SPACE_BEFORE_PAREN_PATTERN), &t, repl_paren, NULL) < 0 ||
	    rn_resub_tmpl(PU(MISSING_SPACE_BEFORE_LETTER_PAREN_AFTER_DIGIT_PATTERN), &t, RN_U(" (")) < 0)
		goto done;
	WORK_END(out)
}

static const char32_t HOMO_FROM[] = U"ABCEHKMOPTXYacepoxy";
static const char32_t HOMO_TO[] = U"АВСЕНКМОРТХУасероху";

static int repl_homoglyph(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const uint32_t *w = m->s + m->start[0];
	size_t n = m->end[0] - m->start[0];
	int latin = 0, other_alpha = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = w[i];
		if (c < 0x80 && rn_is_ascii_alpha(c)) {
			latin = 1;
			const char32_t *p = HOMO_FROM;
			while (*p && (uint32_t)*p != c)
				p++;
			if (!*p)
				return rn_str_put(out, w, n);
		} else if (c >= 0x80 && rn_py_isalpha(c)) {
			other_alpha = 1;
		}
	}
	if (!latin || !other_alpha)
		return rn_str_put(out, w, n);
	rn_str cand = {0}, plain = {0};
	int res = -1;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = w[i];
		for (size_t k = 0; HOMO_FROM[k]; k++)
			if ((uint32_t)HOMO_FROM[k] == c) {
				c = HOMO_TO[k];
				break;
			}
		if (rn_str_putc(&cand, c) < 0 || (c != '+' && rn_str_putc(&plain, c) < 0))
			goto done;
	}
	res = known(plain.s ? plain.s : RN_U(""), plain.n) ? rn_str_put(out, cand.s, cand.n)
	                                                : rn_str_put(out, w, n);
done:
	rn_str_free(&cand);
	rn_str_free(&plain);
	return res;
}

int rn_pre_normalize_script_homoglyphs(const rn_str *in, rn_str *out)
{
	rn_match m;
	int r = rn_search(PQ(HOMOGLYPH_LATIN_PATTERN), in->s, in->n, 0, &m);
	if (r <= 0)
		return r < 0 ? -1 : copy(in, out);
	return rn_sub(PQ(MIXED_SCRIPT_WORD_PATTERN), in->s, in->n, repl_homoglyph, NULL, 0, out) < 0
	       ? -1 : 0;
}

/* ---- quotes ---- */

static int quote_glue_letter(uint32_t c)
{
	return rn_is_ascii_alpha(c) || (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451;
}

static int insert_missing_quote_spacing(const rn_str *in, rn_str *out)
{
	const uint32_t *s = in->s;
	size_t n = in->n;
	if (!has(in, '"'))
		return copy(in, out);
	size_t last = 0;
	/* quotes line by line: a line's quotes are paired when their count is even */
	for (size_t ls = 0; ls <= n;) {
		size_t le = ls;
		size_t count = 0;
		while (le < n && s[le] != '\n')
			count += s[le++] == '"';
		/* the Python groups quotes into runs without a newline between them, the same */
		int paired = count % 2 == 0;
		size_t index = 0;
		for (size_t p = ls; p < le; p++) {
			if (s[p] != '"')
				continue;
			uint32_t before = p ? s[p - 1] : 0, after = p + 1 < n ? s[p + 1] : 0;
			int has_before = p > 0, has_after = p + 1 < n;
			int between = paired && has_before && quote_glue_letter(before) && has_after &&
			              quote_glue_letter(after);
			size_t ins = (size_t)-1;
			if (paired && index % 2) {
				if (between)
					ins = p + 1;
			} else {
				int after_punct = has_before && (before == ',' || before == ';' || before == ':') &&
				                  has_after && !rn_py_isspace(after) && after != '"';
				if (between || after_punct)
					ins = p;
			}
			index++;
			if (ins != (size_t)-1) {
				if (rn_str_put(out, s + last, ins - last) < 0 || rn_str_putc(out, ' ') < 0)
					return -1;
				last = ins;
			}
		}
		if (le == n)
			break;
		ls = le + 1;
	}
	return rn_str_put(out, s + last, n - last);
}

int rn_pre_normalize_ascii_quote_pairs(const rn_str *in, rn_str *out)
{
	static const char32_t CAND[] = U"\"'`´«»‘’‚‛‹›“”„‟〝〞＂";
	int any = 0;
	for (size_t i = 0; i < in->n && !any; i++)
		for (const char32_t *p = CAND; *p; p++)
			if ((uint32_t)*p == in->s[i]) {
				any = 1;
				break;
			}
	if (!any)
		return copy(in, out);
	WORK_BEGIN(in)
	rn_match m;
	int s = rn_search(PQ(QUOTE_SHAPE_PATTERN), t.s, t.n, 0, &m);
	if (s < 0)
		goto done;
	if (s) {
		static const char32_t *const PAIRS[] = {U"``", U"''", U"‘‘", U"’’", U"´´"};
		for (size_t i = 0; i < N(PAIRS); i++)
			if (rn_py_rereplace(&t, (const uint32_t *)PAIRS[i], RN_U("\"")) < 0)
				goto done;
		if (rn_resub_tmpl(PQ(QUOTE_VARIANT_PATTERN), &t, RN_U("\"")) < 0 ||
		    rn_resub_tmpl(PQ(OPEN_SINGLE_QUOTE_PATTERN), &t, RN_U("\1\"")) < 0 ||
		    rn_resub_tmpl(PQ(CLOSE_SINGLE_QUOTE_PATTERN), &t, RN_U("\"")) < 0)
			goto done;
	}
	if (rn_resub_tmpl(PQ(SPACE_INSIDE_QUOTES_PATTERN), &t, RN_U("\1\"\2\"")) < 0 ||
	    rn_resub_tmpl(PQ(SPACE_AFTER_OPEN_QUOTE_PATTERN), &t, RN_U("\1\"")) < 0 ||
	    rn_resub_tmpl(PQ(SPACE_BEFORE_CLOSE_QUOTE_PATTERN), &t, RN_U("\"")) < 0 ||
	    apply(insert_missing_quote_spacing, &t) < 0)
		goto done;
	WORK_END(out)
}

/* ---- punctuation ---- */

static int repl_space_before(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	return rn_str_putc(out, ' ') < 0 ? -1 : put_group(m, 0, out);
}

int rn_pre_normalize_punctuation_spacing(const rn_str *in, rn_str *out)
{
	WORK_BEGIN(in)
	if (rn_resub_tmpl(PU(ELLIPSIS_SPACE_BEFORE_PATTERN), &t, RN_U("")) < 0 ||
	    rn_resub(PU(ELLIPSIS_SPACE_AFTER_PATTERN), &t, repl_space_before, NULL) < 0 ||
	    rn_resub_tmpl(PU(SENTENCE_SPACE_AFTER_PATTERN), &t, RN_U(" ")) < 0 ||
	    rn_resub_tmpl(PU(DOT_SPACE_DOT_TYPO_PATTERN), &t, RN_U(". ")) < 0 ||
	    rn_resub_tmpl(PU(SPACE_BEFORE_PUNCT_PATTERN), &t, RN_U("\1")) < 0 ||
	    rn_resub_tmpl(PU(DOUBLE_DOT_TYPO_PATTERN), &t, RN_U(".")) < 0 ||
	    rn_resub_tmpl(PU(COMMA_DOT_TYPO_PATTERN), &t, RN_U(".")) < 0 ||
	    rn_resub_tmpl(rn_pat(RN_MATH_FORMULAS_MULTI_SPACE_RE), &t, RN_U(" ")) < 0)
		goto done;
	WORK_END(out)
}
