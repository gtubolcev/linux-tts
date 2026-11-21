/* abbreviation_context.py and the abbreviation helpers of preprocess_utils.py */
#include <stdlib.h>

#include "rn-num.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define AC(id) rn_pat(RN_ABBREVIATION_CONTEXT_##id)
#define PU(id) rn_pat(RN_PREPROCESS_UTILS_##id)

/* text_scan.first_non_whitespace_index */
size_t rn_first_non_ws(const uint32_t *s, size_t n, size_t start, int *saw_newline)
{
	int nl = 0;
	while (start < n && rn_py_isspace(s[start])) {
		nl |= s[start] == '\n';
		start++;
	}
	if (saw_newline)
		*saw_newline = nl;
	return start;
}

static int in_str(uint32_t c, const uint32_t *set)
{
	for (; *set; set++)
		if (*set == c)
			return 1;
	return 0;
}

int rn_keep_terminal_abbr_dot(const uint32_t *s, size_t n, size_t end)
{
	int nl;
	size_t i = rn_first_non_ws(s, n, end, &nl);
	if (i >= n || nl)
		return 1;
	if (in_str(s[i], RN_U(".!?…")) || rn_py_isupper(s[i]))
		return 1;
	if (in_str(s[i], RN_U("\"»”)]}"))) {
		size_t j = rn_first_non_ws(s, n, i + 1, NULL);
		return j >= n || rn_py_isupper(s[j]);
	}
	return 0;
}

static int put_g(const rn_match *m, const rn_re *re, const char *name, rn_str *out)
{
	size_t n;
	const uint32_t *s = rn_group(m, rn_re_group(re, name), &n);
	return rn_str_put(out, s, n);
}

static int birth_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t i = rn_first_non_ws(m->s, m->n, m->end[0], NULL);
	int keep = i >= m->n || rn_py_isupper(m->s[i]) || rn_py_isdigit(m->s[i]);
	if (put_g(m, AC(BIRTH_YEAR_ABBREVIATION_PATTERN), "year", out) < 0 ||
	    rn_puts(out, RN_U(" г. рождения")) < 0)
		return -1;
	return keep ? rn_str_putc(out, '.') : 0;
}

static int mass_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = AC(MASS_GRAM_ABBREVIATION_PATTERN);
	size_t dn;
	rn_group(m, rn_re_group(re, "dot"), &dn);
	int keep = dn && rn_keep_terminal_abbr_dot(m->s, m->n, m->end[0]);
	if (put_g(m, re, "context", out) < 0 || put_g(m, re, "num", out) < 0 ||
	    rn_puts(out, RN_U(" грамм")) < 0)
		return -1;
	return keep ? rn_str_putc(out, '.') : 0;
}

static int legal_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = PU(LEGAL_ARTICLE_REFERENCE_PATTERN);
	size_t cn;
	const uint32_t *code = rn_group(m, rn_re_group(re, "code"), &cn);
	rn_str low = {0};
	if (rn_py_lower(code, cn, &low) < 0)
		return -1;
	int uk = low.n && low.s[0] == U'у';
	rn_str_free(&low);
	return rn_puts(out, RN_U("статья ")) < 0 || put_g(m, re, "number", out) < 0 ||
	                       rn_puts(out, uk ? RN_U(" уголовного кодекса российской федерации")
	                                       : RN_U(" арбитражного кодекса российской федерации")) < 0
	               ? -1
	               : 0;
}

static int era_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = PU(ERA_ABBREVIATION_PATTERN);
	size_t an, tn;
	const uint32_t *abbr = rn_group(m, rn_re_group(re, "abbr"), &an);
	const uint32_t *tail = rn_group(m, rn_re_group(re, "tail"), &tn);
	rn_str low = {0};
	if (rn_py_lower(abbr, an, &low) < 0)
		return -1;
	int before = low.n >= 2 && low.s[0] == U'д' && low.s[1] == U'о';
	rn_str_free(&low);
	const uint32_t *s = m->s;
	size_t n = m->n, i = rn_first_non_ws(s, n, m->end[0], NULL);
	int keep = 0;
	size_t a, b;
	rn_py_strip(abbr, an, &a, &b);   /* rstrip; abbr starts with a letter */
	if (b > a && abbr[b - 1] == '.') {
		int nl = 0;
		for (size_t k = 0; k < tn; k++)
			nl |= tail[k] == '\n';
		keep = i >= n || nl || (i < n && rn_py_isupper(s[i]));
		if (i < n && in_str(s[i], RN_U("\"»”)]}")) && i + 1 < n && rn_py_isupper(s[i + 1]))
			keep = 1;
	}
	if (rn_puts(out, before ? RN_U("до нашей эры") : RN_U("нашей эры")) < 0 ||
	    (keep && rn_str_putc(out, '.') < 0))
		return -1;
	return rn_str_put(out, tail, tn);
}

int rn_pre_normalize_era_abbreviations(const rn_str *in, rn_str *out)
{
	return rn_sub(PU(ERA_ABBREVIATION_PATTERN), in->s ? in->s : RN_U(""), in->n, era_repl, NULL,
	              0, out) < 0
	               ? -1
	               : 0;
}

/* the template ctx; R25: capitalised when the abbreviation was ("Ок. 5", "Т. 3") and starts
 * a sentence ("см. Т. 3" stays "том") */
static int cap_repl(const rn_match *m, rn_str *out, void *ctx)
{
	const uint32_t *w = ctx;
	size_t st = m->start[0], i = st;
	int cap = 0;
	if ((rn_fixes & RN_FIX_ABBR_SENT_CAP) && rn_py_isupper(m->s[st])) {
		while (i && (rn_py_isspace(m->s[i - 1]) || in_str(m->s[i - 1], RN_U("\"«„“([—–-"))))
			i--;
		cap = !i || in_str(m->s[i - 1], RN_U("!?…"));
		if (i && m->s[i - 1] == '.') {
			/* not after an abbreviation ("См. Т. 3"): a word of 4+ letters or a number */
			size_t k = i - 1;
			while (k && rn_py_isalpha(m->s[k - 1]))
				k--;
			cap = i - 1 - k >= 4 || (i - 1 == k && k && rn_py_isdigit(m->s[k - 1]));
		}
	}
	if (cap) {
		if (rn_py_upper(w, 1, out) < 0)
			return -1;
		w++;
	}
	return rn_puts(out, w);
}

int rn_pre_normalize_numeric_abbreviations(const rn_str *in, rn_str *out)
{
	rn_str t = {0};
	int r = -1;
	if (rn_str_copy(&t, in) < 0)
		return -1;
	if (rn_resub(AC(BIRTH_YEAR_ABBREVIATION_PATTERN), &t, birth_repl, NULL) < 0 ||
	    rn_resub(AC(MASS_GRAM_ABBREVIATION_PATTERN), &t, mass_repl, NULL) < 0 ||
	    rn_resub(PU(APPROXIMATE_ABBREVIATION_PATTERN), &t, cap_repl, (void *)RN_U("около ")) < 0 ||
	    rn_resub(PU(BIBLIOGRAPHIC_NUMBER_ABBREVIATIONS_0_0), &t, cap_repl, (void *)RN_U("том ")) < 0 ||
	    rn_resub(PU(BIBLIOGRAPHIC_NUMBER_ABBREVIATIONS_1_0), &t, cap_repl, (void *)RN_U("том ")) < 0 ||
	    rn_resub(PU(BIBLIOGRAPHIC_NUMBER_ABBREVIATIONS_2_0), &t, cap_repl, (void *)RN_U("номер ")) < 0 ||
	    rn_resub(PU(LEGAL_ARTICLE_REFERENCE_PATTERN), &t, legal_repl, NULL) < 0)
		goto done;
	r = rn_pre_normalize_era_abbreviations(&t, out);
done:
	rn_str_free(&t);
	return r;
}
