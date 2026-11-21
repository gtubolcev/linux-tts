/* tn-morph: pymorphy3 2.0.6 in C. See morph.h.
 *
 * The code follows pymorphy3's analyzer units one by one (names in the comments refer to
 * pymorphy3/units/ and dawg_python/); where Python semantics matter (slicing, sum(),
 * sort stability, truthiness of DAWG indices) they are reproduced exactly. Strings are
 * handled as arrays of code points inside, UTF-8 at the API.
 *
 * The dictionary file is third-party data: every index read from it is checked.
 */
#include "morph.h"

#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unidata.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ arena */

struct chunk {
	struct chunk *next;
	size_t used, size;
	_Alignas(16) unsigned char data[];
};

struct tn_arena {
	struct chunk *head, *cur;   /* chunks before cur are full; reset keeps them all */
	jmp_buf *oom;               /* set while a tn_morph_* call runs */
};

tn_arena *tn_arena_new(void)
{
	return calloc(1, sizeof(tn_arena));
}

void tn_arena_reset(tn_arena *a)
{
	if (!a)
		return;
	for (struct chunk *c = a->head; c; c = c->next)
		c->used = 0;
	a->cur = a->head;
}

void tn_arena_free(tn_arena *a)
{
	if (!a)
		return;
	for (struct chunk *c = a->head; c;) {
		struct chunk *n = c->next;
		free(c);
		c = n;
	}
	free(a);
}

void *tn_arena_alloc(tn_arena *a, size_t n)
{
	if (n > SIZE_MAX / 2)
		goto oom;
	n = (n + 15) & ~(size_t)15;
	if (n == 0)
		n = 16;
	struct chunk *c = a->cur;
	while (c && c->size - c->used < n) {
		if (!c->next)
			break;
		c = c->next;
	}
	if (!c || c->size - c->used < n) {
		size_t sz = 256 * 1024;
		if (sz < n)
			sz = n;
		struct chunk *nc = malloc(sizeof(struct chunk) + sz);
		if (!nc)
			goto oom;
		nc->size = sz;
		nc->used = 0;
		nc->next = NULL;
		if (c)
			c->next = nc;
		else
			a->head = nc;
		c = nc;
	}
	a->cur = c;
	void *p = c->data + c->used;
	c->used += n;
	return p;
oom:
	if (a->oom)
		longjmp(*a->oom, 1);
	return NULL;
}

char *tn_arena_strdup(tn_arena *a, const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = tn_arena_alloc(a, n);
	if (p)
		memcpy(p, s, n);
	return p;
}

/* ---------------------------------------------------------------- unicode */

typedef struct {
	const uint32_t *s;
	int n;
} ustr;

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

#define U_IS(tab, c) in_ranges(tab, N(tab), (c))

static int u_decimal(uint32_t c)
{
	size_t lo = 0, hi = N(U_DECIMAL);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (c < U_DECIMAL[mid][0])
			hi = mid;
		else if (c > U_DECIMAL[mid][1])
			lo = mid + 1;
		else
			return (int)(U_DECIMAL[mid][2] + (c - U_DECIMAL[mid][0]));
	}
	return -1;
}

static const uint32_t *u_lower_entry(uint32_t c)
{
	size_t lo = 0, hi = N(U_LOWER);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (c < U_LOWER[mid][0])
			hi = mid;
		else if (c > U_LOWER[mid][0])
			lo = mid + 1;
		else
			return U_LOWER[mid];
	}
	return NULL;
}

static void *aalloc(tn_arena *a, size_t n)
{
	return tn_arena_alloc(a, n);
}

/* UTF-8 → code points; invalid bytes become U+FFFD */
static ustr u_from_utf8(tn_arena *a, const char *s, size_t len)
{
	uint32_t *o = aalloc(a, (len + 1) * sizeof(uint32_t));
	int n = 0;
	const unsigned char *p = (const unsigned char *)s, *e = p + len;
	while (p < e) {
		uint32_t c = *p;
		int k = c < 0x80 ? 0 : c < 0xC2 ? -1 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : c < 0xF5 ? 3 : -1;
		if (k < 0 || e - p <= k) {
			o[n++] = 0xFFFD;
			p++;
			continue;
		}
		if (k == 0) {
			o[n++] = c;
			p++;
			continue;
		}
		c &= 0x3F >> k;
		int ok = 1;
		for (int i = 1; i <= k; i++) {
			if ((p[i] & 0xC0) != 0x80) {
				ok = 0;
				break;
			}
			c = (c << 6) | (p[i] & 0x3F);
		}
		if (!ok || (k == 2 && (c < 0x800 || (c >= 0xD800 && c < 0xE000))) ||
		    (k == 3 && (c < 0x10000 || c > 0x10FFFF))) {
			o[n++] = 0xFFFD;
			p++;
			continue;
		}
		o[n++] = c;
		p += k + 1;
	}
	return (ustr){o, n};
}

static int utf8_put(unsigned char *o, uint32_t c)
{
	if (c < 0x80) {
		o[0] = (unsigned char)c;
		return 1;
	}
	if (c < 0x800) {
		o[0] = (unsigned char)(0xC0 | (c >> 6));
		o[1] = (unsigned char)(0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000) {
		o[0] = (unsigned char)(0xE0 | (c >> 12));
		o[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
		o[2] = (unsigned char)(0x80 | (c & 0x3F));
		return 3;
	}
	o[0] = (unsigned char)(0xF0 | (c >> 18));
	o[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
	o[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
	o[3] = (unsigned char)(0x80 | (c & 0x3F));
	return 4;
}

static char *u_to_utf8(tn_arena *a, ustr s)
{
	unsigned char *o = aalloc(a, (size_t)s.n * 4 + 1);
	size_t k = 0;
	for (int i = 0; i < s.n; i++)
		k += (size_t)utf8_put(o + k, s.s[i]);
	o[k] = 0;
	return (char *)o;
}

static ustr u_cat(tn_arena *a, ustr x, ustr y)
{
	uint32_t *o = aalloc(a, ((size_t)x.n + y.n + 1) * sizeof(uint32_t));
	if (x.n)
		memcpy(o, x.s, (size_t)x.n * sizeof(uint32_t));
	if (y.n)
		memcpy(o + x.n, y.s, (size_t)y.n * sizeof(uint32_t));
	return (ustr){o, x.n + y.n};
}

static ustr u_cat3(tn_arena *a, ustr x, ustr y, ustr z)
{
	return u_cat(a, u_cat(a, x, y), z);
}

/* Python s[i:j] with negative indices and clamping */
static ustr u_slice(ustr s, long i, long j)
{
	if (i < 0)
		i += s.n;
	if (i < 0)
		i = 0;
	if (i > s.n)
		i = s.n;
	if (j < 0)
		j += s.n;
	if (j < 0)
		j = 0;
	if (j > s.n)
		j = s.n;
	if (j < i)
		j = i;
	return (ustr){s.s + i, (int)(j - i)};
}

static int u_eq(ustr x, ustr y)
{
	return x.n == y.n && (x.n == 0 || memcmp(x.s, y.s, (size_t)x.n * sizeof(uint32_t)) == 0);
}

static int u_startswith(ustr s, ustr p)
{
	return s.n >= p.n && (p.n == 0 || memcmp(s.s, p.s, (size_t)p.n * sizeof(uint32_t)) == 0);
}

static int u_endswith(ustr s, ustr p)
{
	return s.n >= p.n &&
	       (p.n == 0 || memcmp(s.s + s.n - p.n, p.s, (size_t)p.n * sizeof(uint32_t)) == 0);
}

static int u_index(ustr s, uint32_t c)
{
	for (int i = 0; i < s.n; i++)
		if (s.s[i] == c)
			return i;
	return -1;
}

static const uint32_t HYPHEN[] = {'-'};
static const ustr U_HYPHEN = {HYPHEN, 1};

/* str.lower(), with the final-sigma rule of CPython's handle_capital_sigma() */
static ustr u_lower(tn_arena *a, ustr s)
{
	uint32_t *o = aalloc(a, ((size_t)s.n * 3 + 1) * sizeof(uint32_t));
	int n = 0;
	for (int i = 0; i < s.n; i++) {
		uint32_t c = s.s[i];
		if (c < 0x80) {
			o[n++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
			continue;
		}
		if (c == 0x3A3) {
			int j;
			uint32_t d = 0;
			for (j = i - 1; j >= 0; j--) {
				d = s.s[j];
				if (!U_IS(U_CASE_IGNORABLE, d))
					break;
			}
			int fs = j >= 0 && U_IS(U_CASED, d);
			if (fs && i + 1 < s.n) {
				for (j = i + 1; j < s.n; j++) {
					d = s.s[j];
					if (!U_IS(U_CASE_IGNORABLE, d))
						break;
				}
				fs = j == s.n || !U_IS(U_CASED, d);
			}
			o[n++] = fs ? 0x3C2 : 0x3C3;
			continue;
		}
		const uint32_t *e = u_lower_entry(c);
		if (!e) {
			o[n++] = c;
			continue;
		}
		for (uint32_t k = 0; k < e[1]; k++)
			o[n++] = e[2 + k];
	}
	return (ustr){o, n};
}

/* ------------------------------------------------------------- shapes.py */

static int is_space(uint32_t c)
{
	return U_IS(U_SPACE, c);
}

/* unicodedata.name(c) contains LATIN, for letters */
static int is_latin(ustr w)
{
	int any = 0;
	for (int i = 0; i < w.n; i++) {
		if (!U_IS(U_ALPHA, w.s[i]))
			continue;
		any = 1;
		if (!U_IS(U_LATIN, w.s[i]))
			return 0;
	}
	return any;
}

static int is_punctuation(ustr w)
{
	if (w.n == 0)
		return 0;
	int all_space = 1;
	for (int i = 0; i < w.n; i++)
		if (!is_space(w.s[i]))
			all_space = 0;
	if (all_space)
		return 0;
	for (int i = 0; i < w.n; i++)
		if (!is_space(w.s[i]) && !U_IS(U_PUNCT, w.s[i]))
			return 0;
	return 1;
}

/* ROMAN_NUMBERS_RE.match(): M{0,4}(CM|CD|D?C{0,3})(XC|XL|L?X{0,3})(IX|IV|V?I{0,3})$ with
 * IGNORECASE (I also matches U+0130 and U+0131) */
static const char *const ROMAN_GROUPS[4][10] = {
	{"", "M", "MM", "MMM", "MMMM"},
	{"CM", "CD", "", "C", "CC", "CCC", "D", "DC", "DCC", "DCCC"},
	{"XC", "XL", "", "X", "XX", "XXX", "L", "LX", "LXX", "LXXX"},
	{"IX", "IV", "", "I", "II", "III", "V", "VI", "VII", "VIII"},
};

static int roman_match(const char *s, int n, int pos, int g)
{
	if (g == 4)
		return pos == n;
	for (int k = 0; k < 10; k++) {
		const char *alt = ROMAN_GROUPS[g][k];
		if (!alt)
			break;
		int l = (int)strlen(alt);
		if (pos + l <= n && memcmp(s + pos, alt, (size_t)l) == 0 &&
		    roman_match(s, n, pos + l, g + 1))
			return 1;
	}
	return 0;
}

static int is_roman_number(ustr w)
{
	if (w.n == 0)
		return 0;
	int n = w.n;
	if (w.s[n - 1] == '\n')
		n--;
	if (n > 32)
		return 0;
	char buf[33];
	for (int i = 0; i < n; i++) {
		uint32_t c = w.s[i];
		if (c == 0x130 || c == 0x131)
			c = 'I';
		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (c >= 128 || c == 0 || !strchr("MDCLXVI", (int)c))
			return 0;
		buf[i] = (char)c;
	}
	return roman_match(buf, n, 0, 0);
}

/* _PyUnicode_TransformDecimalAndSpaceToASCII; the result stops after the first '?' */
static int to_ascii_number(ustr w, char *o, int cap)
{
	int n = 0;
	for (int i = 0; i < w.n && n < cap - 1; i++) {
		uint32_t c = w.s[i];
		if (c < 127) {
			o[n++] = (char)c;
		} else if (is_space(c)) {
			o[n++] = ' ';
		} else {
			int d = u_decimal(c);
			if (d < 0) {
				o[n++] = '?';
				break;
			}
			o[n++] = (char)('0' + d);
		}
	}
	o[n] = 0;
	return n;
}

static int py_isspace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* int(word) succeeds (base 10, underscores between digits, at most 4300 digits) */
static int py_int_ok(const char *s, int n)
{
	int i = 0, digits = 0;
	if ((int)strlen(s) != n)
		return 0;   /* NUL inside */
	while (i < n && py_isspace(s[i]))
		i++;
	if (i < n && (s[i] == '+' || s[i] == '-'))
		i++;
	if (i >= n || !is_digit(s[i]))
		return 0;
	while (i < n) {
		if (is_digit(s[i])) {
			digits++;
			i++;
		} else if (s[i] == '_' && i + 1 < n && is_digit(s[i + 1])) {
			i++;
		} else {
			break;
		}
	}
	while (i < n && py_isspace(s[i]))
		i++;
	return i == n && digits <= 4300;
}

static int ci_prefix(const char *s, int n, const char *w)
{
	int l = (int)strlen(w);
	if (n < l)
		return 0;
	for (int i = 0; i < l; i++) {
		char c = s[i];
		if (c >= 'A' && c <= 'Z')
			c += 32;
		if (c != w[i])
			return 0;
	}
	return l;
}

/* float(word) succeeds */
static int py_float_ok(const char *s, int n, char *buf)
{
	if ((int)strlen(s) != n)
		return 0;
	while (n > 0 && py_isspace(s[n - 1]))
		n--;
	while (n > 0 && py_isspace(*s)) {
		s++;
		n--;
	}
	if (n == 0)
		return 0;
	/* _Py_string_to_number_with_underscores */
	int k = 0;
	char prev = 0;
	for (int i = 0; i < n; i++) {
		char c = s[i];
		if (c == '_') {
			if (!is_digit(prev))
				return 0;
		} else {
			buf[k++] = c;
			if (prev == '_' && !is_digit(c))
				return 0;
		}
		prev = c;
	}
	if (prev == '_')
		return 0;
	buf[k] = 0;
	s = buf;
	n = k;
	int i = 0;
	if (i < n && (s[i] == '+' || s[i] == '-'))
		i++;
	int l;
	if ((l = ci_prefix(s + i, n - i, "infinity")) || (l = ci_prefix(s + i, n - i, "inf")) ||
	    (l = ci_prefix(s + i, n - i, "nan")))
		return i + l == n;
	int mant = 0;
	while (i < n && is_digit(s[i])) {
		i++;
		mant++;
	}
	if (i < n && s[i] == '.') {
		i++;
		while (i < n && is_digit(s[i])) {
			i++;
			mant++;
		}
	}
	if (!mant)
		return 0;
	if (i < n && (s[i] == 'e' || s[i] == 'E')) {
		int j = i + 1;
		if (j < n && (s[j] == '+' || s[j] == '-'))
			j++;
		if (j < n && is_digit(s[j])) {
			while (j < n && is_digit(s[j]))
				j++;
			i = j;
		}
	}
	return i == n;
}

/* --------------------------------------------------------------- DAWG */

typedef struct {
	const uint32_t *units;
	uint32_t nunits;
	const uint8_t *guide;
	uint32_t nguide;   /* guide units; 2 bytes each */
} dawg;

#define ROOT 0

static inline uint32_t du_offset(uint32_t b)
{
	return (b >> 10) << ((b & (1u << 9)) >> 6);
}

static inline uint32_t du_label(uint32_t b)
{
	return b & 0x800000FFu;
}

/* Dictionary.follow_char; -1 for None */
static long d_follow_char(const dawg *d, uint32_t label, long index)
{
	if (index < 0 || (uint32_t)index >= d->nunits)
		return -1;
	uint32_t next = ((uint32_t)index ^ du_offset(d->units[index]) ^ label);
	if (next >= d->nunits || du_label(d->units[next]) != label)
		return -1;
	return next;
}

static long d_follow_bytes(const dawg *d, const unsigned char *s, int n, long index)
{
	for (int i = 0; i < n && index >= 0; i++)
		index = d_follow_char(d, s[i], index);
	return index;
}

static long d_follow_uchar(const dawg *d, uint32_t c, long index)
{
	unsigned char b[4];
	int n = utf8_put(b, c);
	return d_follow_bytes(d, b, n, index);
}

static int d_has_value(const dawg *d, long index)
{
	return index >= 0 && (uint32_t)index < d->nunits && (d->units[index] >> 8) & 1;
}

static long d_value(const dawg *d, long index)
{
	if (index < 0 || (uint32_t)index >= d->nunits)
		return -1;
	uint32_t vi = (uint32_t)index ^ du_offset(d->units[index]);
	if (vi >= d->nunits)
		return -1;
	return d->units[vi] & 0x7FFFFFFFu;
}

/* Dictionary.find */
static long d_find(const dawg *d, const unsigned char *key, int n)
{
	long index = d_follow_bytes(d, key, n, ROOT);
	if (index < 0 || !d_has_value(d, index))
		return -1;
	return d_value(d, index);
}

static unsigned g_child(const dawg *d, long index)
{
	return (uint32_t)index < d->nguide ? d->guide[index * 2] : 0;
}

static unsigned g_sibling(const dawg *d, long index)
{
	return (uint32_t)index < d->nguide ? d->guide[index * 2 + 1] : 0;
}

/* wrapper.Completer, for short keys (record payloads) */
#define COMPL_MAX 64
typedef struct {
	const dawg *d;
	long stack[COMPL_MAX + 1];
	int sp;
	unsigned char key[COMPL_MAX];
	int klen;
	long last;
} completer;

static void c_start(completer *c, const dawg *d, long index)
{
	c->d = d;
	c->klen = 0;
	c->sp = 0;
	if (d->nguide) {
		c->stack[c->sp++] = index;
		c->last = ROOT;
	}
}

static int c_push(completer *c, unsigned label, long index)
{
	if (c->klen >= COMPL_MAX || c->sp > COMPL_MAX)
		return 0;
	c->key[c->klen++] = (unsigned char)label;
	c->stack[c->sp++] = index;
	return 1;
}

static int c_find_terminal(completer *c, long index)
{
	while (!d_has_value(c->d, index)) {
		unsigned label = g_child(c->d, index);
		index = d_follow_char(c->d, label, index);
		if (index < 0 || !c_push(c, label, index))
			return 0;
	}
	c->last = index;
	return 1;
}

static int c_next(completer *c)
{
	if (c->sp == 0)
		return 0;
	long index = c->stack[c->sp - 1];
	if (c->last != ROOT) {
		unsigned child = g_child(c->d, index);
		if (child) {
			index = d_follow_char(c->d, child, index);
			if (index < 0 || !c_push(c, child, index))
				return 0;
		} else {
			for (;;) {
				unsigned sib = g_sibling(c->d, index);
				if (c->klen > 0)
					c->klen--;
				c->sp--;
				if (c->sp == 0)
					return 0;
				index = c->stack[c->sp - 1];
				if (sib) {
					index = d_follow_char(c->d, sib, index);
					if (index < 0 || !c_push(c, sib, index))
						return 0;
					break;
				}
			}
		}
	}
	return c_find_terminal(c, index);
}

/* binascii.a2b_base64 (non-strict) of a short key; returns the length or -1 */
static int b64_decode(const unsigned char *s, int n, unsigned char *o, int cap)
{
	uint32_t acc = 0;
	int bits = 0, k = 0, pads = 0;
	for (int i = 0; i < n; i++) {
		int v;
		unsigned char ch = s[i];
		if (ch >= 'A' && ch <= 'Z')
			v = ch - 'A';
		else if (ch >= 'a' && ch <= 'z')
			v = ch - 'a' + 26;
		else if (ch >= '0' && ch <= '9')
			v = ch - '0' + 52;
		else if (ch == '+')
			v = 62;
		else if (ch == '/')
			v = 63;
		else if (ch == '=') {
			pads++;
			continue;
		} else
			continue;
		if (pads)
			return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (k >= cap)
				return -1;
			o[k++] = (unsigned char)(acc >> bits);
		}
	}
	return k;
}

#define BYTES_SEP 0x01

/* one key of BytesDAWG.similar_items with its (big-endian) records */
typedef struct {
	ustr key;
	int nrec;
	const uint32_t *rec;   /* nrec × fields */
} ditem;

typedef struct {
	ditem *v;
	int n, cap;
} ditemvec;

static void div_push(tn_arena *a, ditemvec *x, ditem it)
{
	if (x->n == x->cap) {
		int cap = x->cap ? x->cap * 2 : 4;
		ditem *nv = aalloc(a, (size_t)cap * sizeof(ditem));
		if (x->n)
			memcpy(nv, x->v, (size_t)x->n * sizeof(ditem));
		x->v = nv;
		x->cap = cap;
	}
	x->v[x->n++] = it;
}

/* RecordDAWG._value_for_index: fmt ">HH" (fields 2) or ">IHH" (fields 3) */
static void records_for_index(tn_arena *a, const dawg *d, long index, int fields, ditem *it)
{
	completer c;
	int cap = 8, n = 0;
	uint32_t *rec = aalloc(a, (size_t)cap * fields * sizeof(uint32_t));
	c_start(&c, d, index);
	while (c_next(&c)) {
		unsigned char raw[16];
		int len = b64_decode(c.key, c.klen, raw, sizeof(raw));
		if (len != (fields == 2 ? 4 : 8))
			continue;
		if (n == cap) {
			uint32_t *nr = aalloc(a, (size_t)cap * 2 * fields * sizeof(uint32_t));
			memcpy(nr, rec, (size_t)n * fields * sizeof(uint32_t));
			rec = nr;
			cap *= 2;
		}
		uint32_t *r = rec + (size_t)n * fields;
		if (fields == 2) {
			r[0] = (uint32_t)raw[0] << 8 | raw[1];
			r[1] = (uint32_t)raw[2] << 8 | raw[3];
		} else {
			r[0] = (uint32_t)raw[0] << 24 | (uint32_t)raw[1] << 16 | (uint32_t)raw[2] << 8 | raw[3];
			r[1] = (uint32_t)raw[4] << 8 | raw[5];
			r[2] = (uint32_t)raw[6] << 8 | raw[7];
		}
		n++;
	}
	it->nrec = n;
	it->rec = rec;
}

#define CYR_IE 0x435   /* е */
#define CYR_IO 0x451   /* ё */

/* BytesDAWG._similar_items with the replaces {'е': 'ё'}; buf holds the key with the
 * replacements made so far (positions < pos) */
static void similar_items_rec(tn_arena *a, const dawg *d, uint32_t *buf, int n, int pos,
                              long index, int fields, ditemvec *res)
{
	ditemvec extra = {0};
	int start = pos;
	(void)start;
	while (pos < n) {
		uint32_t c = buf[pos];
		if (c == CYR_IE) {
			long next = d_follow_uchar(d, CYR_IO, index);
			if (next > 0) {
				buf[pos] = CYR_IO;
				similar_items_rec(a, d, buf, n, pos + 1, next, fields, &extra);
				buf[pos] = CYR_IE;
			}
		}
		index = d_follow_uchar(d, c, index);
		if (index <= 0)
			goto done;
		pos++;
	}
	{
		unsigned char sep = BYTES_SEP;
		long vi = d_follow_bytes(d, &sep, 1, index);
		if (vi > 0) {
			ditem it;
			uint32_t *k = aalloc(a, ((size_t)n + 1) * sizeof(uint32_t));
			memcpy(k, buf, (size_t)n * sizeof(uint32_t));
			it.key = (ustr){k, n};
			records_for_index(a, d, vi, fields, &it);
			div_push(a, res, it);
		}
	}
done:
	for (int i = 0; i < extra.n; i++)
		div_push(a, res, extra.v[i]);
}

static ditemvec similar_items(tn_arena *a, const dawg *d, ustr key, int fields)
{
	ditemvec res = {0};
	uint32_t *buf = aalloc(a, ((size_t)key.n + 1) * sizeof(uint32_t));
	if (key.n)
		memcpy(buf, key.s, (size_t)key.n * sizeof(uint32_t));
	similar_items_rec(a, d, buf, key.n, 0, ROOT, fields, &res);
	return res;
}

/* bool(DAWG._similar_keys(...)) for the words DAWG (BytesDAWG._has_value) */
static int similar_key_exists(const dawg *d, ustr key, int pos, long index)
{
	while (pos < key.n) {
		uint32_t c = key.s[pos];
		if (c == CYR_IE) {
			long next = d_follow_uchar(d, CYR_IO, index);
			if (next > 0 && similar_key_exists(d, key, pos + 1, next))
				return 1;
		}
		index = d_follow_uchar(d, c, index);
		if (index < 0)
			return 0;
		pos++;
	}
	unsigned char sep = BYTES_SEP;
	return d_follow_bytes(d, &sep, 1, index) > 0;
}

/* ------------------------------------------------------------ dictionary */

struct tagrec {
	tn_gset g;
	const char *str;
	uint16_t first;   /* first grammeme (OpencorporaTag._POS) */
	uint16_t eq;      /* tags with the same grammeme tuple compare equal */
	int8_t cat[TN_NCATEGORIES];
};

enum {
	SP_UNKN, SP_PNCT, SP_LATN, SP_INTG, SP_REAL, SP_ROMN, SP_ADVB, SP_INIT_FIRST,
	SP_INIT_PATR = SP_INIT_FIRST + 12, SP_COUNT = SP_INIT_PATR + 12
};

#define MAX_KNOWN_PREFIXES 160

struct tn_morph {
	void *map;
	size_t maplen;
	const unsigned char *strs;
	size_t nstrs;
	int ngram;
	const char **gname;
	tn_gset *incompat;
	int ntags;
	struct tagrec *tags;
	int nsuffix, npprefix;
	ustr *suffix, *pprefix;
	uint32_t npara;
	const uint32_t *para_start;
	const uint16_t *para_data;
	uint32_t npara_data;
	dawg words, pred[3], ptw;
	int max_suffix_len;
	const char *revision;
	uint16_t sp[SP_COUNT];

	tn_gset cat[TN_NCATEGORIES], pos_set, nonproductive, feature;
	int g_masc, g_femn, g_gen1, g_gent, g_loc1, g_loct, g_voy, g_vey;
	tn_gset adv_req;   /* ADJF, sing, datv */
	int rare_from[7], rare_to[7];

	int nknown;
	ustr known[MAX_KNOWN_PREFIXES];
	ustr particles[8];
	ustr initials;
	ustr po_hyphen;
	uint32_t *ubuf;   /* storage of the ustr above */
};

static const char *const KNOWN_PREFIXES[] = {
	"авиа", "авто", "аква", "анти", "анти-", "антропо", "архи", "арт", "арт-", "астро",
	"аудио", "аэро", "без", "бес", "био", "вело", "взаимо", "вне", "внутри", "видео",
	"вице-", "вперед", "впереди", "гекто", "гелио", "гео", "гетеро", "гига", "гигро",
	"гипер", "гипо", "гомо", "дву", "двух", "де", "дез", "дека", "деци", "дис", "до",
	"евро", "за", "зоо", "интер", "инфра", "квази", "квази-", "кило", "кино", "контр",
	"контр-", "космо", "космо-", "крипто", "лейб-", "лже", "лже-", "макро", "макси",
	"макси-", "мало", "меж", "медиа", "медиа-", "мега", "мета", "мета-", "метео", "метро",
	"микро", "милли", "мини", "мини-", "моно", "мото", "много", "мульти", "нано", "нарко",
	"не", "небез", "недо", "нейро", "нео", "низко", "обер-", "обще", "одно", "около",
	"орто", "палео", "пан", "пара", "пента", "пере", "пиро", "поли", "полу", "после",
	"пост", "пост-", "порно", "пра", "пра-", "пред", "пресс-", "противо", "противо-",
	"прото", "псевдо", "псевдо-", "радио", "разно", "ре", "ретро", "ретро-", "само",
	"санти", "сверх", "сверх-", "спец", "суб", "супер", "супер-", "супра", "теле", "тетра",
	"топ-", "транс", "транс-", "ультра", "унтер-", "штаб-", "экзо", "эко", "эндо",
	"эконом-", "экс", "экс-", "экстра", "экстра-", "электро", "энерго", "этно",
};
static const char *const PARTICLES[] = {"-то", "-ка", "-таки", "-де", "-тко", "-тка", "-с", "-ста"};
static const char INITIAL_LETTERS[] = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЭЮЯ";

static const char *const CAT_NAMES[TN_NCATEGORIES] = {
	"NOUN ADJF ADJS COMP VERB INFN PRTF PRTS GRND NUMR ADVB NPRO PRED PREP CONJ PRCL INTJ",
	"anim inan", "perf impf", "nomn gent datv accs ablt loct voct gen1 gen2 acc2 loc1 loc2",
	"masc femn neut", "incl excl", "indc impr", "sing plur", "1per 2per 3per",
	"pres past futr", "tran intr", "actv pssv",
};

static void gset_add(tn_gset *s, int i)
{
	if (i < 0)
		return;
	if (i < 64)
		s->lo |= 1ull << i;
	else
		s->hi |= 1ull << (i - 64);
}

static tn_gset gs_or(tn_gset a, tn_gset b)
{
	return (tn_gset){a.lo | b.lo, a.hi | b.hi};
}

static tn_gset gs_and(tn_gset a, tn_gset b)
{
	return (tn_gset){a.lo & b.lo, a.hi & b.hi};
}

static tn_gset gs_andnot(tn_gset a, tn_gset b)
{
	return (tn_gset){a.lo & ~b.lo, a.hi & ~b.hi};
}

static tn_gset gs_xor(tn_gset a, tn_gset b)
{
	return (tn_gset){a.lo ^ b.lo, a.hi ^ b.hi};
}

static int gs_empty(tn_gset a)
{
	return !a.lo && !a.hi;
}

static int gs_eq(tn_gset a, tn_gset b)
{
	return a.lo == b.lo && a.hi == b.hi;
}

static int gs_count(tn_gset a)
{
	return __builtin_popcountll(a.lo) + __builtin_popcountll(a.hi);
}

int tn_morph_grammeme(const tn_morph *m, const char *name)
{
	for (int i = 0; i < m->ngram; i++)
		if (strcmp(m->gname[i], name) == 0)
			return i;
	return -1;
}

static int grammeme_n(const tn_morph *m, const char *name, size_t len)
{
	for (int i = 0; i < m->ngram; i++)
		if (strlen(m->gname[i]) == len && memcmp(m->gname[i], name, len) == 0)
			return i;
	return -1;
}

const char *tn_morph_grammeme_name(const tn_morph *m, int i)
{
	return i >= 0 && i < m->ngram ? m->gname[i] : NULL;
}

int tn_morph_grammeme_count(const tn_morph *m)
{
	return m->ngram;
}

int tn_morph_gset(const tn_morph *m, const char *names, tn_gset *out)
{
	tn_gset s = {0, 0};
	const char *p = names;
	while (*p) {
		while (*p == ',' || *p == ' ')
			p++;
		if (!*p)
			break;
		const char *e = p;
		while (*e && *e != ',' && *e != ' ')
			e++;
		int g = grammeme_n(m, p, (size_t)(e - p));
		if (g < 0)
			return 0;
		gset_add(&s, g);
		p = e;
	}
	*out = s;
	return 1;
}

int tn_morph_tag_count(const tn_morph *m)
{
	return m->ntags;
}

const char *tn_morph_tag_str(const tn_morph *m, uint16_t tag)
{
	return tag < m->ntags ? m->tags[tag].str : "";
}

tn_gset tn_morph_tag_gset(const tn_morph *m, uint16_t tag)
{
	return tag < m->ntags ? m->tags[tag].g : (tn_gset){0, 0};
}

int tn_morph_tag_get(const tn_morph *m, uint16_t tag, enum tn_category c)
{
	if (tag >= m->ntags || c < 0 || c >= TN_NCATEGORIES)
		return -1;
	return m->tags[tag].cat[c];
}

tn_gset tn_morph_fix_rare_cases(const tn_morph *m, tn_gset g)
{
	tn_gset r = g;
	for (int i = 0; i < 7; i++) {
		if (m->rare_from[i] >= 0 && tn_gset_has(g, m->rare_from[i])) {
			tn_gset one = {0, 0};
			gset_add(&one, m->rare_from[i]);
			r = gs_andnot(r, one);
		}
	}
	for (int i = 0; i < 7; i++)
		if (m->rare_from[i] >= 0 && tn_gset_has(g, m->rare_from[i]))
			gset_add(&r, m->rare_to[i]);
	return r;
}

const char *tn_morph_revision(const tn_morph *m)
{
	return m->revision;
}

/* ------------------------------------------------------------ file */

static const char *str_at(const tn_morph *m, uint32_t off, int *clen)
{
	if (off > m->nstrs || m->nstrs - off < 5)
		return NULL;
	uint16_t bl, cl;
	memcpy(&bl, m->strs + off, 2);
	memcpy(&cl, m->strs + off + 2, 2);
	if (m->nstrs - off - 4 < (size_t)bl + 1 || m->strs[off + 4 + bl] != 0)
		return NULL;
	if (memchr(m->strs + off + 4, 0, bl))
		return NULL;
	if (clen)
		*clen = cl;
	return (const char *)m->strs + off + 4;
}

static int load_dawg(const unsigned char *p, uint64_t len, int completion, dawg *d)
{
	uint32_t n;
	if (len < 4)
		return 0;
	memcpy(&n, p, 4);
	if ((len - 4) / 4 < n || n == 0)
		return 0;
	d->units = (const uint32_t *)(p + 4);
	d->nunits = n;
	d->guide = NULL;
	d->nguide = 0;
	if (completion) {
		uint64_t off = 4 + (uint64_t)n * 4;
		uint32_t g;
		if (len - off < 4)
			return 0;
		memcpy(&g, p + off, 4);
		if ((len - off - 4) / 2 < g)
			return 0;
		d->guide = p + off + 4;
		d->nguide = g;
	}
	return 1;
}

static ustr ustr_from_static(tn_morph *m, const char *s, size_t *used, size_t cap)
{
	/* decode into m->ubuf */
	uint32_t *o = m->ubuf + *used;
	size_t n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p && *used + n < cap) {
		uint32_t c = *p;
		if (c < 0x80) {
			p++;
		} else if (c < 0xE0) {
			c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
			p += 2;
		} else {
			c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
			p += 3;
		}
		o[n++] = c;
	}
	*used += n;
	return (ustr){o, (int)n};
}

static ustr *load_strings(tn_morph *m, const unsigned char *p, uint64_t len, int *count)
{
	uint32_t n;
	if (len < 4)
		return NULL;
	memcpy(&n, p, 4);
	if ((len - 4) / 4 < n)
		return NULL;
	ustr *v = calloc(n ? n : 1, sizeof(ustr));
	if (!v)
		return NULL;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t off;
		int cl;
		memcpy(&off, p + 4 + 4 * i, 4);
		const char *s = str_at(m, off, &cl);
		if (!s) {
			free(v);
			return NULL;
		}
		size_t bl = strlen(s);
		uint32_t *o = malloc((bl + 1) * sizeof(uint32_t));
		if (!o) {
			free(v);
			return NULL;
		}
		/* strings were written by mkmorph.py from valid JSON: decode strictly */
		const unsigned char *q = (const unsigned char *)s;
		int k = 0;
		while (*q) {
			uint32_t c = *q;
			int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
			if (c >= 0x80)
				c &= 0x3F >> extra;
			q++;
			for (int j = 0; j < extra; j++) {
				if ((*q & 0xC0) != 0x80) {
					free(o);
					free(v);
					return NULL;
				}
				c = (c << 6) | (*q++ & 0x3F);
			}
			o[k++] = c;
		}
		v[i] = (ustr){o, k};
	}
	*count = (int)n;
	return v;
}

static void free_strings(ustr *v, int n)
{
	if (!v)
		return;
	for (int i = 0; i < n; i++)
		free((void *)v[i].s);
	free(v);
}

void tn_morph_close(tn_morph *m)
{
	if (!m)
		return;
	free_strings(m->suffix, m->nsuffix);
	free_strings(m->pprefix, m->npprefix);
	free(m->gname);
	free(m->incompat);
	free(m->tags);
	free(m->ubuf);
	if (m->map)
		munmap(m->map, m->maplen);
	free(m);
}

#define FAIL(msg)              \
	do {                   \
		if (err)       \
			*err = msg; \
		tn_morph_close(m); \
		return NULL;   \
	} while (0)

tn_morph *tn_morph_open(const char *path, const char **err)
{
	tn_morph *m = calloc(1, sizeof(tn_morph));
	if (!m) {
		if (err)
			*err = "out of memory";
		return NULL;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		FAIL("cannot open the dictionary");
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < 16 || (uint64_t)st.st_size > (1ull << 32)) {
		close(fd);
		FAIL("bad dictionary size");
	}
	m->maplen = (size_t)st.st_size;
	m->map = mmap(NULL, m->maplen, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m->map == MAP_FAILED) {
		m->map = NULL;
		FAIL("cannot map the dictionary");
	}
	const unsigned char *base = m->map;
	uint32_t ver, nsec;
	if (memcmp(base, "TNMORPH1", 8) != 0)
		FAIL("not a tn-morph dictionary");
	memcpy(&ver, base + 8, 4);
	memcpy(&nsec, base + 12, 4);
	if (ver != 1)
		FAIL("unsupported dictionary version");
	if (nsec > 64 || 16 + (uint64_t)nsec * 24 > m->maplen)
		FAIL("bad section table");
	const unsigned char *sp[13] = {0};
	uint64_t sl[13] = {0};
	for (uint32_t i = 0; i < nsec; i++) {
		uint32_t id;
		uint64_t off, len;
		memcpy(&id, base + 16 + 24 * i, 4);
		memcpy(&off, base + 16 + 24 * i + 8, 8);
		memcpy(&len, base + 16 + 24 * i + 16, 8);
		if (off > m->maplen || len > m->maplen - off || (off & 7))
			FAIL("bad section");
		if (id < 13) {
			sp[id] = base + off;
			sl[id] = len;
		}
	}
	for (int i = 0; i < 13; i++)
		if (!sp[i])
			FAIL("missing section");
	m->strs = sp[0];
	m->nstrs = sl[0];

	/* grammemes */
	uint32_t ng;
	memcpy(&ng, sp[1], 4);
	if (ng == 0 || ng > 128 || (sl[1] - 8) / 24 < ng)
		FAIL("bad grammeme table");
	m->ngram = (int)ng;
	m->gname = calloc(ng, sizeof(char *));
	m->incompat = calloc(ng, sizeof(tn_gset));
	if (!m->gname || !m->incompat)
		FAIL("out of memory");
	for (uint32_t i = 0; i < ng; i++) {
		const unsigned char *r = sp[1] + 8 + 24 * i;
		uint32_t so;
		memcpy(&so, r, 4);
		memcpy(&m->incompat[i].lo, r + 8, 8);
		memcpy(&m->incompat[i].hi, r + 16, 8);
		m->gname[i] = str_at(m, so, NULL);
		if (!m->gname[i])
			FAIL("bad grammeme name");
	}

	/* categories and the grammeme sets the units use */
	for (int c = 0; c < TN_NCATEGORIES; c++) {
		const char *p = CAT_NAMES[c];
		while (*p) {
			const char *e = strchr(p, ' ');
			if (!e)
				e = p + strlen(p);
			gset_add(&m->cat[c], grammeme_n(m, p, (size_t)(e - p)));
			p = *e ? e + 1 : e;
		}
	}
	m->pos_set = m->cat[TN_POS];
	if (!tn_morph_gset(m, "NUMR,NPRO,PRED,PREP,CONJ,PRCL,INTJ,Apro", &m->nonproductive) ||
	    !tn_morph_gset(m, "ADJF,sing,datv", &m->adv_req))
		FAIL("grammemes missing");
	m->feature = gs_or(gs_or(m->cat[TN_POS], m->cat[TN_NUMBER]),
	                   gs_or(m->cat[TN_CASE], gs_or(m->cat[TN_PERSON], m->cat[TN_TENSE])));
	m->g_masc = tn_morph_grammeme(m, "masc");
	m->g_femn = tn_morph_grammeme(m, "femn");
	m->g_gen1 = tn_morph_grammeme(m, "gen1");
	m->g_gent = tn_morph_grammeme(m, "gent");
	m->g_loc1 = tn_morph_grammeme(m, "loc1");
	m->g_loct = tn_morph_grammeme(m, "loct");
	m->g_voy = tn_morph_grammeme(m, "V-oy");
	m->g_vey = tn_morph_grammeme(m, "V-ey");
	if (m->g_masc < 0 || m->g_femn < 0 || m->g_gent < 0 || m->g_loct < 0)
		FAIL("grammemes missing");
	{
		static const char *const rf[7] = {"gen1", "gen2", "acc1", "acc2", "loc1", "loc2", "voct"};
		static const char *const rt[7] = {"gent", "gent", "accs", "accs", "loct", "loct", "nomn"};
		for (int i = 0; i < 7; i++) {
			m->rare_from[i] = tn_morph_grammeme(m, rf[i]);
			m->rare_to[i] = tn_morph_grammeme(m, rt[i]);
			if (m->rare_to[i] < 0)
				m->rare_from[i] = -1;
		}
	}

	/* tags */
	uint32_t nt;
	memcpy(&nt, sp[2], 4);
	if (nt == 0 || nt > 65535 || (sl[2] - 8) / 24 < nt)
		FAIL("bad tag table");
	m->ntags = (int)nt;
	m->tags = calloc(nt, sizeof(struct tagrec));
	if (!m->tags)
		FAIL("out of memory");
	for (uint32_t i = 0; i < nt; i++) {
		const unsigned char *r = sp[2] + 8 + 24 * i;
		struct tagrec *t = &m->tags[i];
		uint32_t so;
		memcpy(&t->g.lo, r, 8);
		memcpy(&t->g.hi, r + 8, 8);
		memcpy(&so, r + 16, 4);
		memcpy(&t->first, r + 20, 2);
		memcpy(&t->eq, r + 22, 2);
		t->str = str_at(m, so, NULL);
		if (!t->str || t->first >= ng || t->eq > i || (ng < 128 && (ng <= 64 ? (t->g.hi || (ng < 64 && (t->g.lo >> ng))) : (t->g.hi >> (ng - 64)))))
			FAIL("bad tag");
		for (int c = 0; c < TN_NCATEGORIES; c++) {
			tn_gset x = gs_and(t->g, m->cat[c]);
			t->cat[c] = -1;
			if (gs_empty(x))
				continue;
			/* several (e.g. 'anim' and 'inan' in Inmx tags): Python picks one in set
			 * order, which depends on the string hash seed; take the first in the tag */
			const char *p = t->str;
			while (*p) {
				const char *e = p;
				while (*e && *e != ',' && *e != ' ')
					e++;
				int g = grammeme_n(m, p, (size_t)(e - p));
				if (tn_gset_has(x, g)) {
					t->cat[c] = (int8_t)g;
					break;
				}
				p = *e ? e + 1 : e;
			}
		}
	}

	/* special tags */
	uint32_t nsp;
	memcpy(&nsp, sp[3], 4);
	if (nsp != SP_COUNT || sl[3] < 4 + 4 * (uint64_t)nsp)
		FAIL("bad special tags");
	for (int i = 0; i < SP_COUNT; i++) {
		uint32_t t;
		memcpy(&t, sp[3] + 4 + 4 * i, 4);
		if (t >= nt)
			FAIL("bad special tag");
		m->sp[i] = (uint16_t)t;
	}

	m->suffix = load_strings(m, sp[4], sl[4], &m->nsuffix);
	m->pprefix = load_strings(m, sp[5], sl[5], &m->npprefix);
	if (!m->suffix || !m->pprefix || m->npprefix < 1 || m->npprefix > 3)
		FAIL("bad affix tables");

	/* paradigms */
	uint32_t np;
	memcpy(&np, sp[6], 4);
	if ((sl[6] - 4) / 4 < (uint64_t)np + 1)
		FAIL("bad paradigms");
	m->npara = np;
	m->para_start = (const uint32_t *)(sp[6] + 4);
	m->para_data = (const uint16_t *)(sp[6] + 4 + 4 * ((uint64_t)np + 1));
	{
		uint64_t avail = (sl[6] - 4 - 4 * ((uint64_t)np + 1)) / 2;
		if (m->para_start[0] != 0 || m->para_start[np] > avail)
			FAIL("bad paradigms");
		m->npara_data = m->para_start[np];
		for (uint32_t p = 0; p < np; p++) {
			uint32_t a = m->para_start[p], b = m->para_start[p + 1];
			if (b < a || b > m->npara_data || (b - a) % 3)
				FAIL("bad paradigm");
			uint32_t n3 = (b - a) / 3;
			for (uint32_t k = 0; k < n3; k++)
				if (m->para_data[a + k] >= m->nsuffix || m->para_data[a + n3 + k] >= nt ||
				    m->para_data[a + 2 * n3 + k] >= m->npprefix)
					FAIL("bad paradigm entry");
		}
	}

	if (!load_dawg(sp[7], sl[7], 1, &m->words))
		FAIL("bad words DAWG");
	for (int i = 0; i < m->npprefix; i++)
		if (!load_dawg(sp[8 + i], sl[8 + i], 1, &m->pred[i]))
			FAIL("bad prediction DAWG");
	if (!load_dawg(sp[11], sl[11], 1, &m->ptw))
		FAIL("bad P(t|w) DAWG");

	uint32_t msl, rev;
	if (sl[12] < 8)
		FAIL("bad meta");
	memcpy(&msl, sp[12], 4);
	memcpy(&rev, sp[12] + 4, 4);
	if (msl < 1 || msl > 16)
		FAIL("bad max suffix length");
	m->max_suffix_len = (int)msl;
	m->revision = str_at(m, rev, NULL);
	if (!m->revision)
		m->revision = "";

	/* constant strings of the units */
	size_t cap = 4096, used = 0;
	m->ubuf = malloc(cap * sizeof(uint32_t));
	if (!m->ubuf)
		FAIL("out of memory");
	m->nknown = (int)N(KNOWN_PREFIXES);
	for (int i = 0; i < m->nknown; i++)
		m->known[i] = ustr_from_static(m, KNOWN_PREFIXES[i], &used, cap);
	for (size_t i = 0; i < N(PARTICLES); i++)
		m->particles[i] = ustr_from_static(m, PARTICLES[i], &used, cap);
	m->initials = ustr_from_static(m, INITIAL_LETTERS, &used, cap);
	m->po_hyphen = ustr_from_static(m, "по-", &used, cap);
	if (used >= cap)
		FAIL("internal: string buffer");
	return m;
}

/* ------------------------------------------------------- methods stacks */

enum mkind {
	M_DICT, M_ABBR_FIRST, M_ABBR_PATR, M_NUMBER, M_PNCT, M_ROMAN, M_LATIN, M_PARTICLE,
	M_HADVB, M_HYPHEN_FIXED, M_HYPHEN_BOTH, M_KPREFIX, M_UPREFIX, M_KSUFFIX, M_UNKN
};
/* DictionaryAnalyzer instances: the analyzer's own, UnknownPrefixAnalyzer.dict_analyzer,
 * KnownSuffixAnalyzer.fake_dict; methods compare by analyzer identity */
enum { DI_MAIN, DI_UPREFIX, DI_FAKE };

typedef struct meth {
	uint8_t kind, inst;
	uint16_t para, idx;
	ustr str;
	const struct tn_stack *l, *r;
} meth;

struct tn_stack {
	int n;
	const meth *m[];
};

typedef struct {
	ustr word, nf;
	uint16_t tag;
	double score;
	const tn_stack *stack;
} ip;

typedef struct {
	ip *v;
	int n, cap;
} ipvec;

static void ipv_push(tn_arena *a, ipvec *x, ip p)
{
	if (x->n == x->cap) {
		int cap = x->cap ? x->cap * 2 : 8;
		ip *nv = aalloc(a, (size_t)cap * sizeof(ip));
		if (x->n)
			memcpy(nv, x->v, (size_t)x->n * sizeof(ip));
		x->v = nv;
		x->cap = cap;
	}
	x->v[x->n++] = p;
}

static const meth *mk_meth(tn_arena *a, int kind, int inst, ustr str, int para, int idx,
                           const tn_stack *l, const tn_stack *r)
{
	meth *m = aalloc(a, sizeof(meth));
	m->kind = (uint8_t)kind;
	m->inst = (uint8_t)inst;
	m->str = str;
	m->para = (uint16_t)para;
	m->idx = (uint16_t)idx;
	m->l = l;
	m->r = r;
	return m;
}

static const tn_stack *stack1(tn_arena *a, const meth *m0)
{
	tn_stack *s = aalloc(a, sizeof(tn_stack) + sizeof(meth *));
	s->n = 1;
	s->m[0] = m0;
	return s;
}

static const tn_stack *stack_push(tn_arena *a, const tn_stack *s, const meth *m)
{
	tn_stack *o = aalloc(a, sizeof(tn_stack) + (size_t)(s->n + 1) * sizeof(meth *));
	o->n = s->n + 1;
	memcpy(o->m, s->m, (size_t)s->n * sizeof(meth *));
	o->m[s->n] = m;
	return o;
}

static const tn_stack *stack_pop(tn_arena *a, const tn_stack *s)
{
	tn_stack *o = aalloc(a, sizeof(tn_stack) + (size_t)(s->n > 0 ? s->n - 1 : 0) * sizeof(meth *));
	o->n = s->n > 0 ? s->n - 1 : 0;
	memcpy(o->m, s->m, (size_t)o->n * sizeof(meth *));
	return o;
}

/* (method0,) + stack[1:] */
static const tn_stack *stack_replace0(tn_arena *a, const tn_stack *s, const meth *m0)
{
	int n = s->n > 0 ? s->n : 1;
	tn_stack *o = aalloc(a, sizeof(tn_stack) + (size_t)n * sizeof(meth *));
	o->n = n;
	if (s->n > 1)
		memcpy(o->m + 1, s->m + 1, (size_t)(s->n - 1) * sizeof(meth *));
	o->m[0] = m0;
	return o;
}

static int stack_eq(const tn_stack *x, const tn_stack *y);

static int meth_eq(const meth *x, const meth *y)
{
	if (x == y)
		return 1;
	if (x->kind != y->kind || x->inst != y->inst)
		return 0;
	switch (x->kind) {
	case M_DICT:
		return x->para == y->para && x->idx == y->idx && u_eq(x->str, y->str);
	case M_HYPHEN_FIXED:
		return u_eq(x->str, y->str) && stack_eq(x->r, y->r);
	case M_HYPHEN_BOTH:
		return stack_eq(x->l, y->l) && stack_eq(x->r, y->r);
	default:
		return u_eq(x->str, y->str);
	}
}

static int stack_eq(const tn_stack *x, const tn_stack *y)
{
	if (x == y)
		return 1;
	if (x->n != y->n)
		return 0;
	for (int i = 0; i < x->n; i++)
		if (!meth_eq(x->m[i], y->m[i]))
			return 0;
	return 1;
}

/* ------------------------------------------------- seen parses (units/utils.py) */

typedef struct {
	ustr word;
	uint16_t eq;
	uint8_t kind;    /* 0 None, 1 para id, 2 methods stack */
	uint16_t para;
	const tn_stack *stack;
} seenkey;

typedef struct {
	seenkey *v;
	int n, cap;
	int nonempty;    /* also set by HyphenAdverbAnalyzer, which adds a whole parse */
} seenset;

static seenkey make_key(const tn_morph *m, ustr word, uint16_t tag, const tn_stack *s)
{
	seenkey k = {word, m->tags[tag].eq, 0, 0, NULL};
	if (s->n > 0) {
		const meth *m0 = s->m[0];
		if (m0->kind == M_DICT) {
			k.kind = 1;
			k.para = m0->para;
		} else if (m0->kind == M_HYPHEN_FIXED || m0->kind == M_HYPHEN_BOTH) {
			k.kind = 2;
			k.stack = m0->r;
		}
	}
	return k;
}

static int seen_has(const seenset *s, const seenkey *k)
{
	for (int i = 0; i < s->n; i++) {
		const seenkey *x = &s->v[i];
		if (x->eq != k->eq || x->kind != k->kind || !u_eq(x->word, k->word))
			continue;
		if (k->kind == 1 && x->para != k->para)
			continue;
		if (k->kind == 2 && !stack_eq(x->stack, k->stack))
			continue;
		return 1;
	}
	return 0;
}

static void seen_add(tn_arena *a, seenset *s, seenkey k)
{
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 16;
		seenkey *nv = aalloc(a, (size_t)cap * sizeof(seenkey));
		if (s->n)
			memcpy(nv, s->v, (size_t)s->n * sizeof(seenkey));
		s->v = nv;
		s->cap = cap;
	}
	s->v[s->n++] = k;
	s->nonempty = 1;
}

static void add_parse_if_not_seen(const tn_morph *m, tn_arena *a, ip p, ipvec *res, seenset *seen)
{
	seenkey k = make_key(m, p.word, p.tag, p.stack);
	if (seen_has(seen, &k))
		return;
	seen_add(a, seen, k);
	ipv_push(a, res, p);
}

/* ------------------------------------------------------------ paradigms */

static int para_len(const tn_morph *m, uint32_t para)
{
	return (int)((m->para_start[para + 1] - m->para_start[para]) / 3);
}

static int para_ok(const tn_morph *m, uint32_t para, uint32_t idx)
{
	return para < m->npara && idx < (uint32_t)para_len(m, para);
}

static uint16_t para_tag(const tn_morph *m, uint32_t para, uint32_t idx)
{
	const uint16_t *p = m->para_data + m->para_start[para];
	return p[para_len(m, para) + idx];
}

static ustr para_suffix(const tn_morph *m, uint32_t para, uint32_t idx)
{
	return m->suffix[m->para_data[m->para_start[para] + idx]];
}

static ustr para_prefix(const tn_morph *m, uint32_t para, uint32_t idx)
{
	const uint16_t *p = m->para_data + m->para_start[para];
	return m->pprefix[p[2 * para_len(m, para) + idx]];
}

/* Dictionary.build_stem */
static ustr build_stem(const tn_morph *m, uint32_t para, uint32_t idx, ustr word)
{
	ustr pre = para_prefix(m, para, idx), suf = para_suffix(m, para, idx);
	if (suf.n)
		return u_slice(word, pre.n, -suf.n);
	return u_slice(word, pre.n, word.n);
}

/* Dictionary.build_normal_form */
static ustr build_normal_form(const tn_morph *m, tn_arena *a, uint32_t para, uint32_t idx, ustr word)
{
	if (idx == 0)
		return word;
	ustr stem = build_stem(m, para, idx, word);
	return u_cat3(a, para_prefix(m, para, 0), stem, para_suffix(m, para, 0));
}

static int tag_productive(const tn_morph *m, uint16_t tag)
{
	return gs_empty(gs_and(m->tags[tag].g, m->nonproductive));
}

static int tag_unknown(const tn_morph *m, uint16_t tag)
{
	return !tn_gset_has(m->pos_set, m->tags[tag].first);
}

/* ------------------------------------------------------------ analyzer */

static ipvec morph_parse(const tn_morph *m, tn_arena *a, ustr word);

/* sum() of floats in Python >= 3.12 (Neumaier compensation) */
static double py_fsum(const double *x, int n)
{
	double f = 0.0, c = 0.0;
	for (int i = 0; i < n; i++) {
		double t = f + x[i];
		if (fabs(f) >= fabs(x[i]))
			c += (f - t) + x[i];
		else
			c += (x[i] - t) + f;
		f = t;
	}
	if (c != 0.0 && isfinite(c))
		f += c;
	return f;
}

/* stable sort by score, descending (sorted(..., reverse=True) keeps equal items in order) */
static void sort_by_score(tn_arena *a, ip *v, int n)
{
	if (n < 2)
		return;
	ip *tmp = aalloc(a, (size_t)n * sizeof(ip));
	for (int w = 1; w < n; w *= 2) {
		for (int lo = 0; lo < n; lo += 2 * w) {
			int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
			int i = lo, j = mid, k = lo;
			while (i < mid && j < hi)
				tmp[k++] = (v[j].score > v[i].score) ? v[j++] : v[i++];
			while (i < mid)
				tmp[k++] = v[i++];
			while (j < hi)
				tmp[k++] = v[j++];
		}
		memcpy(v, tmp, (size_t)n * sizeof(ip));
	}
}

/* ProbabilityEstimator.apply_to_parses */
static void estimate(const tn_morph *m, tn_arena *a, ustr word_lower, ipvec *res)
{
	if (res->n == 0)
		return;
	size_t wl = (size_t)word_lower.n * 4;
	unsigned char *key = aalloc(a, wl + 1 + 512);
	size_t k = 0;
	for (int i = 0; i < word_lower.n; i++)
		k += (size_t)utf8_put(key + k, word_lower.s[i]);
	key[k++] = ':';
	double *probs = aalloc(a, (size_t)res->n * sizeof(double));
	int any = 0;
	for (int i = 0; i < res->n; i++) {
		const char *ts = m->tags[res->v[i].tag].str;
		size_t tl = strlen(ts);
		long v = -1;
		if (tl <= 511) {
			memcpy(key + k, ts, tl);
			v = d_find(&m->ptw, key, (int)(k + tl));
		}
		probs[i] = v < 0 ? 0.0 : (double)v / 1000000.0;
		if (probs[i] != 0.0)
			any = 1;
	}
	if (!any) {
		double *sc = aalloc(a, (size_t)res->n * sizeof(double));
		for (int i = 0; i < res->n; i++)
			sc[i] = res->v[i].score;
		double kk = 1.0 / py_fsum(sc, res->n);
		for (int i = 0; i < res->n; i++)
			res->v[i].score = res->v[i].score * kk;
		return;
	}
	for (int i = 0; i < res->n; i++)
		res->v[i].score = probs[i];
	sort_by_score(a, res->v, res->n);
}

/* DictionaryAnalyzer.parse */
static void unit_dictionary(const tn_morph *m, tn_arena *a, ustr word_lower, int inst, ipvec *res)
{
	ditemvec items = similar_items(a, &m->words, word_lower, 2);
	for (int i = 0; i < items.n; i++) {
		const ditem *it = &items.v[i];
		for (int r = 0; r < it->nrec; r++) {
			uint32_t para = it->rec[2 * r], idx = it->rec[2 * r + 1];
			if (!para_ok(m, para, idx))
				continue;
			ip p;
			p.word = it->key;
			p.nf = build_normal_form(m, a, para, idx, it->key);
			p.tag = para_tag(m, para, idx);
			p.score = 1.0;
			p.stack = stack1(a, mk_meth(a, M_DICT, inst, it->key, (int)para, (int)idx, NULL, NULL));
			ipv_push(a, res, p);
		}
	}
}

/* _InitialsAnalyzer.parse */
static void unit_initials(const tn_morph *m, tn_arena *a, ustr word, ustr word_lower, int patr,
                          ipvec *res)
{
	if (word.n != 1 || u_index(m->initials, word.s[0]) < 0)
		return;
	const tn_stack *s = stack1(a, mk_meth(a, patr ? M_ABBR_PATR : M_ABBR_FIRST, 0, word, 0, 0, NULL, NULL));
	for (int i = 0; i < 12; i++) {
		ip p = {word_lower, word_lower, m->sp[(patr ? SP_INIT_PATR : SP_INIT_FIRST) + i], 0.1, s};
		ipv_push(a, res, p);
	}
}

static void shape_parse(tn_arena *a, ustr word, ustr word_lower, int kind, uint16_t tag, ipvec *res)
{
	ip p = {word_lower, word_lower, tag, 0.9, stack1(a, mk_meth(a, kind, 0, word, 0, 0, NULL, NULL))};
	ipv_push(a, res, p);
}

/* NumberAnalyzer.check_shape: 0, SP_INTG or SP_REAL */
static int number_shape(tn_arena *a, ustr word)
{
	if (word.n == 0)
		return 0;
	char *buf = aalloc(a, (size_t)word.n + 2);
	int n = to_ascii_number(word, buf, word.n + 2);
	if (py_int_ok(buf, n))
		return SP_INTG;
	uint32_t *w2 = aalloc(a, (size_t)word.n * sizeof(uint32_t));
	for (int i = 0; i < word.n; i++)
		w2[i] = word.s[i] == ',' ? '.' : word.s[i];
	n = to_ascii_number((ustr){w2, word.n}, buf, word.n + 2);
	if (py_float_ok(buf, n, aalloc(a, (size_t)n + 1)))
		return SP_REAL;
	return 0;
}

/* HyphenSeparatedParticleAnalyzer.parse */
static void unit_particle(const tn_morph *m, tn_arena *a, ustr wl, ipvec *res, seenset *seen)
{
	if (u_index(wl, '-') < 0)
		return;
	for (size_t i = 0; i < N(PARTICLES); i++) {
		ustr pt = m->particles[i];
		if (!u_endswith(wl, pt))
			continue;
		ustr base = u_slice(wl, 0, -pt.n);
		if (base.n == 0)
			continue;
		const meth *me = mk_meth(a, M_PARTICLE, 0, pt, 0, 0, NULL, NULL);
		ipvec ps = morph_parse(m, a, base);
		for (int j = 0; j < ps.n; j++) {
			ip p = {u_cat(a, ps.v[j].word, pt), u_cat(a, ps.v[j].nf, pt), ps.v[j].tag,
			        ps.v[j].score * 0.9, stack_push(a, ps.v[j].stack, me)};
			add_parse_if_not_seen(m, a, p, res, seen);
		}
		break;
	}
}

/* HyphenAdverbAnalyzer.parse; morph.tag() of the rest has the same set of tags as
 * morph.parse(), and only that set matters here */
static void unit_hyphen_adverb(const tn_morph *m, tn_arena *a, ustr word, ustr wl, ipvec *res,
                               seenset *seen)
{
	if (wl.n < 5 || !u_startswith(wl, m->po_hyphen))
		return;
	ipvec ps = morph_parse(m, a, u_slice(wl, 3, wl.n));
	int ok = 0;
	for (int i = 0; i < ps.n && !ok; i++)
		ok = tn_gset_subset(m->adv_req, m->tags[ps.v[i].tag].g);
	if (!ok)
		return;
	ip p = {wl, wl, m->sp[SP_ADVB], 0.7, stack1(a, mk_meth(a, M_HADVB, 0, word, 0, 0, NULL, NULL))};
	seen->nonempty = 1;
	ipv_push(a, res, p);
}

static int has_known_prefix(const tn_morph *m, ustr w)
{
	for (int i = 0; i < m->nknown; i++)
		if (u_startswith(w, m->known[i]))
			return 1;
	return 0;
}

static tn_gset similarity_features(const tn_morph *m, uint16_t tag)
{
	tn_gset g = gs_and(m->tags[tag].g, m->feature);
	tn_gset r = g;
	int from[2] = {m->g_gen1, m->g_loc1}, to[2] = {m->g_gent, m->g_loct};
	for (int i = 0; i < 2; i++) {
		if (from[i] >= 0 && tn_gset_has(r, from[i])) {
			tn_gset one = {0, 0};
			gset_add(&one, from[i]);
			r = gs_andnot(r, one);
			gset_add(&r, to[i]);
		}
	}
	return r;
}

/* HyphenatedWordsAnalyzer.parse; its parses are not added to seen_parses (the
 * seen_parses.update(seen_parses.copy()) there changes nothing) */
static void unit_hyphenated(const tn_morph *m, tn_arena *a, ustr wl, ipvec *res)
{
	int h = u_index(wl, '-');
	if (h < 0 || wl.s[0] == '-' || wl.s[wl.n - 1] == '-')
		return;
	int cnt = 0;
	for (int i = 0; i < wl.n; i++)
		cnt += wl.s[i] == '-';
	if (cnt != 1 || has_known_prefix(m, wl))
		return;
	ustr left = u_slice(wl, 0, h), right = u_slice(wl, h + 1, wl.n);
	ipvec lp = morph_parse(m, a, left);
	ipvec rp = morph_parse(m, a, right);
	/* _parse_as_variable_both */
	tn_gset *rf = aalloc(a, (size_t)(rp.n ? rp.n : 1) * sizeof(tn_gset));
	for (int j = 0; j < rp.n; j++)
		rf[j] = similarity_features(m, rp.v[j].tag);
	for (int i = 0; i < lp.n; i++) {
		if (tag_unknown(m, lp.v[i].tag))
			continue;
		tn_gset lf = similarity_features(m, lp.v[i].tag);
		for (int j = 0; j < rp.n; j++) {
			if (!gs_eq(lf, rf[j]))
				continue;
			const meth *me = mk_meth(a, M_HYPHEN_BOTH, 0, (ustr){NULL, 0}, 0, 0, lp.v[i].stack, rp.v[j].stack);
			ip p;
			p.word = u_cat3(a, lp.v[i].word, U_HYPHEN, rp.v[j].word);
			p.nf = u_cat3(a, lp.v[i].nf, U_HYPHEN, rp.v[j].nf);
			p.tag = lp.v[i].tag;
			p.score = lp.v[i].score * 0.75;
			p.stack = stack1(a, me);
			ipv_push(a, res, p);
		}
	}
	/* _parse_as_fixed_left */
	for (int j = 0; j < rp.n; j++) {
		if (tag_unknown(m, rp.v[j].tag))
			continue;
		const meth *me = mk_meth(a, M_HYPHEN_FIXED, 0, left, 0, 0, NULL, rp.v[j].stack);
		ip p;
		p.word = u_cat3(a, left, U_HYPHEN, rp.v[j].word);
		p.nf = u_cat3(a, left, U_HYPHEN, rp.v[j].nf);
		p.tag = rp.v[j].tag;
		p.score = rp.v[j].score * 0.75;
		p.stack = stack1(a, me);
		ipv_push(a, res, p);
	}
}

/* KnownPrefixAnalyzer.parse */
static void unit_known_prefix(const tn_morph *m, tn_arena *a, ustr wl, ipvec *res, seenset *seen)
{
	int idx[MAX_KNOWN_PREFIXES], n = 0;
	for (int i = 0; i < m->nknown; i++)
		if (u_startswith(wl, m->known[i]))
			idx[n++] = i;
	/* list.sort(key=len, reverse=True): stable */
	for (int i = 1; i < n; i++) {
		int v = idx[i], j = i;
		while (j > 0 && m->known[idx[j - 1]].n < m->known[v].n) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = v;
	}
	for (int i = 0; i < n; i++) {
		ustr pre = m->known[idx[i]];
		ustr rest = u_slice(wl, pre.n, wl.n);
		if (rest.n < 3)
			continue;
		const meth *me = mk_meth(a, M_KPREFIX, 0, pre, 0, 0, NULL, NULL);
		ipvec ps = morph_parse(m, a, rest);
		for (int j = 0; j < ps.n; j++) {
			if (!tag_productive(m, ps.v[j].tag))
				continue;
			ip p;
			p.word = u_cat(a, pre, ps.v[j].word);
			p.nf = u_cat(a, pre, ps.v[j].nf);
			p.tag = ps.v[j].tag;
			p.score = ps.v[j].score * 0.75;
			p.stack = stack_push(a, ps.v[j].stack, me);
			add_parse_if_not_seen(m, a, p, res, seen);
		}
	}
}

/* UnknownPrefixAnalyzer.parse */
static void unit_unknown_prefix(const tn_morph *m, tn_arena *a, ustr wl, ipvec *res, seenset *seen)
{
	int max_split = wl.n - 3 < 5 ? wl.n - 3 : 5;
	for (int i = 1; i <= max_split; i++) {
		ustr pre = u_slice(wl, 0, i), rest = u_slice(wl, i, wl.n);
		const meth *me = mk_meth(a, M_UPREFIX, 0, pre, 0, 0, NULL, NULL);
		ipvec ps = {0};
		unit_dictionary(m, a, rest, DI_UPREFIX, &ps);
		for (int j = 0; j < ps.n; j++) {
			if (!tag_productive(m, ps.v[j].tag))
				continue;
			ip p;
			p.word = u_cat(a, pre, ps.v[j].word);
			p.nf = u_cat(a, pre, ps.v[j].nf);
			p.tag = ps.v[j].tag;
			p.score = ps.v[j].score * 0.5;
			p.stack = stack_push(a, ps.v[j].stack, me);
			add_parse_if_not_seen(m, a, p, res, seen);
		}
	}
}

/* KnownSuffixAnalyzer.parse */
static void unit_known_suffix(const tn_morph *m, tn_arena *a, ustr word, ustr wl, ipvec *res,
                              seenset *seen)
{
	if (word.n < 4)
		return;
	typedef struct {
		ip p;
		uint32_t cnt;
		int prefix_id;
	} cand;
	cand *cv = NULL;
	int cn = 0, ccap = 0;
	unsigned long total[3] = {1, 1, 1};
	for (int pid = m->npprefix - 1; pid >= 0; pid--) {
		if (!u_startswith(wl, m->pprefix[pid]))
			continue;
		for (int i = m->max_suffix_len; i >= 1; i--) {
			ustr start = u_slice(wl, 0, -i), end = u_slice(wl, -i, wl.n);
			ditemvec items = similar_items(a, &m->pred[pid], end, 3);
			for (int t = 0; t < items.n; t++) {
				ustr fixed_word = u_cat(a, start, items.v[t].key);
				for (int r = 0; r < items.v[t].nrec; r++) {
					const uint32_t *rec = items.v[t].rec + 3 * r;
					uint32_t cnt = rec[0], para = rec[1], idx = rec[2];
					if (!para_ok(m, para, idx))
						continue;
					uint16_t tag = para_tag(m, para, idx);
					if (!tag_productive(m, tag))
						continue;
					total[pid] += cnt;
					seenkey k = {fixed_word, m->tags[tag].eq, 1, (uint16_t)para, NULL};
					if (seen_has(seen, &k))
						continue;
					seen_add(a, seen, k);
					const meth *m0 = mk_meth(a, M_DICT, DI_FAKE, fixed_word, (int)para, (int)idx, NULL, NULL);
					const meth *m1 = mk_meth(a, M_KSUFFIX, 0, items.v[t].key, 0, 0, NULL, NULL);
					cand c;
					c.p.word = fixed_word;
					c.p.nf = build_normal_form(m, a, para, idx, fixed_word);
					c.p.tag = tag;
					c.p.score = 0;
					c.p.stack = stack_push(a, stack1(a, m0), m1);
					c.cnt = cnt;
					c.prefix_id = pid;
					if (cn == ccap) {
						int nc = ccap ? ccap * 2 : 16;
						cand *nv = aalloc(a, (size_t)nc * sizeof(cand));
						if (cn)
							memcpy(nv, cv, (size_t)cn * sizeof(cand));
						cv = nv;
						ccap = nc;
					}
					cv[cn++] = c;
				}
			}
			if (total[pid] > 1)
				break;
		}
	}
	ipvec out = {0};
	for (int i = 0; i < cn; i++) {
		ip p = cv[i].p;
		p.score = (double)cv[i].cnt / (double)total[cv[i].prefix_id] * 0.5;
		ipv_push(a, &out, p);
	}
	sort_by_score(a, out.v, out.n);
	for (int i = 0; i < out.n; i++)
		ipv_push(a, res, out.v[i]);
}

/* MorphAnalyzer.parse */
static ipvec morph_parse(const tn_morph *m, tn_arena *a, ustr word)
{
	ipvec res = {0};
	seenset seen = {0};
	ustr wl = u_lower(a, word);

	/* [Dictionary, AbbreviatedFirstName, AbbreviatedPatronymic] */
	unit_dictionary(m, a, wl, DI_MAIN, &res);
	unit_initials(m, a, word, wl, 0, &res);
	unit_initials(m, a, word, wl, 1, &res);
	if (res.n)
		goto done;
	/* Number */
	int sh = number_shape(a, word);
	if (sh) {
		shape_parse(a, word, wl, M_NUMBER, m->sp[sh], &res);
		goto done;
	}
	/* Punctuation */
	if (is_punctuation(word)) {
		shape_parse(a, word, wl, M_PNCT, m->sp[SP_PNCT], &res);
		goto done;
	}
	/* [RomanNumber, Latin] */
	if (is_roman_number(word))
		shape_parse(a, word, wl, M_ROMAN, m->sp[SP_ROMN], &res);
	if (is_latin(word))
		shape_parse(a, word, wl, M_LATIN, m->sp[SP_LATN], &res);
	if (res.n)
		goto done;
	unit_particle(m, a, wl, &res, &seen);
	if (res.n)
		goto done;
	unit_hyphen_adverb(m, a, word, wl, &res, &seen);
	if (res.n)
		goto done;
	unit_hyphenated(m, a, wl, &res);
	if (res.n)
		goto done;
	unit_known_prefix(m, a, wl, &res, &seen);
	if (res.n)
		goto done;
	/* [UnknownPrefix, KnownSuffix] */
	unit_unknown_prefix(m, a, wl, &res, &seen);
	unit_known_suffix(m, a, word, wl, &res, &seen);
	if (res.n)
		goto done;
	/* Unkn */
	if (!seen.nonempty) {
		ip p = {wl, wl, m->sp[SP_UNKN], 1.0, stack1(a, mk_meth(a, M_UNKN, 0, word, 0, 0, NULL, NULL))};
		ipv_push(a, &res, p);
	}
done:
	estimate(m, a, wl, &res);
	return res;
}

/* ------------------------------------------------------------- lexemes */

static ipvec lexeme(const tn_morph *m, tn_arena *a, ip form);

static ip with_stack(ip f, const tn_stack *s)
{
	f.stack = s;
	return f;
}

static tn_gset replace_same(const tn_morph *m, tn_gset g)
{
	int from[3] = {m->g_voy, m->g_gen1, m->g_loc1}, to[3] = {m->g_vey, m->g_gent, m->g_loct};
	for (int i = 0; i < 3; i++) {
		if (from[i] >= 0 && to[i] >= 0 && tn_gset_has(g, from[i])) {
			tn_gset one = {0, 0};
			gset_add(&one, from[i]);
			g = gs_andnot(g, one);
			gset_add(&g, to[i]);
		}
	}
	return g;
}

static ipvec lexeme(const tn_morph *m, tn_arena *a, ip form)
{
	ipvec out = {0};
	const tn_stack *s = form.stack;
	if (!s || s->n == 0)
		return out;
	const meth *last = s->m[s->n - 1];
	switch (last->kind) {
	case M_DICT: {
		/* DictionaryAnalyzer.get_lexeme: the para info is in methods_stack[0] */
		const meth *m0 = s->m[0];
		if (m0->kind != M_DICT || !para_ok(m, m0->para, m0->idx))
			return out;
		ustr stem = build_stem(m, m0->para, m0->idx, form.word);
		int n = para_len(m, m0->para);
		for (int i = 0; i < n; i++) {
			ip p;
			p.word = u_cat3(a, para_prefix(m, m0->para, (uint32_t)i), stem, para_suffix(m, m0->para, (uint32_t)i));
			p.tag = para_tag(m, m0->para, (uint32_t)i);
			p.nf = form.nf;
			p.score = 1.0;
			p.stack = stack_replace0(a, s, mk_meth(a, M_DICT, m0->inst, p.word, m0->para, i, NULL, NULL));
			ipv_push(a, &out, p);
		}
		return out;
	}
	case M_KPREFIX:
	case M_UPREFIX: {
		ustr pre = last->str;
		ip f = form;
		f.word = u_slice(form.word, pre.n, form.word.n);
		f.nf = u_slice(form.nf, pre.n, form.nf.n);
		f.stack = stack_pop(a, s);
		ipvec lx = lexeme(m, a, f);
		for (int i = 0; i < lx.n; i++) {
			ip p = lx.v[i];
			p.word = u_cat(a, pre, p.word);
			p.nf = u_cat(a, pre, p.nf);
			p.stack = stack_push(a, p.stack, last);
			ipv_push(a, &out, p);
		}
		return out;
	}
	case M_PARTICLE: {
		ustr pt = last->str;
		ip f = form;
		f.word = u_slice(form.word, 0, -pt.n);
		f.nf = u_slice(form.nf, 0, -pt.n);
		f.stack = stack_pop(a, s);
		ipvec lx = lexeme(m, a, f);
		for (int i = 0; i < lx.n; i++) {
			ip p = lx.v[i];
			p.word = u_cat(a, p.word, pt);
			p.nf = u_cat(a, p.nf, pt);
			p.stack = stack_push(a, p.stack, last);
			ipv_push(a, &out, p);
		}
		return out;
	}
	case M_KSUFFIX: {
		ipvec lx = lexeme(m, a, with_stack(form, stack_pop(a, s)));
		for (int i = 0; i < lx.n; i++) {
			ip p = lx.v[i];
			p.stack = stack_push(a, p.stack, last);
			ipv_push(a, &out, p);
		}
		return out;
	}
	case M_ABBR_FIRST: {
		int base = tn_gset_has(m->tags[form.tag].g, m->g_masc) ? 0 : 6;
		for (int i = 0; i < 6; i++) {
			ip p = form;
			p.tag = m->sp[SP_INIT_FIRST + base + i];
			ipv_push(a, &out, p);
		}
		return out;
	}
	case M_ABBR_PATR:
		for (int i = 0; i < 12; i++) {
			ip p = form;
			p.tag = m->sp[SP_INIT_PATR + i];
			ipv_push(a, &out, p);
		}
		return out;
	case M_HYPHEN_FIXED: {
		if (s->n != 1)
			return out;   /* AssertionError in pymorphy3 */
		ustr prefix = u_cat(a, last->str, U_HYPHEN);
		ip rf = form;
		rf.stack = last->r;
		rf.word = u_slice(form.word, prefix.n, form.word.n);
		rf.nf = u_slice(form.nf, prefix.n, form.nf.n);
		ipvec lx = lexeme(m, a, rf);
		for (int i = 0; i < lx.n; i++) {
			ip p = lx.v[i];
			p.word = u_cat(a, prefix, p.word);
			p.nf = u_cat(a, prefix, p.nf);
			p.stack = stack1(a, mk_meth(a, M_HYPHEN_FIXED, 0, last->str, 0, 0, NULL, lx.v[i].stack));
			ipv_push(a, &out, p);
		}
		return out;
	}
	case M_HYPHEN_BOTH: {
		if (s->n != 1)
			return out;
		int wi = u_index(form.word, '-'), ni = u_index(form.nf, '-');
		if (wi < 0 || ni < 0)
			return out;   /* ValueError in pymorphy3 */
		ip lf = form, rf = form;
		lf.stack = last->l;
		lf.word = u_slice(form.word, 0, wi);
		lf.nf = u_slice(form.nf, 0, ni);
		rf.stack = last->r;
		rf.word = u_slice(form.word, wi + 1, form.word.n);
		rf.nf = u_slice(form.nf, ni + 1, form.nf.n);
		ipvec ll = lexeme(m, a, lf), rl = lexeme(m, a, rf);
		if (ll.n == 0)
			return out;
		for (int j = 0; j < rl.n; j++) {
			int best = -1, min = 1000000;
			tn_gset gr = replace_same(m, m->tags[rl.v[j].tag].g);
			for (int i = 0; i < ll.n; i++) {
				int d = gs_count(gs_xor(replace_same(m, m->tags[ll.v[i].tag].g), gr));
				if (d < min) {
					min = d;
					best = i;
				}
			}
			ip L = ll.v[best], R = rl.v[j];
			ip p;
			p.word = u_cat3(a, L.word, U_HYPHEN, R.word);
			p.tag = L.tag;
			p.nf = u_cat3(a, L.nf, U_HYPHEN, R.nf);
			p.score = (L.score + R.score) / 2;
			p.stack = stack1(a, mk_meth(a, M_HYPHEN_BOTH, 0, (ustr){NULL, 0}, 0, 0, L.stack, R.stack));
			ipv_push(a, &out, p);
		}
		return out;
	}
	default:   /* shapes, Unkn, HyphenAdverb */
		ipv_push(a, &out, form);
		return out;
	}
}

/* MorphAnalyzer._inflect */
static int inflect(const tn_morph *m, tn_arena *a, ip form, tn_gset req, ip *out)
{
	ipvec lx = lexeme(m, a, form);
	int any = 0;
	for (int i = 0; i < lx.n && !any; i++)
		any = tn_gset_subset(req, m->tags[lx.v[i].tag].g);
	if (!any)
		req = tn_morph_fix_rare_cases(m, req);
	tn_gset g = gs_or(m->tags[form.tag].g, req);
	for (int i = 0; i < m->ngram; i++)
		if (tn_gset_has(req, i))
			g = gs_andnot(g, m->incompat[i]);
	int best = -1;
	double bs = 0;
	for (int i = 0; i < lx.n; i++) {
		tn_gset t = m->tags[lx.v[i].tag].g;
		if (!tn_gset_subset(req, t))
			continue;
		double sim = (double)gs_count(gs_and(g, t)) - 0.1 * (double)gs_count(gs_xor(g, t));
		if (best < 0 || sim > bs) {
			best = i;
			bs = sim;
		}
	}
	if (best < 0)
		return 0;
	*out = lx.v[best];
	return 1;
}

/* ------------------------------------------------------------- API */

static void to_public(tn_arena *a, const ip *p, tn_parse *o)
{
	o->word = u_to_utf8(a, p->word);
	o->normal_form = u_to_utf8(a, p->nf);
	o->tag = p->tag;
	o->score = p->score;
	o->stack = p->stack;
}

static int from_public(const tn_morph *m, tn_arena *a, const tn_parse *f, ip *p)
{
	if (!f || !f->word || !f->normal_form || f->tag >= m->ntags || !f->stack)
		return 0;
	p->word = u_from_utf8(a, f->word, strlen(f->word));
	p->nf = u_from_utf8(a, f->normal_form, strlen(f->normal_form));
	p->tag = f->tag;
	p->score = f->score;
	p->stack = f->stack;
	return 1;
}

int tn_morph_parse(const tn_morph *m, tn_arena *a, const char *word, tn_parse **out)
{
	jmp_buf jb;
	*out = NULL;
	if (!m || !a || !word)
		return -1;
	if (setjmp(jb)) {
		a->oom = NULL;
		return -1;
	}
	a->oom = &jb;
	ustr w = u_from_utf8(a, word, strlen(word));
	ipvec r = morph_parse(m, a, w);
	tn_parse *o = aalloc(a, (size_t)(r.n ? r.n : 1) * sizeof(tn_parse));
	for (int i = 0; i < r.n; i++)
		to_public(a, &r.v[i], &o[i]);
	a->oom = NULL;
	*out = o;
	return r.n;
}

int tn_morph_lexeme(const tn_morph *m, tn_arena *a, const tn_parse *form, tn_parse **out)
{
	jmp_buf jb;
	*out = NULL;
	if (!m || !a)
		return -1;
	if (setjmp(jb)) {
		a->oom = NULL;
		return -1;
	}
	a->oom = &jb;
	ip f;
	int n = 0;
	if (from_public(m, a, form, &f)) {
		ipvec r = lexeme(m, a, f);
		tn_parse *o = aalloc(a, (size_t)(r.n ? r.n : 1) * sizeof(tn_parse));
		for (int i = 0; i < r.n; i++)
			to_public(a, &r.v[i], &o[i]);
		*out = o;
		n = r.n;
	}
	a->oom = NULL;
	return n;
}

int tn_morph_inflect(const tn_morph *m, tn_arena *a, const tn_parse *form, tn_gset required,
                     tn_parse *out)
{
	jmp_buf jb;
	volatile tn_gset req = required;   /* survives the longjmp */
	if (!m || !a || !out)
		return -1;
	if (setjmp(jb)) {
		a->oom = NULL;
		return -1;
	}
	a->oom = &jb;
	ip f, r;
	int ok = 0;
	if (from_public(m, a, form, &f) && inflect(m, a, f, (tn_gset){req.lo, req.hi}, &r)) {
		to_public(a, &r, out);
		ok = 1;
	}
	a->oom = NULL;
	return ok;
}

int tn_morph_word_is_known(const tn_morph *m, const char *word)
{
	tn_arena *a = tn_arena_new();
	if (!a)
		return 0;
	jmp_buf jb;
	volatile int r = 0;
	if (!setjmp(jb)) {
		a->oom = &jb;
		ustr w = u_lower(a, u_from_utf8(a, word, strlen(word)));
		r = similar_key_exists(&m->words, w, 0, ROOT);
	}
	tn_arena_free(a);
	return r;
}
