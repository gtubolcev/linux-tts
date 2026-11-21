/* caps.py: normalize_first_word_caps, normalize_caps_lines */
#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-tables.h"
#include "rn-util.h"

static int known_abbr(const uint32_t *s, size_t n)
{
	return rn_table_get32(RN_T_KNOWN_ABBREVIATIONS, s, n) != NULL;
}

/* parse_word(word.lower()): parses non-empty and all with Abbr */
static int dictionary_abbreviation(const uint32_t *w, size_t n)
{
	cmorphy3_arena *a = cmorphy3_arena_new();
	cmorphy3_form *f;
	if (!a)
		return -1;
	int k = rn_parse(a, w, n, &f), r = k > 0;
	for (int i = 0; i < k && r; i++)
		r = rn_tag_has(f[i].tag, "Abbr");
	cmorphy3_arena_free(a);
	return k < 0 ? -1 : r;
}

static int first_word_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const uint32_t *s = m->s;
	size_t e = m->end[0], le = e;
	while (le < m->n && s[le] != '\n')
		le++;
	size_t wn;
	const uint32_t *w = rn_group(m, 2, &wn);
	int ok = !known_abbr(w, wn) && wn > 3;
	if (ok) {
		int d = dictionary_abbreviation(w, wn);
		if (d < 0)
			return -1;
		ok = !d;
	}
	if (ok) {
		ok = 0;
		for (size_t i = e; i < le && !ok; i++)
			ok = rn_py_isalpha(s[i]) || rn_py_isdigit(s[i]);
	}
	if (!ok)
		return rn_str_put(out, s + m->start[0], m->end[0] - m->start[0]);
	size_t pn;
	const uint32_t *p = rn_group(m, 1, &pn);
	return rn_str_put(out, p, pn) < 0 ? -1 : rn_py_capitalize(w, wn, out);
}

int rn_pre_normalize_first_word_caps(const rn_str *in, rn_str *out)
{
	return rn_sub(rn_pat(RN_CAPS_FIRST_WORD_CAPS_PATTERN), in->s ? in->s : RN_U(""), in->n,
	              first_word_repl, NULL, 0, out) < 0
	               ? -1
	               : 0;
}

/* _letters_only into buf (cap ≥ n) */
static size_t letters_only(const uint32_t *t, size_t n, uint32_t *buf)
{
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = t[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0x410 && c <= 0x44F) ||
		    c == 0x401 || c == 0x451)
			buf[k++] = c;
	}
	return k;
}

/* str.isupper() of a _letters_only result */
static int letters_upper(const uint32_t *l, size_t n)
{
	return rn_py_str_isupper(l, n);
}

static int is_connector(const uint32_t *l, size_t n)
{
	static const uint32_t C[] = {0x410, 0x412, 0x418, 0x41A, 0x41E, 0x421, 0x423};   /* АВИКОСУ */
	if (n != 1)
		return 0;
	for (size_t i = 0; i < sizeof C / sizeof *C; i++)
		if (l[0] == C[i])
			return 1;
	return 0;
}

typedef struct {
	size_t a, b;   /* token span in the line */
	int space;
} tok;

/* re.split(r"(\s+)", s): tokens alternating text/space, as Python (empty text tokens at
 * the ends included) */
static tok *split_ws(const uint32_t *s, size_t n, size_t *nt)
{
	size_t cap = 8, k = 0, i = 0;
	tok *t = malloc(cap * sizeof *t);
	if (!t)
		return NULL;
	for (;;) {
		size_t a = i;
		while (i < n && !rn_py_isspace(s[i]))
			i++;
		if (k + 2 > cap) {
			tok *x = realloc(t, (cap *= 2) * sizeof *t);
			if (!x) {
				free(t);
				return NULL;
			}
			t = x;
		}
		t[k++] = (tok){a, i, 0};
		if (i >= n)
			break;
		a = i;
		while (i < n && rn_py_isspace(s[i]))
			i++;
		t[k++] = (tok){a, i, 1};
	}
	*nt = k;
	return t;
}

/* token.isspace(): non-empty space token */
static int tspace(const tok *t)
{
	return t->space && t->b > t->a;
}

static int caps_token(const uint32_t *s, const tok *t, uint32_t *buf)
{
	if (!rn_py_str_isupper(s + t->a, t->b - t->a))
		return 0;
	size_t ln = letters_only(s + t->a, t->b - t->a, buf);
	return ln >= 2 && letters_upper(buf, ln) && !known_abbr(buf, ln);
}

/* _lowercase_preserve_abbrevs, appended to out */
static int lower_preserve(const uint32_t *s, size_t n, uint32_t *buf, rn_str *out)
{
	size_t ln = letters_only(s, n, buf);
	if (letters_upper(buf, ln) && known_abbr(buf, ln))
		return rn_str_put(out, s, n);
	return rn_py_lower(s, n, out);
}

static int inline_caps(const uint32_t *s, size_t n, rn_str *out)
{
	size_t nt;
	tok *t = split_ws(s, n, &nt);
	uint32_t *buf = malloc((n + 1) * sizeof *buf);
	unsigned char *run = calloc(nt + 1, 1);   /* 1 first word of a run, 2 other word */
	int r = 0;
	if (!t || !buf || !run) {
		r = -1;
		goto done;
	}
	for (size_t i = 0; i < nt;) {
		if (tspace(&t[i])) {
			i++;
			continue;
		}
		if (!caps_token(s, &t[i], buf)) {
			i++;
			continue;
		}
		size_t j = i + 1, count = 1;
		while (j < nt) {
			if (tspace(&t[j])) {
				j++;
				continue;
			}
			if (caps_token(s, &t[j], buf)) {
				count++;
				j++;
			} else {
				size_t ln = letters_only(s + t[j].a, t[j].b - t[j].a, buf);
				if (!is_connector(buf, ln))
					break;
				j++;
			}
		}
		if (count >= 2) {
			int first = 1;
			for (size_t k = i; k < j; k++)
				if (!tspace(&t[k])) {
					run[k] = first ? 1 : 2;
					first = 0;
				}
		}
		i = j;
	}
	for (size_t k = 0; k < nt && !r; k++) {
		const uint32_t *p = s + t[k].a;
		size_t pn = t[k].b - t[k].a;
		if (!run[k]) {
			r = rn_str_put(out, p, pn);
		} else if (run[k] == 2) {
			r = lower_preserve(p, pn, buf, out);
		} else {
			rn_str tmp = {0};
			r = lower_preserve(p, pn, buf, &tmp) < 0 ? -1 : rn_py_capitalize(tmp.s, tmp.n, out);
			rn_str_free(&tmp);
		}
	}
done:
	free(t);
	free(buf);
	free(run);
	return r;
}

/* first_parse_required(word).tag.POS is not None */
static int real_short_word(const uint32_t *l, size_t n)
{
	cmorphy3_arena *a = cmorphy3_arena_new();
	cmorphy3_form *f;
	if (!a)
		return -1;
	int k = rn_parse(a, l, n, &f);
	int r = k < 0 ? -1 : k > 0 && cmorphy3_tag_get(rn_morph, f[0].tag, CMORPHY3_POS) >= 0;
	cmorphy3_arena_free(a);
	return r;
}

static int is_vowel(uint32_t c)
{
	static const uint32_t V[] = {0x410, 0x415, 0x401, 0x418, 0x41E,
	                             0x423, 0x42B, 0x42D, 0x42E, 0x42F};   /* АЕЁИОУЫЭЮЯ */
	for (size_t i = 0; i < sizeof V / sizeof *V; i++)
		if (c == V[i])
			return 1;
	return 0;
}

/* stripped.lower() with _restore_known_abbreviations, appended to out */
static int lower_restore(const uint32_t *s, size_t n, rn_str *out)
{
	size_t nt;
	tok *t = split_ws(s, n, &nt);
	uint32_t *buf = malloc((n + 1) * sizeof *buf);
	int r = 0;
	if (!t || !buf) {
		r = -1;
		goto done;
	}
	for (size_t k = 0; k < nt && !r; k++) {
		const uint32_t *p = s + t[k].a;
		size_t pn = t[k].b - t[k].a, ln = letters_only(p, pn, buf);
		int keep = 0;
		if (ln && letters_upper(buf, ln)) {
			int dot = 0;
			for (size_t i = 0; i < pn; i++)
				dot |= p[i] == '.';
			if (!(is_connector(buf, ln) && !dot)) {
				size_t vowels = 0;
				for (size_t i = 0; i < ln; i++)
					vowels += is_vowel(buf[i]);
				int real = 0;
				if (ln >= 2 && ln <= 3) {
					rn_str low = {0};
					real = rn_py_lower(buf, ln, &low) < 0 ? -1 : real_short_word(low.s, low.n);
					rn_str_free(&low);
					if (real < 0) {
						r = -1;
						break;
					}
				}
				keep = known_abbr(buf, ln) || (ln <= 3 && !real) || (ln == 4 && vowels <= 1) ||
				       !vowels;
			}
		}
		r = keep ? rn_str_put(out, p, pn) : rn_py_lower(p, pn, out);
	}
done:
	free(t);
	free(buf);
	return r;
}

static int caps_line(const uint32_t *s, size_t n, rn_str *out)
{
	size_t letters = 0, upper = 0;
	for (size_t i = 0; i < n; i++)
		if (rn_py_isalpha(s[i])) {
			letters++;
			upper += rn_py_isupper(s[i]);
		}
	if (letters >= 10 && (double)upper / (double)letters >= 0.8) {
		size_t a, b;
		rn_py_strip(s, n, &a, &b);
		rn_str low = {0};
		int r = lower_restore(s + a, b - a, &low);
		if (r == 0)
			r = rn_str_put(out, s, a);
		size_t i = 0;
		while (r == 0 && i < low.n && !rn_py_isalpha(low.s[i]))
			i++;
		if (r == 0 && i < low.n)
			r = rn_str_put(out, low.s, i) < 0 || rn_py_upper(low.s + i, 1, out) < 0 ||
			            rn_str_put(out, low.s + i + 1, low.n - i - 1) < 0
			        ? -1
			        : 0;
		else if (r == 0)
			r = rn_str_put(out, low.s, low.n);
		if (r == 0)
			r = rn_str_put(out, s + b, n - b);
		rn_str_free(&low);
		return r;
	}
	if (upper < 4)
		return rn_str_put(out, s, n);
	return inline_caps(s, n, out);
}

int rn_pre_normalize_caps_lines(const rn_str *in, rn_str *out)
{
	const uint32_t *s = in->s ? in->s : RN_U("");
	size_t n = in->n, a = 0;
	for (;;) {
		size_t b = a;
		while (b < n && s[b] != '\n')
			b++;
		if (caps_line(s + a, b - a, out) < 0)
			return -1;
		if (b >= n)
			return 0;
		if (rn_str_putc(out, '\n') < 0)
			return -1;
		a = b + 1;
	}
}
