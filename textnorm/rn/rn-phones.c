/* phones.py: international phone numbers with an explicit "+" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-num.h"
#include "rn-pat.h"
#include "rn-phone.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-units.h"
#include "rn-util.h"

#define PH(id) rn_pat(RN_PHONES_##id)

static const uint32_t SP[] = {' ', '\t', 0xa0, 0x202f, 0x2009, 0};
static const uint32_t HY[] = {'-', 0x2010, 0x2011, 0};

static int in_str(uint32_t c, const uint32_t *set)
{
	for (; *set; set++)
		if (*set == c)
			return 1;
	return 0;
}

static int in_both(uint32_t c, const uint32_t *a, const uint32_t *b)
{
	return in_str(c, a) || in_str(c, b);
}

/* s[a:b] made only of chars in the two sets (str.strip(chars) == "") */
static int only(const uint32_t *s, size_t a, size_t b, const uint32_t *x, const uint32_t *y)
{
	for (; a < b; a++)
		if (!in_both(s[a], x, y))
			return 0;
	return 1;
}

static long rfind_nl(const uint32_t *s, size_t a, size_t b)
{
	while (b > a)
		if (s[--b] == '\n')
			return (long)b;
	return -1;
}

/* _phone_context: 1, 0, -1 error */
static int phone_context(const uint32_t *s, size_t n, size_t start, size_t end)
{
	size_t la = start > 80 ? start - 80 : 0, rb = end + 80 < n ? end + 80 : n;
	long nl = rfind_nl(s, la, start);
	size_t ca = nl < 0 ? la : (size_t)nl + 1;   /* left_clause = s[ca:start] */
	rn_iter it = rn_iter_init(PH(PHONE_CONTEXT), s + ca, start - ca, 0);
	rn_match m;
	int r, found = 0;
	size_t last = 0;
	while ((r = rn_iter_next(&it, &m)) == 1) {
		found = 1;
		last = m.end[0];
	}
	if (r < 0)
		return -1;
	if (found) {
		r = rn_search(rn_pat(RN_PHONES_L41), s + ca + last, start - ca - last, 0, &m);
		if (r < 0)
			return -1;
		if (!r)
			return 1;
	}
	size_t re = end;
	while (re < rb && s[re] != '\n')
		re++;
	int nl_right = re < rb;
	return (start <= 80 || nl >= 0) && (n - end <= 80 || nl_right) &&
	       only(s, ca, start, SP, RN_U("(«\"")) && only(s, end, re, SP, RN_U(").»\""));
}

#define MAXG 40

/* _written_groups: the number of groups, 0 for None; spans in ga/gb */
static int written_groups(const uint32_t *raw, size_t n, size_t *ga, size_t *gb)
{
	size_t open = 0, close = 0;
	for (size_t i = 0; i < n; i++) {
		open += raw[i] == '(';
		close += raw[i] == ')';
	}
	rn_match m;
	if (open || close) {
		if (open != 1 || close != 1)
			return 0;
		if (rn_search(PH(PAREN_GROUP), raw, n, 0, &m) != 1)
			return 0;
	}
	int k = 0;
	for (size_t i = 0; i < n;) {
		if (raw[i] < '0' || raw[i] > '9') {
			i++;
			continue;
		}
		size_t j = i;
		while (j < n && raw[j] >= '0' && raw[j] <= '9')
			j++;
		if (k == MAXG)
			return 0;
		ga[k] = i;
		gb[k++] = j;
		i = j;
	}
	for (int g = 1; g < k; g++) {
		int hy = 0, o = 0, c = 0;
		for (size_t i = gb[g - 1]; i < ga[g]; i++) {
			hy += in_str(raw[i], HY);
			o |= raw[i] == '(';
			c |= raw[i] == ')';
		}
		if (hy > 1 || (o && c))
			return 0;
	}
	return k;
}

/* _read_group */
static int read_group(rn_str *out, const char *g)
{
	int z = 0;
	while (g[z] == '0')
		z++;
	for (int i = 0; i < z; i++)
		if ((i && rn_str_putc(out, ' ') < 0) || rn_puts(out, RN_U("ноль")) < 0)
			return -1;
	if (!g[z])
		return 0;
	if (z && rn_str_putc(out, ' ') < 0)
		return -1;
	return rn_put_cardinal_a(out, g + z);   /* 1: raised */
}

static int unit_utf8(const char *u)
{
	rn_str t = {0};
	int r = rn_str_utf8(&t, u) < 0 ? -1 : rn_unit_resolve(t.s, t.n) != NULL;
	rn_str_free(&t);
	return r;
}

/* _is_quantity: 1, 0, -1 */
static int is_quantity(const uint32_t *t, size_t n)
{
	rn_match m;
	if (rn_unit_resolve(t, n))
		return 1;
	int r = rn_fullmatch(PH(QUANTITY_NOUN), t, n, 0, &m);
	if (r)
		return r;
	cmorphy3_arena *a = cmorphy3_arena_new();
	cmorphy3_form *f;
	if (!a)
		return -1;
	int k = rn_parse(a, t, n, &f);
	r = k < 0 ? -1 : 0;
	for (int i = 0; i < k && !r; i++)
		if (rn_tag_has(f[i].tag, "NOUN"))
			r = unit_utf8(f[i].normal_form);
	cmorphy3_arena_free(a);
	return r;
}

static int keep(const rn_match *m, rn_str *out)
{
	return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
}

static int phone_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const uint32_t *s = m->s;
	size_t n = m->n, start = m->start[0], b = m->end[0];
	while (b > start && in_str(s[b - 1], SP))
		b--;
	size_t open = 0, close = 0;
	for (size_t i = start; i < b; i++) {
		open += s[i] == '(';
		close += s[i] == ')';
	}
	if (b > start && s[b - 1] == ')' && close == open + 1) {
		b--;
		while (b > start && in_str(s[b - 1], SP))
			b--;
	}
	const uint32_t *raw = s + start;
	size_t rn = b - start, end = b, an = n - end < 80 ? n - end : 80;
	const uint32_t *after = s + end;
	if (!rn_py_isdigit(raw[rn - 1]) && raw[rn - 1] != ')')
		return keep(m, out);
	if (an && (rn_py_isalnum(after[0]) || in_both(after[0], RN_U("_/-"), HY)))
		return keep(m, out);
	rn_match t;
	int r = rn_match_at(rn_pat(RN_PHONES_L108), after, an, 0, &t);
	if (r)
		return r < 0 ? -1 : keep(m, out);
	size_t i = 0;
	while (i < an && in_str(after[i], SP))
		i++;
	if (i < an && in_str(after[i], RN_U("+=*/^<>")))
		return keep(m, out);
	size_t bs = start > 80 ? start - 80 : 0, be = start;
	while (be > bs && in_str(s[be - 1], SP))
		be--;
	if (be > bs && (rn_py_isdigit(s[be - 1]) || in_str(s[be - 1], RN_U("=+*/^<>−-"))))
		return keep(m, out);
	if ((r = rn_match_at(PH(RIGHT_TOKEN), after, an, 0, &t)) < 0)
		return -1;
	if (r) {
		size_t tn;
		const uint32_t *tok = rn_group(&t, 1, &tn);
		if ((r = is_quantity(tok, tn)) != 0)
			return r < 0 ? -1 : keep(m, out);
	}
	if ((r = phone_context(s, n, start, end)) <= 0)
		return r < 0 ? -1 : keep(m, out);
	size_t ga[MAXG], gb[MAXG];
	int k = written_groups(raw, rn, ga, gb);
	if (!k)
		return keep(m, out);
	char digits[24], grp[MAXG + 2][24];
	size_t dn = 0;
	for (int g = 0; g < k; g++) {
		size_t l = gb[g] - ga[g];
		if (dn + l > 15)
			return keep(m, out);
		for (size_t j = 0; j < l; j++)
			digits[dn++] = (char)raw[ga[g] + j];
		if (l > 15)
			return keep(m, out);
		for (size_t j = 0; j < l; j++)
			grp[g][j] = (char)raw[ga[g] + j];
		grp[g][l] = 0;
	}
	digits[dn] = 0;
	if (dn < 1)
		return keep(m, out);
	int cc;
	char intl[64];
	if ((r = rn_phone_international(digits, &cc, intl, sizeof intl)) <= 0)
		return r < 0 ? -1 : keep(m, out);
	char country[8];
	snprintf(country, sizeof country, "%d", cc);
	size_t cl = strlen(country);
	if (k == 1) {
		k = 0;
		for (char *sv, *p = strtok_r(intl, " ", &sv); p && k < MAXG; p = strtok_r(NULL, " ", &sv))
			snprintf(grp[k++], sizeof grp[0], "%s", p);
	} else if (strcmp(grp[0], country)) {
		if (strncmp(grp[0], country, cl))
			return keep(m, out);
		memmove(grp[1 + 1], grp[1], sizeof grp[0] * (size_t)(k - 1));
		snprintf(grp[1], sizeof grp[0], "%s", grp[0] + cl);
		snprintf(grp[0], sizeof grp[0], "%s", country);
		k++;
	}
	size_t mark = out->n;
	if (rn_puts(out, RN_U("плюс ")) < 0)
		return -1;
	for (int g = 0; g < k; g++) {
		if (g && rn_puts(out, RN_U(", ")) < 0)
			return -1;
		if ((r = read_group(out, grp[g])) != 0) {
			if (r < 0)
				return -1;
			out->n = mark;
			return keep(m, out);
		}
	}
	return rn_str_put(out, s + end, m->end[0] - end);
}

int rn_pre_normalize_international_phone_numbers(const rn_str *in, rn_str *out)
{
	const uint32_t *s = in->s ? in->s : RN_U("");
	int plus = 0;
	for (size_t i = 0; i < in->n && !plus; i++)
		plus = s[i] == '+';
	if (!plus)
		return rn_str_put(out, s, in->n);
	return rn_sub(PH(CANDIDATE), s, in->n, phone_repl, NULL, 0, out) < 0 ? -1 : 0;
}
