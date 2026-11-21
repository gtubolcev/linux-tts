#include "rn-num.h"

#include <stdio.h>
#include <string.h>

#include "rn-pystr.h"

unsigned rn_n2w_fixes;
unsigned rn_fixes;

int rn_int_str(const uint32_t *s, size_t n, char *buf, size_t cap)
{
	size_t i = 0, k = 0;
	while (i + 1 < n && rn_py_decimal(s[i]) == 0)
		i++;
	for (; i < n; i++) {
		if (k + 1 >= cap)
			return -1;
		buf[k++] = (char)('0' + rn_py_decimal(s[i]));
	}
	if (!k)
		return -1;
	buf[k] = 0;
	return (int)k;
}

static int put_n2w(rn_str *out, int ordinal, const char *number, n2w_opts o)
{
	char buf[1024];
	int len = ordinal ? n2w_ru_ordinal(number, o, buf, sizeof buf)
	                  : n2w_ru_cardinal(number, o, buf, sizeof buf);
	if (len < 0 || (size_t)len >= sizeof buf)
		return 1;   /* raised */
	return rn_str_utf8(out, buf) < 0 ? -1 : 0;
}

int rn_put_cardinal_a(rn_str *out, const char *number)
{
	n2w_opts o = N2W_DEFAULTS;
	o.fixes = rn_n2w_fixes;
	return put_n2w(out, 0, number, o);
}

int rn_put_cardinal(rn_str *out, const uint32_t *s, size_t n)
{
	char num[256];
	if (rn_int_str(s, n, num, sizeof num) < 0)
		return rn_str_put(out, s, n);
	n2w_opts o = N2W_DEFAULTS;
	o.fixes = rn_n2w_fixes;
	int r = put_n2w(out, 0, num, o);
	return r == 1 ? rn_str_utf8(out, num) : r;
}

int rn_put_ordinal(rn_str *out, const char *number, enum n2w_case c, int gender, int plural,
                   int inanimate)
{
	if (c == N2W_ACCS && inanimate && (plural || gender == N2W_MASC || gender == N2W_NEUT))
		c = N2W_NOMN;
	n2w_opts o = N2W_DEFAULTS;
	o.fixes = rn_n2w_fixes;
	o.gcase = c;
	o.plural = plural;
	if (!plural && gender != RN_NO_GENDER)
		o.gender = (enum n2w_gender)gender;
	int r = put_n2w(out, 1, number, o);
	if (r != 1)
		return r;
	/* TODO the parse/inflect fallback of render_ordinal: num2words' ordinal does not raise
	 * for the numbers the passes give it (non-negative, < 10**33) */
	o = N2W_DEFAULTS;
	o.fixes = rn_n2w_fixes;
	r = put_n2w(out, 1, number, o);
	return r == 1 ? rn_str_utf8(out, number) : r;
}

long rn_roman_value(const uint32_t *s, size_t n)
{
	static const struct {
		const char *r;
		int v;
	} MAP[] = {{"M", 1000}, {"CM", 900}, {"D", 500}, {"CD", 400}, {"C", 100}, {"XC", 90},
	           {"L", 50},   {"XL", 40},  {"X", 10},  {"IX", 9},   {"V", 5},   {"IV", 4},
	           {"I", 1}};
	char u[64];
	rn_str up = {0};
	if (!n || rn_py_upper(s, n, &up) < 0 || up.n >= sizeof u) {
		rn_str_free(&up);
		return -1;
	}
	for (size_t i = 0; i < up.n; i++) {
		if (up.s[i] >= 0x80) {
			rn_str_free(&up);
			return -1;
		}
		u[i] = (char)up.s[i];
	}
	u[up.n] = 0;
	rn_str_free(&up);
	if (!strcmp(u, "N"))
		return 0;
	/* romanNumeralPattern accepts exactly the canonical numerals of 1..4999 */
	long v = 0;
	size_t i = 0;
	for (size_t k = 0; k < sizeof MAP / sizeof *MAP; k++) {
		size_t l = strlen(MAP[k].r);
		while (!strncmp(u + i, MAP[k].r, l)) {
			v += MAP[k].v;
			i += l;
		}
	}
	if (v < 1 || v > 4999)
		return -1;
	char canon[64];
	size_t c = 0;
	long w = v;
	for (size_t k = 0; k < sizeof MAP / sizeof *MAP; k++)
		while (w >= MAP[k].v) {
			c += (size_t)snprintf(canon + c, sizeof canon - c, "%s", MAP[k].r);
			w -= MAP[k].v;
		}
	return strcmp(canon, u) ? -1 : v;
}

int rn_put_capitalized(rn_str *out, const uint32_t *s, size_t n)
{
	if (!n)
		return 0;
	/* titlecase of the first character: the same as upper for the letters met here */
	return rn_py_upper(s, 1, out) < 0 || rn_py_lower(s + 1, n - 1, out) < 0 ? -1 : 0;
}
