/* numbering.py: line numbering, inline list markers, bracketed references; and
 * restore_non_normative_day_suffix of dates_time.py */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-num.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define NB(id) rn_pat(RN_NUMBERING_##id)
#define PU(id) rn_pat(RN_PREPROCESS_UTILS_##id)

/* str(int(s)) of decimal digits (malloc'd); NULL where int() raises ValueError: more
 * than the default int_max_str_digits of 4300 */
static char *int_str(const uint32_t *s, size_t n)
{
	if (!n || n > 4300)
		return NULL;
	char *b = malloc(n + 1);
	if (b && rn_int_str(s, n, b, n + 1) < 0) {
		free(b);
		b = NULL;
	}
	return b;
}

static int all_decimal(const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!rn_py_isdecimal(s[i]))
			return 0;
	return n > 0;
}

/* roman_to_word(s): num_to_word(fromRoman(s.upper())), s itself if invalid */
static int put_roman_word(rn_str *out, const uint32_t *s, size_t n)
{
	long v = rn_roman_value(s, n);
	if (v < 0)
		return rn_str_put(out, s, n);
	char num[16];
	uint32_t w[16];
	int k = snprintf(num, sizeof num, "%ld", v);
	for (int i = 0; i < k; i++)
		w[i] = (uint32_t)num[i];
	return rn_put_cardinal(out, w, (size_t)k);
}

static int is_roman_word(const uint32_t *s, size_t n)
{
	rn_match m;
	return rn_match_at(NB(L183), s, n, 0, &m) == 1;
}

/* convert_numeric_sequence */
static int numeric_sequence(rn_str *out, const uint32_t *s, size_t n)
{
	while (n && s[n - 1] == '.')
		n--;
	if (is_roman_word(s, n))
		return put_roman_word(out, s, n);
	int first = 1;
	for (size_t i = 0; i < n;) {
		size_t j = i;
		while (j < n && s[j] != '.')
			j++;
		if (j > i) {
			if (!first && rn_puts(out, RN_U(" точка ")) < 0)
				return -1;
			first = 0;
			/* int(part) of [\d]+: num_to_word; ValueError (too long) keeps the part */
			char *num = int_str(s + i, j - i);
			int r;
			if (num) {
				uint32_t *w = malloc((strlen(num) + 1) * sizeof *w);
				if (!w) {
					free(num);
					return -1;
				}
				size_t k = 0;
				for (; num[k]; k++)
					w[k] = (unsigned char)num[k];
				r = rn_put_cardinal(out, w, k);
				free(w);
				free(num);
			} else {
				r = rn_str_put(out, s + i, j - i);
			}
			if (r < 0)
				return -1;
		}
		i = j + 1;
	}
	return 0;
}

static int put_cap(rn_str *out, const rn_str *w)
{
	return rn_put_capitalized(out, w->s, w->n);
}

int rn_pre_convert_line_numbering(const rn_str *in, rn_str *out)
{
	const rn_re *re = NB(LINE_NUMBERING_PATTERN);
	size_t cap = 16, nm = 0;
	rn_match *ms = malloc(cap * sizeof *ms);
	long *vals = malloc(cap * sizeof *vals);
	int r = -1, chronology = 0, any_bare = 0;
	if (!ms || !vals)
		goto done;
	rn_iter it = rn_iter_init(re, in->s, in->n, 0);
	int k;
	while ((k = rn_iter_next(&it, &ms[nm])) == 1) {
		size_t gn;
		const uint32_t *g = rn_group(&ms[nm], 2, &gn);
		rn_match bm;
		vals[nm] = -1;
		k = rn_match_at(NB(BARE_LINE_MARKER_PATTERN), g, gn, 0, &bm);
		if (k < 0)
			goto done;
		if (k == 1) {
			/* a bare value: only its range matters, so a long one is just "big" */
			size_t vn = gn - 1, a = 0;
			while (a + 1 < vn && rn_py_decimal(g[a]) == 0)
				a++;
			long v = 0;
			if (vn - a > 6)
				v = 1000000;
			else
				for (size_t i = a; i < vn; i++)
					v = v * 10 + rn_py_decimal(g[i]);
			vals[nm] = v;
		}
		if (++nm == cap) {
			cap *= 2;
			rn_match *m2 = realloc(ms, cap * sizeof *ms);
			if (m2)
				ms = m2;
			long *v2 = realloc(vals, cap * sizeof *vals);
			if (v2)
				vals = v2;
			if (!m2 || !v2)
				goto done;
		}
	}
	if (k < 0)
		goto done;
	if (!nm) {
		r = rn_str_put(out, in->s, in->n);
		goto done;
	}
	chronology = 1;
	for (size_t i = 0; i < nm; i++)
		if (vals[i] >= 0) {
			any_bare = 1;
			if (vals[i] < 1000 || vals[i] > 2100)
				chronology = 0;
		}
	chronology &= any_bare;
	size_t pos = 0;
	rn_str w = {0};
	for (size_t i = 0; i < nm; i++) {
		const rn_match *m = &ms[i];
		size_t gn;
		const uint32_t *g = rn_group(m, 2, &gn);
		w.n = 0;
		int e;
		if (chronology && vals[i] >= 0) {
			char num[24];
			snprintf(num, sizeof num, "%ld", vals[i]);
			e = rn_put_ordinal(&w, num, N2W_NOMN, N2W_MASC, 0, 1);
		} else {
			e = numeric_sequence(&w, g, gn);
		}
		if (e < 0 || rn_str_put(out, in->s + pos, m->start[0] - pos) < 0) {
			rn_str_free(&w);
			goto done;
		}
		const uint32_t *g1 = rn_group(m, 1, &gn);
		if (rn_str_put(out, g1, gn) < 0 || put_cap(out, &w) < 0 || rn_str_putc(out, '.') < 0) {
			rn_str_free(&w);
			goto done;
		}
		const uint32_t *g3 = rn_group(m, 3, &gn);
		if (rn_str_put(out, g3, gn) < 0) {
			rn_str_free(&w);
			goto done;
		}
		pos = m->end[0];
	}
	rn_str_free(&w);
	r = rn_str_put(out, in->s + pos, in->n - pos);
done:
	free(ms);
	free(vals);
	return r;
}

static size_t count_char(const uint32_t *s, size_t a, size_t b, uint32_t c)
{
	size_t k = 0;
	for (size_t i = a; i < b; i++)
		k += s[i] == c;
	return k;
}

int rn_pre_convert_inline_list_markers(const rn_str *in, rn_str *out)
{
	const uint32_t *s = in->s;
	if (!count_char(s, 0, in->n, ')'))
		return rn_str_put(out, s, in->n);
	size_t cap = 16, nsel = 0;
	rn_match *sel = malloc(cap * sizeof *sel);   /* matches in the runs, in order */
	size_t run_start = 0;                        /* index in sel of the current run */
	int r = -1;
	if (!sel)
		return -1;
	rn_iter it = rn_iter_init(NB(INLINE_LIST_MARKER_PATTERN), s, in->n, 0);
	rn_match m;
	long balance = 0, expected = 1;
	size_t scanned = 0;
	int k;
	/* the current run is sel[run_start .. nsel); a finished run of < 2 is dropped */
	while ((k = rn_iter_next(&it, &m)) == 1) {
		balance += (long)count_char(s, scanned, m.start[0], '(');
		balance -= (long)count_char(s, scanned, m.start[0], ')');
		scanned = m.start[0];
		if (balance > 0)
			continue;
		size_t gn;
		const uint32_t *g = rn_group(&m, 1, &gn);
		long value = 0;
		for (size_t i = 0; i < gn; i++)
			value = value * 10 + rn_py_decimal(g[i]);
		uint32_t lead = s[m.start[0] - 2];
		rn_match lm;
		if (lead == U'и' && m.start[0] >= 3 &&
		    rn_match_at(NB(LETTER_BEFORE_LEAD), s, in->n, m.start[0] - 3, &lm) == 1)
			lead = '?';
		int cur = nsel > run_start;
		if (cur && value == expected && (lead == ',' || lead == ';' || lead == U'и')) {
			expected++;
		} else {
			if (nsel - run_start < 2)
				nsel = run_start;   /* drop a single */
			run_start = nsel;
			expected = 1;
			if (!(value == 1 && (lead == ':' || lead == ';')))
				continue;
			expected = 2;
		}
		if (nsel + 1 >= cap) {
			cap *= 2;
			rn_match *p = realloc(sel, cap * sizeof *sel);
			if (!p)
				goto done;
			sel = p;
		}
		sel[nsel++] = m;
	}
	if (k < 0)
		goto done;
	if (nsel - run_start < 2)
		nsel = run_start;
	size_t pos = 0;
	char num[8];
	for (size_t i = 0; i < nsel; i++) {
		size_t gn;
		const uint32_t *g = rn_group(&sel[i], 1, &gn);
		long value = 0;
		for (size_t j = 0; j < gn; j++)
			value = value * 10 + rn_py_decimal(g[j]);
		snprintf(num, sizeof num, "%ld", value);
		if (rn_str_put(out, s + pos, sel[i].start[0] - pos) < 0 ||
		    rn_put_ordinal(out, num, N2W_NOMN, N2W_NEUT, 0, 0) < 0 ||
		    rn_str_putc(out, ')') < 0)
			goto done;
		pos = sel[i].end[0];
	}
	r = rn_str_put(out, s + pos, in->n - pos);
done:
	free(sel);
	return r;
}

/* classify_bracketed_numeric_content: 'n'umeric, 'r'eference, 'o'ther */
char rn_classify_bracketed(const uint32_t *c, size_t cn)
{
	size_t a, b;
	rn_py_strip(c, cn, &a, &b);
	const uint32_t *s = c + a;
	size_t n = b - a;
	rn_match m;
	if (!n)
		return 'o';
	if (rn_fullmatch(PU(SIGNED_COMMA_NUMBER_PATTERN), s, n, 0, &m) == 1 ||
	    rn_fullmatch(PU(SIGNED_INTEGER_WITH_UNIT_PATTERN), s, n, 0, &m) == 1)
		return 'n';
	if (rn_py_str_isdigit(s, n) || rn_fullmatch(PU(INTEGER_RANGE_PATTERN), s, n, 0, &m) == 1)
		return 'r';
	if (rn_fullmatch(PU(INTEGER_LIST_PATTERN), s, n, 0, &m) == 1) {
		int ok = count_char(s, 0, n, ',') == 1;
		for (size_t i = 0; ok && i + 1 < n; i++)
			if ((s[i] == ',' && s[i + 1] == ' ') || (s[i] == ' ' && s[i + 1] == ','))
				ok = 0;
		return ok ? 'n' : 'r';
	}
	if (count_char(s, 0, n, '.')) {
		size_t parts = 0, l0 = 0, l1 = 0;
		for (size_t i = 0; i <= n;) {
			size_t j = i;
			while (j < n && s[j] != '.')
				j++;
			if (!rn_py_str_isdigit(s + i, j - i))
				return 'o';
			if (parts == 0)
				l0 = j - i;
			else if (parts == 1)
				l1 = j - i;
			parts++;
			i = j + 1;
		}
		if (parts >= 3)
			return 'r';
		return l0 <= 2 && l1 <= 3 ? 'r' : 'n';
	}
	return 'o';
}

static int bracket_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	uint32_t op = *rn_group(m, 1, &n), cl = *rn_group(m, 3, &n);
	const uint32_t *seq = rn_group(m, 2, &n);
	int pair = (op == '(' && cl == ')') || (op == '[' && cl == ']') || (op == '{' && cl == '}');
	if (pair) {
		size_t a, b;
		rn_py_strip(seq, n, &a, &b);
		const uint32_t *s = seq + a;
		size_t sn = b - a;
		char kind = is_roman_word(s, sn) ? 'r' : rn_classify_bracketed(s, sn);
		if (kind == 'r') {
			/* remove_links=True, remove_links_ignore_interval=(1000, 2200) of tts() */
			int keep = 0;
			if (rn_py_str_isdigit(s, sn) && all_decimal(s, sn)) {
				size_t i = 0;
				while (i + 1 < sn && rn_py_decimal(s[i]) == 0)
					i++;
				if (sn - i <= 4) {
					long v = 0;
					for (; i < sn; i++)
						v = v * 10 + rn_py_decimal(s[i]);
					keep = v >= 1000 && v <= 2200;
				}
			}
			if (!keep)
				return 0;
		}
	}
	return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
}

int rn_pre_convert_bracketed_numbers(const rn_str *in, rn_str *out)
{
	return rn_sub(NB(BRACKETED_NUMBER_PATTERN), in->s, in->n, bracket_repl, NULL, 0, out) < 0
	               ? -1
	               : 0;
}

static int day_repl(const rn_match *m, rn_str *out, void *ctx)
{
	int g = *(int *)ctx;
	size_t n;
	const uint32_t *s = rn_group(m, g, &n);
	return rn_str_put(out, s, n);
}

int rn_pre_restore_non_normative_day_suffix(const rn_str *in, rn_str *out)
{
	const rn_re *re = rn_pat(RN_DATES_TIME_NON_NORMATIVE_DAY_SUFFIX_PATTERN);
	int g = rn_re_group(re, "day");
	return rn_sub(re, in->s, in->n, day_repl, &g, 0, out) < 0 ? -1 : 0;
}
