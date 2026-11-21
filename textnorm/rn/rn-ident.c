/* identifiers.py: letter suffixes, vehicle plates, phone numbers, labelled identifiers,
 * superscript footnotes, glued numbers */
#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-num.h"
#include "rn-pat.h"
#include "rn-ph.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-tables.h"
#include "rn-units.h"
#include "rn-util.h"

#define ID(id) rn_pat(RN_IDENTIFIERS_##id)

/* letter_vowel_mode of NormalizeOptions.tts() */
int rn_letter_double = 1;

static int put_g(const rn_match *m, const rn_re *re, const char *name, rn_str *out)
{
	size_t n;
	const uint32_t *s = rn_group(m, rn_re_group(re, name), &n);
	return rn_str_put(out, s, n);
}

static int put_whole(const rn_match *m, rn_str *out)
{
	return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
}

static const uint32_t *grp(const rn_match *m, const rn_re *re, const char *name, size_t *n)
{
	return rn_group(m, rn_re_group(re, name), n);
}

/* inflect_numeral_string(s, "nomn"): num2words(int(s), case="nominative"); s if it raises */
static int put_nomn(rn_str *out, const uint32_t *s, size_t n)
{
	char *num = n && n <= 4300 ? malloc(n + 1) : NULL;
	int r = 1;
	if (num && rn_int_str(s, n, num, n + 1) > 0)
		r = rn_put_cardinal_a(out, num);
	free(num);
	return r == 1 ? rn_str_put(out, s, n) : r;
}

/* ---- letter suffixes, plates, phones ---- */

static int letter_suffix_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = ID(IDENTIFIER_LETTER_SUFFIX_PATTERN);
	size_t n;
	uint32_t letter = *grp(m, re, "letter", &n);
	const char *name = rn_letter_name(letter, letter < 0x80 && rn_letter_double);
	if (!name)
		return put_whole(m, out);
	return put_g(m, re, "prefix", out) < 0 || put_g(m, re, "number", out) < 0 ||
	                       rn_str_putc(out, ' ') < 0 || rn_str_utf8(out, name) < 0
	               ? -1
	               : 0;
}

int rn_pre_normalize_identifier_letter_suffixes(const rn_str *in, rn_str *out)
{
	return rn_sub(ID(IDENTIFIER_LETTER_SUFFIX_PATTERN), in->s, in->n, letter_suffix_repl, NULL, 0,
	              out) < 0
	               ? -1
	               : 0;
}

static int plate_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = ID(VEHICLE_PLATE_PATTERN);
	size_t n;
	uint32_t letter = *grp(m, re, "leading", &n);
	const char *name = rn_letter_name(letter, letter < 0x80 && rn_letter_double);
	if (!name)
		return put_whole(m, out);
	return put_g(m, re, "prefix", out) < 0 || rn_str_utf8(out, name) < 0 ||
	                       rn_str_putc(out, ' ') < 0 || put_g(m, re, "number", out) < 0 ||
	                       rn_str_putc(out, ' ') < 0 || put_g(m, re, "series", out) < 0 ||
	                       rn_str_putc(out, ' ') < 0 || put_g(m, re, "region", out) < 0
	               ? -1
	               : 0;
}

int rn_pre_normalize_vehicle_registration_plates(const rn_str *in, rn_str *out)
{
	return rn_sub(ID(VEHICLE_PLATE_PATTERN), in->s, in->n, plate_repl, NULL, 0, out) < 0 ? -1 : 0;
}

static int phone_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = ID(PHONE_NUMBER_PATTERN);
	size_t pos = m->start[0], a = pos > 48 ? pos - 48 : 0;
	rn_match cm;
	int f = rn_search(ID(PHONE_CONTEXT_PATTERN), m->s + a, pos - a, 0, &cm);
	if (f < 0)
		return -1;
	if (!f)
		return put_whole(m, out);
	static const char *const G[] = {"country", "area", "first", "second", "third"};
	for (int i = 0; i < 5; i++)
		if ((i && rn_puts(out, RN_U(", ")) < 0) || put_g(m, re, G[i], out) < 0)
			return -1;
	return 0;
}

int rn_pre_normalize_phone_numbers(const rn_str *in, rn_str *out)
{
	return rn_sub(ID(PHONE_NUMBER_PATTERN), in->s, in->n, phone_repl, NULL, 0, out) < 0 ? -1 : 0;
}

/* ---- labelled identifiers ---- */

static int is_sep(uint32_t c)
{
	return c == '.' || c == '-' || c == ':';
}

/* _read_code_groups */
static int code_groups(rn_str *out, const uint32_t *s, size_t n)
{
	rn_str t = {0};
	int r = -1;
	size_t g = 0;   /* start of the digit group */
	int ing = 0;
	for (size_t i = 0; i <= n; i++) {
		if (i < n && rn_py_isdecimal(s[i])) {
			if (!ing)
				g = i;
			ing = 1;
			continue;
		}
		if (ing && put_nomn(&t, s + g, i - g) < 0)
			goto done;
		ing = 0;
		if (i == n)
			break;
		const uint32_t *rd = s[i] == '.' ? RN_U(" точка ") : s[i] == ':' ? RN_U(", ") : RN_U(" ");
		if (rn_puts(&t, rd) < 0)
			goto done;
	}
	size_t a, b;
	rn_py_strip(t.s ? t.s : RN_U(""), t.n, &a, &b);
	r = rn_str_put(out, t.s + a, b - a);
done:
	rn_str_free(&t);
	return r;
}

static int labelled_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	static const uint32_t *const DIGIT_LABELS[] = {RN_U("ИНН"), RN_U("ОГРНИП"), RN_U("ОГРН"),
	                                               RN_U("КПП"), RN_U("БИК"), RN_U("ОКПО")};
	const rn_re *re = ID(LABELLED_IDENTIFIER_PATTERN);
	size_t ln, cn, sn;
	const uint32_t *label = grp(m, re, "label", &ln);
	const uint32_t *code = grp(m, re, "code", &cn);
	const uint32_t *series = grp(m, re, "series", &sn);
	int digits = 0;
	for (size_t i = 0; i < sizeof DIGIT_LABELS / sizeof *DIGIT_LABELS; i++)
		if (rn_u32len(DIGIT_LABELS[i]) == ln && !memcmp(DIGIT_LABELS[i], label, ln * 4))
			digits = 1;
	rn_str reading = {0};
	int r = -1;
	if (digits) {
		int dec = 1;
		for (size_t i = 0; i < cn; i++)
			dec &= rn_py_isdecimal(code[i]);
		if (!dec || cn < 6) {
			r = put_whole(m, out);
			goto done;
		}
		for (size_t i = 0; i < cn; i++)
			if ((i && rn_str_putc(&reading, ' ') < 0) || put_nomn(&reading, code + i, 1) < 0)
				goto done;
	} else {
		int sep = 0;
		for (size_t i = 0; i < cn; i++)
			sep |= is_sep(code[i]);
		if (!sep) {
			r = put_whole(m, out);
			goto done;
		}
		if (code_groups(&reading, code, cn) < 0)
			goto done;
	}
	r = rn_str_put(out, label, ln) < 0 || rn_str_put(out, series, sn) < 0 ||
	                    rn_str_putc(out, ' ') < 0 || rn_str_put(out, reading.s, reading.n) < 0
	            ? -1
	            : 0;
done:
	rn_str_free(&reading);
	return r;
}

static int unit_token(const uint32_t *s, size_t n)
{
	return rn_unit_resolve(s, n) != NULL;
}

static int compact_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = ID(COMPACT_ALPHANUMERIC_CODE_PATTERN);
	size_t n;
	uint32_t letter = *grp(m, re, "letter", &n);
	if ((letter | 0x20) == 'x' || (letter | 0x20) == 'e')
		return put_whole(m, out);
	if (unit_token(m->s + m->start[0], m->end[0] - m->start[0]))
		return put_whole(m, out);
	const char *name = rn_letter_name(letter, rn_letter_double);
	if (!name)
		return put_whole(m, out);
	size_t hn, tn;
	const uint32_t *head = grp(m, re, "head", &hn), *tail = grp(m, re, "tail", &tn);
	return put_nomn(out, head, hn) < 0 || rn_str_putc(out, ' ') < 0 || rn_str_utf8(out, name) < 0 ||
	                       rn_str_putc(out, ' ') < 0 || put_nomn(out, tail, tn) < 0
	               ? -1
	               : 0;
}

int rn_pre_normalize_labelled_identifiers(const rn_str *in, rn_str *out)
{
	rn_str t = {0};
	int r = rn_sub(ID(LABELLED_IDENTIFIER_PATTERN), in->s, in->n, labelled_repl, NULL, 0, &t) < 0 ||
	                        rn_sub(ID(COMPACT_ALPHANUMERIC_CODE_PATTERN), t.s ? t.s : RN_U(""), t.n,
	                               compact_repl, NULL, 0, out) < 0
	                ? -1
	                : 0;
	rn_str_free(&t);
	return r;
}

/* ---- superscript footnotes ---- */

static int superscript_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *host = grp(m, ID(SUPERSCRIPT_FOOTNOTE_PATTERN), "host", &n);
	if (unit_token(host, n))
		return put_whole(m, out);
	return rn_str_put(out, host, n);
}

int rn_pre_drop_superscript_footnote_markers(const rn_str *in, rn_str *out)
{
	return rn_sub(ID(SUPERSCRIPT_FOOTNOTE_PATTERN), in->s, in->n, superscript_repl, NULL, 0, out) < 0
	               ? -1
	               : 0;
}

/* ---- glued numbers ---- */

static int in_str(uint32_t c, const uint32_t *set)
{
	for (; *set; set++)
		if (*set == c)
			return 1;
	return 0;
}

/* _standalone_letter_precedes */
static int standalone_letter_precedes(const uint32_t *s, size_t start)
{
	size_t p = start;
	while (p > 0 && rn_py_isspace(s[p - 1]))
		p--;
	if (p == start || p == 0)
		return 0;
	if (!rn_py_isalpha(s[p - 1]))
		return 0;
	if (p < 2)
		return 1;
	size_t before = p - 2;
	if (in_str(s[before], RN_U("-'’")))
		return before == 0 || !rn_py_isalnum(s[before - 1]);
	return !rn_py_isalnum(s[before]);
}

static int str_isascii(const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (s[i] >= 0x80)
			return 0;
	return 1;
}

/* classify_glued_numeric_rhs: 'k' thousand_suffix, 'h' hyphen, 's' space; -1 error */
static int classify_rhs(const uint32_t *w, size_t n)
{
	if (n == 1 && w[0] == 'k')
		return 'k';
	rn_str low = {0};
	int r = -1;
	if (rn_py_lower(w, n, &low) < 0)
		goto done;
	if (rn_table_get32(RN_T_NUMERIC_SUFFIXES, low.s, low.n) ||
	    (low.n == 1 && (low.s[0] == U'о' || low.s[0] == U'а'))) {
		r = 'h';
		goto done;
	}
	if (unit_token(w, n) || rn_table_get32(RN_T_GLUED_PREPOSITIONS, low.s, low.n)) {
		r = 's';
		goto done;
	}
	cmorphy3_arena *a = cmorphy3_arena_new();
	cmorphy3_form *f;
	if (!a)
		goto done;
	if (rn_parse(a, low.s, low.n, &f) < 1) {
		cmorphy3_arena_free(a);
		goto done;
	}
	uint16_t tag = f[0].tag;
	cmorphy3_arena_free(a);
	if (rn_tag_has(tag, "ADJF"))
		r = 'h';
	else if (rn_tag_has(tag, "NOUN"))
		r = 's';
	else if (rn_tag_has(tag, "UNKN") || str_isascii(w, n))
		r = 's';
	else
		r = 'h';
done:
	rn_str_free(&low);
	return r;
}

static int glued_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t nn, wn;
	const uint32_t *num = rn_group(m, 1, &nn), *w = rn_group(m, 2, &wn);
	size_t start = m->start[0];
	int prev_letter = start > 0 && rn_py_isalpha(m->s[start - 1]);
	int embedded = (prev_letter || standalone_letter_precedes(m->s, start)) && wn == 1;
	int kind = embedded ? 'h' : classify_rhs(w, wn);
	if (kind < 0 || rn_str_put(out, num, nn) < 0)
		return -1;
	if (kind == 'k')
		return rn_puts(out, RN_U(" тыс"));
	return rn_str_putc(out, kind == 's' ? ' ' : '-') < 0 ? -1 : rn_str_put(out, w, wn);
}

static int any_parse(const uint32_t *s, size_t n, int mode)
{
	/* mode 0: normal_form in LABEL_NOUN_LEMMAS; 1: NOUN and gent; 2: NOUN, gent, no Abbr */
	cmorphy3_arena *a = cmorphy3_arena_new();
	cmorphy3_form *f;
	int res = -1;
	if (!a)
		return -1;
	int k = rn_parse(a, s, n, &f);
	if (k >= 0) {
		res = 0;
		for (int i = 0; i < k && !res; i++) {
			if (mode == 0)
				res = rn_table_get(RN_T_LABEL_NOUN_LEMMAS, f[i].normal_form) != NULL;
			else
				res = rn_tag_has(f[i].tag, "NOUN") && rn_tag_case_is(f[i].tag, "gent") &&
				      (mode == 1 || !rn_tag_has(f[i].tag, "Abbr"));
		}
	}
	cmorphy3_arena_free(a);
	return res;
}

/* _glued_digits_are_footnote: 1, 0, -1 error */
static int glued_footnote(const uint32_t *letters, size_t ln, const uint32_t *num, size_t nn,
                          const uint32_t *s, size_t n, size_t end)
{
	rn_match m;
	if (nn > 2 || num[0] == '0')
		return 0;
	if (end < n && (rn_py_isalnum(s[end]) || in_str(s[end], RN_U("-–—%°"))))
		return 0;
	if (rn_search(ID(FOOTNOTE_HOST_WORD_PATTERN), letters, ln, 0, &m) != 1) {
		if (rn_search(ID(CAPITALIZED_FOOTNOTE_HOST_PATTERN), letters, ln, 0, &m) != 1)
			return 0;
		if (end < n && !in_str(s[end], RN_U(".,;:!?…")))
			return 0;
	}
	int k;
	if ((k = any_parse(letters, ln, 0)) != 0 || (k = any_parse(letters, ln, 1)) != 0)
		return k < 0 ? -1 : 0;
	/* _next_word_after */
	size_t p = end;
	while (p < n && rn_py_isspace(s[p]))
		p++;
	size_t ws = p;
	while (p < n && (rn_py_isalpha(s[p]) || in_str(s[p], RN_U("-'’"))))
		p++;
	if (p > ws) {
		size_t a = ws, b = p;
		while (a < b && in_str(s[a], RN_U("-'’")))
			a++;
		while (b > a && in_str(s[b - 1], RN_U("-'’")))
			b--;
		rn_str clean = {0};
		if (rn_py_lower(s + a, b - a, &clean) < 0)
			return -1;
		int res = 1;
		if (clean.n && unit_token(clean.s, clean.n))
			res = 0;
		else if (clean.n >= 3 && (k = any_parse(clean.s, clean.n, 2)) != 0)
			res = k < 0 ? -1 : 0;
		rn_str_free(&clean);
		return res;
	}
	return 1;
}

static int leading_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = ID(LEADING_LETTER_DIGIT_PATTERN);
	if (unit_token(m->s + m->start[0], m->end[0] - m->start[0]))
		return put_whole(m, out);
	size_t ln, nn;
	const uint32_t *letters = grp(m, re, "letters", &ln), *num = grp(m, re, "number", &nn);
	int f = glued_footnote(letters, ln, num, nn, m->s, m->n, m->end[0]);
	if (f < 0 || rn_str_put(out, letters, ln) < 0)
		return -1;
	return f ? 0 : rn_str_putc(out, ' ') < 0 ? -1 : rn_str_put(out, num, nn);
}

static int protect_repl(const rn_match *m, rn_str *out, void *ctx)
{
	long p = rn_ph_alloc(ctx, m->s + m->start[0], m->end[0] - m->start[0]);
	if (p < 0)
		return -1;
	return p ? rn_str_putc(out, (uint32_t)p) : put_whole(m, out);
}

int rn_pre_fix_glued_numbers(const rn_str *in, rn_str *out)
{
	rn_ph codes;
	rn_str t = {0}, u = {0};
	int r = -1;
	if (rn_ph_init(&codes, RN_PH_GLUED_CODES, in->s ? in->s : RN_U(""), in->n) < 0)
		return -1;
	if (rn_sub(ID(EMBEDDED_NUMERIC_SUFFIX_CODE_PATTERN), in->s ? in->s : RN_U(""), in->n,
	           protect_repl, &codes, 0, &t) < 0)
		goto done;
	if (rn_sub(ID(LEADING_LETTER_DIGIT_PATTERN), t.s, t.n, leading_repl, NULL, 0, &u) < 0)
		goto done;
	for (int it = 0; it < 5; it++) {
		t.n = 0;
		long k = rn_sub(ID(GLUED_NUMBER_WORD_PATTERN), u.s, u.n, glued_repl, NULL, 0, &t);
		if (k < 0)
			goto done;
		int same = rn_str_eq(&t, &u);
		rn_str x = t;
		t = u;
		u = x;
		if (same)
			break;
	}
	r = rn_ph_restore(&codes, u.s ? u.s : RN_U(""), u.n, out);
done:
	rn_str_free(&t);
	rn_str_free(&u);
	rn_ph_free(&codes);
	return r;
}
