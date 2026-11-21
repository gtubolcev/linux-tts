/* normalizer.py: _run_preprocess_steps (without caps, as normalize_text calls it),
 * _run_caps_normalization and remove_numeric_footnotes, for NormalizeOptions.tts() */
#include <stdlib.h>
#include <string.h>

#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define NZ(id) rn_pat(RN_NORMALIZER_##id)
#define PU(id) rn_pat(RN_PREPROCESS_UTILS_##id)
#define UNIT_SLASH_PLACEHOLDER 0xE003

/* t = f(t) */
static int step(int (*f)(const rn_str *, rn_str *), rn_str *t)
{
	rn_str u = {0};
	if (f(t, &u) < 0) {
		rn_str_free(&u);
		return -1;
	}
	if (!u.s && rn_str_put(&u, NULL, 0) < 0)
		return -1;
	rn_str_free(t);
	*t = u;
	return 0;
}

static int replace(rn_str *t, const uint32_t *old, const uint32_t *new)
{
	return rn_py_rereplace(t, old, new);
}

static int superscripts(rn_str *t)
{
	static const uint32_t SUP[] = {0x2070, 0xB9, 0xB2, 0xB3, 0x2074, 0x2075, 0x2076, 0x2077,
	                               0x2078, 0x2079};
	rn_match m;
	int k = rn_search(NZ(SUPERSCRIPT_DIGIT_PATTERN), t->s, t->n, 0, &m);
	if (k <= 0)
		return k;
	if (step(rn_pre_drop_superscript_footnote_markers, t) < 0)
		return -1;
	for (size_t i = 0; i < t->n; i++) {
		uint32_t c = t->s[i];
		if (c >= 0x2080 && c <= 0x2089)
			t->s[i] = '0' + (c - 0x2080);
		else
			for (int d = 0; d < 10; d++)
				if (c == SUP[d])
					t->s[i] = '0' + d;
	}
	return 0;
}

int rn_run_preprocess(const rn_str *in, rn_str *out)
{
	rn_str t = {0};
	const uint32_t *s = in->s ? in->s : RN_U("");
	size_t a = 0;
	while (a < in->n && rn_py_isspace(s[a]))
		a++;
	int r = -1;
	if (a < in->n && s[a] == '+') {
		if (rn_str_putc(&t, ' ') < 0 || rn_str_put(&t, s + a, in->n - a) < 0)
			goto done;
	} else if (rn_str_put(&t, s, in->n) < 0) {
		goto done;
	}
	if (!t.s && rn_str_put(&t, NULL, 0) < 0)
		goto done;
	if (replace(&t, RN_U("\r\n"), RN_U("\n")) < 0 || replace(&t, RN_U("\r"), RN_U("\n")) < 0 ||
	    step(rn_pre_normalize_international_phone_numbers, &t) < 0 ||
	    step(rn_pre_remove_decorative_separators, &t) < 0 ||
	    rn_resub_tmpl(NZ(DECORATIVE_MARKER_PATTERN), &t, RN_U(" ")) < 0 ||
	    rn_resub_tmpl(NZ(ASTERISK_SEPARATOR_PATTERN), &t, RN_U(" ")) < 0 ||
	    step(rn_pre_join_hyphenated_line_breaks, &t) < 0 ||
	    step(rn_pre_normalize_linebreaks, &t) < 0 ||
	    step(rn_pre_remove_zero_width_formatting, &t) < 0 ||
	    step(rn_pre_normalize_cyrillic_combining_stress_marks, &t) < 0 ||
	    step(rn_pre_normalize_script_homoglyphs, &t) < 0 ||
	    step(rn_pre_collapse_letterspaced_emphasis, &t) < 0 ||
	    step(rn_pre_normalize_labelled_identifiers, &t) < 0 ||
	    step(rn_pre_expand_address_abbreviations, &t) < 0 ||
	    step(rn_pre_protect_letter_hyphens, &t) < 0 ||
	    replace(&t, RN_U("\x25E6"), RN_U(" ")) < 0 || replace(&t, RN_U("["), RN_U("(")) < 0 ||
	    replace(&t, RN_U("]"), RN_U(")")) < 0 || step(rn_pre_protect_unit_slashes, &t) < 0 ||
	    rn_resub_tmpl(NZ(SLASH_FIX_PATTERN), &t, RN_U(" ")) < 0 ||
	    replace(&t, (const uint32_t[]){UNIT_SLASH_PLACEHOLDER, 0}, RN_U("/")) < 0 ||
	    step(rn_pre_normalize_ascii_quote_pairs, &t) < 0 ||
	    step(rn_pre_normalize_punctuation_spacing, &t) < 0 ||
	    step(rn_pre_insert_missing_space_before_paren, &t) < 0 ||
	    step(rn_pre_expand_years_ago_abbreviation, &t) < 0 ||
	    step(rn_pre_expand_na_prim_abbreviation, &t) < 0 ||
	    step(rn_pre_expand_new_style_date_abbreviation, &t) < 0 ||
	    step(rn_pre_restore_non_normative_day_suffix, &t) < 0 ||
	    step(rn_pre_normalize_page_references, &t) < 0 ||
	    step(rn_pre_normalize_section_references, &t) < 0 ||
	    step(rn_pre_normalize_administrative_law_references, &t) < 0 ||
	    step(rn_pre_normalize_complex_legal_references, &t) < 0 ||
	    step(rn_pre_normalize_clause_references, &t) < 0 ||
	    step(rn_pre_normalize_numbered_references, &t) < 0 ||
	    step(rn_pre_normalize_numeric_abbreviations, &t) < 0 ||
	    step(rn_pre_space_abbreviation_dot_before_digit, &t) < 0 ||
	    step(rn_pre_normalize_number_signs, &t) < 0 ||
	    step(rn_pre_normalize_vehicle_registration_plates, &t) < 0 ||
	    step(rn_pre_normalize_identifier_letter_suffixes, &t) < 0 ||
	    step(rn_pre_normalize_phone_numbers, &t) < 0 ||
	    step(rn_pre_normalize_unary_plus_numbers, &t) < 0 ||
	    step(rn_pre_protect_negative_numbers, &t) < 0 ||
	    step(rn_pre_normalize_explicit_dashes, &t) < 0 ||
	    step(rn_pre_normalize_age_ranges, &t) < 0 ||
	    step(rn_pre_normalize_isbn_identifiers, &t) < 0 ||
	    step(rn_pre_convert_bracketed_numbers, &t) < 0 ||
	    step(rn_pre_convert_line_numbering, &t) < 0 ||
	    step(rn_pre_convert_inline_list_markers, &t) < 0 ||
	    rn_resub_tmpl(NZ(PARTICLE_PATTERN), &t, RN_U("-")) < 0 ||
	    step(rn_pre_protect_binary_math_operators, &t) < 0 ||
	    step(rn_pre_apply_cleanup_replacements, &t) < 0 ||
	    step(rn_pre_normalize_unicode_fractions, &t) < 0 || superscripts(&t) < 0 ||
	    rn_resub_tmpl(NZ(CURRENCY_AMOUNT_PATTERN), &t, RN_U("\2\1")) < 0 ||
	    step(rn_pre_expand_arc_minutes, &t) < 0 ||
	    rn_resub_tmpl(NZ(DEGREE_SPACING_PATTERN), &t, RN_U("\1 \xB0")) < 0 ||
	    step(rn_pre_clean_numbers, &t) < 0 || step(rn_pre_fix_glued_numbers, &t) < 0 ||
	    step(rn_pre_restore_binary_math_operators, &t) < 0 ||
	    step(rn_pre_restore_letter_hyphens, &t) < 0)
		goto done;
	r = rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	return r;
}

int rn_run_caps(const rn_str *in, rn_str *out)
{
	rn_str t = {0};
	int r = -1;
	if (rn_str_put(&t, in->s, in->n) < 0 || step(rn_pre_normalize_caps_lines, &t) < 0 ||
	    step(rn_pre_normalize_first_word_caps, &t) < 0)
		goto done;
	r = rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	return r;
}

/* remove_links_ignore_interval of NormalizeOptions */
#define IGNORE_MIN 1000
#define IGNORE_MAX 2200

static int closer_of(uint32_t o)
{
	switch (o) {
	case '[': return ']';
	case '<': return '>';
	case '(': return ')';
	case '{': return '}';
	case 0xAB: return 0xBB;
	}
	return -1;
}

static int footnote_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t cn, a, b;
	const uint32_t *c = rn_group(m, 2, &cn);
	int keep = closer_of(m->s[m->start[1]]) != (int)m->s[m->start[3]];
	if (!keep && rn_classify_bracketed(c, cn) == 'r') {
		rn_py_strip(c, cn, &a, &b);
		if (rn_py_str_isdigit(c + a, b - a)) {
			/* int(stripped): a non-decimal digit (²) would raise in Python; QUIRKS */
			long v = 0;
			int ok = 1;
			for (size_t i = a; i < b && ok; i++) {
				int d = rn_py_decimal(c[i]);
				if (d < 0)
					ok = 0;
				else if (v <= IGNORE_MAX)
					v = v * 10 + d;
			}
			if (ok && v >= IGNORE_MIN && v <= IGNORE_MAX)
				keep = 1;
		}
		if (!keep)
			return 0;
	} else {
		keep = 1;
	}
	return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
}

int rn_remove_numeric_footnotes(const rn_str *in, rn_str *out)
{
	rn_str t = {0}, u = {0};
	int r = -1;
	if (rn_sub(PU(BRACKETED_NUMERIC_PATTERN), in->s ? in->s : RN_U(""), in->n, footnote_repl,
	           NULL, 0, &t) < 0)
		goto done;
	if (!t.s && rn_str_put(&t, NULL, 0) < 0)
		goto done;
	if (rn_resub_tmpl(PU(SPACE_BEFORE_PUNCT_PATTERN), &t, RN_U("\1")) < 0 ||
	    rn_resub_tmpl(rn_pat(RN_MATH_FORMULAS_MULTI_SPACE_RE), &t, RN_U(" ")) < 0 ||
	    rn_resub_tmpl(PU(SPACE_BEFORE_CLOSE_BRACKET_PATTERN), &t, RN_U("\1")) < 0 ||
	    rn_resub_tmpl(PU(SPACE_AFTER_OPEN_BRACKET_PATTERN), &t, RN_U("\1")) < 0 ||
	    rn_pre_normalize_linebreaks(&t, &u) < 0)
		goto done;
	size_t a, b;
	rn_py_strip(u.s, u.n, &a, &b);
	r = rn_str_put(out, u.s + a, b - a);
done:
	rn_str_free(&t);
	rn_str_free(&u);
	return r;
}
