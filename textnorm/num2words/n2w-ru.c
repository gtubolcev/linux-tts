/* n2w-ru: num2words 0.5.14 lang_RU in C. See n2w-ru.h.
 *
 * Function names follow Num2Word_RU (_int2word, __chunk_cardianl, __chunk_ordinal,
 * __chunk_ordinal_join, to_cardinal's float branch); the word tables are num2words' own,
 * expanded by gen-tables.py.
 */
#include "n2w-ru.h"

#include <string.h>

#include "n2w-ru-tables.h"

#define MAX_CHUNKS 11   /* THOUSANDS has 10**3 .. 10**30 */

typedef struct {
	char *out;
	size_t cap, len;
	int err;
} buf;

static void put(buf *b, const char *s)
{
	if (!s) {
		b->err = 1;   /* KeyError in num2words */
		return;
	}
	size_t n = strlen(s);
	if (b->len < b->cap) {
		size_t room = b->cap - b->len;
		memcpy(b->out + b->len, s, n < room ? n : room);
	}
	b->len += n;
}

/* ' '.join(words): a space before every word but the first */
static void word(buf *b, int *first, const char *s)
{
	if (!*first)
		put(b, " ");
	*first = 0;
	put(b, s);
}

typedef struct {
	int gcase, gender, plural, animate;
} kw;

static const kw KW_DEFAULT = {0, 0, 0, 1};

static kw kw_case(int c)
{
	kw k = KW_DEFAULT;
	k.gcase = c;
	return k;
}

static kw kw_gender(int g)
{
	kw k = KW_DEFAULT;
	k.gender = g;
	return k;
}

#define EL(tab, num, k) ((tab)[num][(k).gcase][(k).plural ? 3 : (k).gender][(k).animate ? 0 : 1])

/* Num2Word_RU.pluralize for 0 <= n */
static const char *pluralize(unsigned n, const char *const forms[3])
{
	if (n % 100 >= 11 && n % 100 <= 14)
		return forms[2];
	if (n % 10 == 1)
		return forms[0];
	if (n % 10 >= 2 && n % 10 <= 4)
		return forms[1];
	return forms[2];
}

static void chunk_cardinal(buf *b, int *first, int h, int t, int o, int i, kw k)
{
	if (h > 0)
		word(b, first, EL(N2W_HUNDREDS, h, k));
	if (t > 1)
		word(b, first, EL(N2W_TWENTIES, t, k));
	if (t == 1) {
		word(b, first, EL(N2W_TENS, o, k));
	} else if (o > 0) {
		kw f = k;
		if (i == 1)
			f.gender = N2W_FEMN;   /* thousands are feminine */
		word(b, first, EL(N2W_ONES, o, f));
	}
}

static void chunk_ordinal(buf *b, int *first, int h, int t, int o, int i, kw k)
{
	if (h > 0) {
		if (t == 0 && o == 0)
			word(b, first, EL(N2W_HUNDREDS_ORD, h, k));
		else
			word(b, first, EL(N2W_HUNDREDS, h, KW_DEFAULT));
	}
	if (t > 1) {
		if (o == 0)
			word(b, first, EL(N2W_TWENTIES_ORD, t, k));
		else
			word(b, first, EL(N2W_TWENTIES, t, KW_DEFAULT));
	}
	if (t == 1) {
		word(b, first, EL(N2W_TENS_ORD, o, k));
	} else if (o > 0) {
		if (i == 0)
			word(b, first, EL(N2W_ONES_ORD, o, k));
		else if (o == 1 && h == 0 && t == 0)
			;   /* тысячный, миллионный */
		else if (i == 1)
			word(b, first, EL(N2W_ONES, o, kw_gender(N2W_FEMN)));
		else
			word(b, first, EL(N2W_ONES, o, KW_DEFAULT));
	}
}

/* parts of one word ('двухсоттысячный'): no spaces */
static void chunk_ordinal_join(buf *b, int h, int t, int o, int i, kw k)
{
	if (h > 1)
		put(b, EL(N2W_HUNDREDS, h, kw_case(N2W_GENT)));
	else if (h == 1)
		put(b, EL(N2W_HUNDREDS, h, KW_DEFAULT));
	if (t == 9)
		put(b, EL(N2W_TWENTIES, t, KW_DEFAULT));
	else if (t > 1)
		put(b, EL(N2W_TWENTIES, t, kw_case(N2W_GENT)));
	if (t == 1) {
		put(b, EL(N2W_TENS, o, kw_case(N2W_GENT)));
	} else if (o > 0) {
		if (i == 0)
			put(b, EL(N2W_ONES_ORD, o, k));
		else if (o == 1) {
			if (t != 0 || h != 0)
				put(b, EL(N2W_ONES, 1, kw_gender(N2W_NEUT)));
		} else
			put(b, EL(N2W_ONES, o, kw_case(N2W_GENT)));
	}
}

/* _int2word for a non-negative number given as digits without leading zeros */
static void int2word(buf *b, const char *d, size_t len, int cardinal, kw k)
{
	if (len == 1 && d[0] == '0') {
		put(b, cardinal ? EL(N2W_ONES, 0, k) : EL(N2W_ONES_ORD, 0, k));
		return;
	}
	size_t nch = (len + 2) / 3;
	if (nch > MAX_CHUNKS) {
		b->err = 1;
		return;
	}
	int chunks[MAX_CHUNKS];
	size_t pos = 0, first_len = len % 3 ? len % 3 : 3;
	for (size_t j = 0; j < nch; j++) {
		size_t l = j == 0 ? first_len : 3;
		int v = 0;
		for (size_t q = 0; q < l; q++)
			v = v * 10 + (d[pos + q] - '0');
		chunks[j] = v;
		pos += l;
	}
	int ord_join = chunks[nch - 1] == 0;
	int rightest = 0;
	for (size_t j = 0; j < nch; j++)
		if (chunks[j])
			rightest = (int)(nch - 1 - j);
	int first = 1;
	for (size_t j = 0; j < nch; j++) {
		int x = chunks[j], i = (int)(nch - 1 - j);
		if (x == 0)
			continue;
		int o = x % 10, t = x / 10 % 10, h = x / 100;
		if (cardinal) {
			chunk_cardinal(b, &first, h, t, o, i, k);
			if (i > 0)
				word(b, &first, pluralize((unsigned)x, N2W_THOUSANDS[i][k.gcase]));
		} else if (!(ord_join && rightest == i)) {
			chunk_ordinal(b, &first, h, t, o, i, k);
			if (i > 0) {
				int tc = rightest == i ? k.gcase : N2W_NOMN;
				word(b, &first, pluralize((unsigned)x, N2W_THOUSANDS[i][tc]));
			}
		} else {
			if (!first)
				put(b, " ");
			first = 0;
			chunk_ordinal_join(b, h, t, o, i, k);
			if (i > 0)
				put(b, EL(N2W_THOUSANDS_ORD, i, k));
		}
	}
}

static int all_digits(const char *s, size_t n)
{
	if (n == 0)
		return 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return 0;
	return 1;
}

/* int(s) of a digit string: skip leading zeros */
static void strip_zeros(const char **s, size_t *n)
{
	while (*n > 1 && **s == '0') {
		(*s)++;
		(*n)--;
	}
}

static int finish(buf *b)
{
	if (b->cap)
		b->out[b->len < b->cap ? b->len : b->cap - 1] = 0;
	if (b->err || b->len > (size_t)0x7FFFFFFF)
		return -1;
	return (int)b->len;
}

static int check_opts(n2w_opts o)
{
	return o.gcase >= N2W_NOMN && o.gcase <= N2W_LOCT && o.gender >= N2W_MASC &&
	       o.gender <= N2W_NEUT;
}

static unsigned mod100(const char *d, size_t n)
{
	return (unsigned)((n >= 2 ? (d[n - 2] - '0') * 10 : 0) + (d[n - 1] - '0'));
}

int n2w_ru_cardinal(const char *number, n2w_opts o, char *out, size_t cap)
{
	buf b = {out, cap, 0, 0};
	if (!number || !check_opts(o))
		return -1;
	kw k = {o.gcase, o.gender, o.plural != 0, o.animate != 0};
	const char *s = number;
	int neg = *s == '-';
	if (neg)
		s++;
	const char *dot = strchr(s, '.');
	if (!dot) {
		size_t n = strlen(s);
		if (!all_digits(s, n))
			return -1;
		strip_zeros(&s, &n);
		if (neg && !(n == 1 && s[0] == '0')) {
			put(&b, "минус ");
		}
		int2word(&b, s, n, 1, k);
		return finish(&b);
	}
	/* float: '%s %s %s %s' % (int part, целая/целых, fraction, десятых/…) */
	const char *l = s, *r = dot + 1;
	size_t ln = (size_t)(dot - s), rn = strlen(r);
	if (!all_digits(l, ln) || !all_digits(r, rn))
		return -1;
	strip_zeros(&l, &ln);
	kw f = kw_gender(N2W_FEMN);
	int lzero = ln == 1 && l[0] == '0';
	if (neg && !lzero)
		put(&b, "минус ");
	int2word(&b, l, ln, 1, f);
	put(&b, " ");
	unsigned m = mod100(l, ln);
	if (neg && !lzero)
		m = (100 - m) % 100;   /* Python's % of a negative number */
	put(&b, pluralize(m, N2W_POINTWORD));
	put(&b, " ");
	const char *rs = r;
	size_t rsn = rn;
	strip_zeros(&rs, &rsn);
	int2word(&b, rs, rsn, 1, f);
	put(&b, " ");
	/* __decimal_bitness: 10 ** len(right) as an ordinal */
	char pow10[64];
	if (rn + 1 >= sizeof(pow10)) {
		b.err = 1;
		return finish(&b);
	}
	pow10[0] = '1';
	memset(pow10 + 1, '0', rn);
	if (r[rn - 1] == '1' && !(rn >= 2 && r[rn - 2] == '1'))
		int2word(&b, pow10, rn + 1, 0, f);
	else {
		kw g = kw_case(N2W_GENT);
		g.plural = 1;
		int2word(&b, pow10, rn + 1, 0, g);
	}
	return finish(&b);
}

int n2w_ru_ordinal(const char *number, n2w_opts o, char *out, size_t cap)
{
	buf b = {out, cap, 0, 0};
	if (!number || !check_opts(o))
		return -1;
	kw k = {o.gcase, o.gender, o.plural != 0, o.animate != 0};
	const char *s = number;
	int neg = *s == '-';
	if (neg)
		s++;
	size_t n = strlen(s);
	if (!all_digits(s, n))
		return -1;   /* verify_ordinal: floats raise */
	strip_zeros(&s, &n);
	if (neg && !(n == 1 && s[0] == '0'))
		return -1;   /* verify_ordinal: negative numbers raise */
	int2word(&b, s, n, 0, k);
	return finish(&b);
}
