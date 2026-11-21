#include "rn-pystr.h"
#include "rn-pyuni.h"

#define N(a) (sizeof a / sizeof *a)

static int in_ranges(const uint32_t (*r)[2], size_t n, uint32_t c)
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (c < r[mid][0])
			hi = mid;
		else if (c > r[mid][1])
			lo = mid + 1;
		else
			return 1;
	}
	return 0;
}

#define IS(tab, c) in_ranges(tab, N(tab), (c))

int rn_py_isalpha(uint32_t c) { return c < 0x80 ? ((c | 0x20) - 'a' < 26) : IS(RN_PY_ALPHA, c); }
int rn_py_isdecimal(uint32_t c) { return c < 0x80 ? c - '0' < 10 : IS(RN_PY_DECIMAL, c); }
int rn_py_isdigit(uint32_t c) { return c < 0x80 ? c - '0' < 10 : IS(RN_PY_DIGIT, c); }
int rn_py_isnumeric(uint32_t c) { return c < 0x80 ? c - '0' < 10 : IS(RN_PY_NUMERIC, c); }
int rn_py_isupper(uint32_t c) { return c < 0x80 ? c - 'A' < 26 : IS(RN_PY_UPPER, c); }
int rn_py_islower(uint32_t c) { return c < 0x80 ? c - 'a' < 26 : IS(RN_PY_LOWER, c); }
int rn_py_isspace(uint32_t c) { return IS(RN_PY_SPACE, c); }

int rn_py_isalnum(uint32_t c)
{
	return rn_py_isalpha(c) || rn_py_isdecimal(c) || rn_py_isdigit(c) || rn_py_isnumeric(c);
}

static const uint32_t *map_entry(const uint32_t (*t)[4], size_t n, uint32_t c)
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (c < t[mid][0])
			hi = mid;
		else if (c > t[mid][0])
			lo = mid + 1;
		else
			return t[mid];
	}
	return NULL;
}

static int put_map(const uint32_t (*t)[4], size_t nt, const uint32_t *s, size_t n, rn_str *out,
                   int sigma)
{
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (sigma && c == 0x3A3) {   /* final sigma, as in str.lower() */
			size_t j;
			uint32_t d = 0;
			int fs = 0;
			for (j = i; j > 0; j--) {
				d = s[j - 1];
				if (!IS(RN_PY_CASE_IGNORABLE, d))
					break;
			}
			fs = j > 0 && IS(RN_PY_CASED, d);
			if (fs) {
				for (j = i + 1; j < n; j++)
					if (!IS(RN_PY_CASE_IGNORABLE, s[j]))
						break;
				fs = j == n || !IS(RN_PY_CASED, s[j]);
			}
			if (rn_str_putc(out, fs ? 0x3C2 : 0x3C3) < 0)
				return -1;
			continue;
		}
		const uint32_t *e = map_entry(t, nt, c);
		if (!e) {
			if (rn_str_putc(out, c) < 0)
				return -1;
			continue;
		}
		for (int k = 1; k < 4 && e[k]; k++)
			if (rn_str_putc(out, e[k]) < 0)
				return -1;
	}
	return 0;
}

int rn_py_lower(const uint32_t *s, size_t n, rn_str *out)
{
	return put_map(RN_PY_LOWERMAP, N(RN_PY_LOWERMAP), s, n, out, 1);
}

int rn_py_upper(const uint32_t *s, size_t n, rn_str *out)
{
	return put_map(RN_PY_UPPERMAP, N(RN_PY_UPPERMAP), s, n, out, 0);
}

int rn_py_casefold(const uint32_t *s, size_t n, rn_str *out)
{
	return put_map(RN_PY_FOLDMAP, N(RN_PY_FOLDMAP), s, n, out, 0);
}

/* isupper(): at least one cased character and no lowercase or titlecase one */
static int str_case(const uint32_t *s, size_t n, int upper)
{
	int any = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (!IS(RN_PY_CASED, c))
			continue;
		if (upper ? !rn_py_isupper(c) : !rn_py_islower(c))
			return 0;
		any = 1;
	}
	return any;
}

int rn_py_str_isupper(const uint32_t *s, size_t n) { return str_case(s, n, 1); }
int rn_py_str_islower(const uint32_t *s, size_t n) { return str_case(s, n, 0); }

int rn_py_str_isdigit(const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!rn_py_isdigit(s[i]))
			return 0;
	return n > 0;
}

int rn_py_str_isalpha(const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!rn_py_isalpha(s[i]))
			return 0;
	return n > 0;
}

int rn_py_decimal(uint32_t c)
{
	if (c < 0x80)
		return c - '0' < 10 ? (int)(c - '0') : -1;
	/* Nd comes in runs of ten starting at zero; merged ranges stay aligned */
	size_t lo = 0, hi = N(RN_PY_DECIMAL);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (c < RN_PY_DECIMAL[mid][0])
			hi = mid;
		else if (c > RN_PY_DECIMAL[mid][1])
			lo = mid + 1;
		else
			return (int)((c - RN_PY_DECIMAL[mid][0]) % 10);
	}
	return -1;
}

long rn_py_find(const uint32_t *s, size_t n, const uint32_t *old, size_t on, size_t pos)
{
	if (on > n)
		return -1;
	for (size_t i = pos; i + on <= n; i++) {
		size_t k = 0;
		while (k < on && s[i + k] == old[k])
			k++;
		if (k == on)
			return (long)i;
	}
	return -1;
}

int rn_py_replace(const uint32_t *s, size_t n, const uint32_t *old, size_t on,
                  const uint32_t *new, size_t nn, rn_str *out)
{
	size_t last = 0;
	long i;
	if (!on)   /* never used with an empty needle */
		return rn_str_put(out, s, n);
	while ((i = rn_py_find(s, n, old, on, last)) >= 0) {
		if (rn_str_put(out, s + last, (size_t)i - last) < 0 || rn_str_put(out, new, nn) < 0)
			return -1;
		last = (size_t)i + on;
	}
	return rn_str_put(out, s + last, n - last);
}

int rn_py_rereplace(rn_str *cur, const uint32_t *old, const uint32_t *new)
{
	size_t on = 0, nn = 0;
	while (old[on])
		on++;
	while (new[nn])
		nn++;
	if (!cur->n || rn_py_find(cur->s, cur->n, old, on, 0) < 0)
		return 0;
	rn_str t = {0};
	if (rn_py_replace(cur->s, cur->n, old, on, new, nn, &t) < 0) {
		rn_str_free(&t);
		return -1;
	}
	rn_str_free(cur);
	*cur = t;
	return 0;
}

void rn_py_strip(const uint32_t *s, size_t n, size_t *a, size_t *b)
{
	size_t i = 0, k = n;
	while (i < k && rn_py_isspace(s[i]))
		i++;
	while (k > i && rn_py_isspace(s[k - 1]))
		k--;
	*a = i;
	*b = k;
}
