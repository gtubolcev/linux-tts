/* small helpers shared by the rn passes */
#ifndef RN_UTIL_H
#define RN_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <uchar.h>

#include "rn-re.h"

/* UTF-32 literal: RN_U("слэш") */
#define RN_U(s) ((const uint32_t *)U##s)

static inline size_t rn_u32len(const uint32_t *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static inline int rn_puts(rn_str *b, const uint32_t *s)
{
	return rn_str_put(b, s, rn_u32len(s));
}

static inline int rn_is_ascii_alpha(uint32_t c)
{
	return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
}

static inline int rn_is_ascii_digit(uint32_t c)
{
	return c >= '0' && c <= '9';
}

/* Python's str.isspace() (the same set as re's \s) */
static inline int rn_is_space(uint32_t c)
{
	return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 ||
	       c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
	       c == 0x202F || c == 0x205F || c == 0x3000;
}

/* a group of m, as pointer and length; *n = 0 and NULL if unmatched */
static inline const uint32_t *rn_group(const rn_match *m, int g, size_t *n)
{
	if (g < 0 || (uint32_t)g > m->ngroups || m->start[g] == RN_UNSET) {
		*n = 0;
		return NULL;
	}
	*n = m->end[g] - m->start[g];
	return m->s + m->start[g];
}

/* s[0..n) equals the ASCII string a, ignoring ASCII case of s (a is lower case) */
static inline int rn_eq_ascii_ci(const uint32_t *s, size_t n, const char *a)
{
	size_t i = 0;
	for (; i < n && a[i]; i++) {
		uint32_t c = s[i];
		if (c >= 'A' && c <= 'Z')
			c |= 0x20;
		if (c != (unsigned char)a[i])
			return 0;
	}
	return i == n && !a[i];
}

#endif
