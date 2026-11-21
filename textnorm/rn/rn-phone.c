/* phonenumbers 9.0.43 for "+digits" input (phonenumberutil.py parse, is_valid_number,
 * format_number); metadata from gen-phonemeta.py. Metadata: libphonenumber, Apache-2.0. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-phone.h"
#include "rn-re.h"

struct pn_desc {
	int pat;
	uint32_t len, local;   /* bit n: length n possible */
};
struct pn_fmt {
	int pat;
	const char *fmt;   /* "\\1 \\2" */
	int ld;            /* last leading_digits_pattern, -1 */
};
enum { D_GENERAL, D_PREMIUM, D_TOLL_FREE, D_SHARED, D_VOIP, D_PERSONAL, D_PAGER, D_UAN,
       D_VOICEMAIL, D_FIXED, D_MOBILE, D_N };
struct pn_meta {
	int cc, leading, npp;
	const char *npt;
	int same_mobile_fixed;
	struct pn_desc d[D_N];
	int fmt0, nfmt;   /* intl_number_format, or number_format if that is empty */
};
struct pn_cc {
	int cc, meta0, nmeta;   /* main region first */
};

#include "rn-phonemeta.h"

#define NCC (sizeof pn_ccs / sizeof *pn_ccs)

static rn_re *cache[PN_NPATS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const rn_re *pat(int i)
{
	rn_re *re = __atomic_load_n(&cache[i], __ATOMIC_ACQUIRE);
	if (re)
		return re;
	pthread_mutex_lock(&lock);
	re = cache[i];
	if (!re) {
		char err[256];
		re = calloc(1, sizeof *re);
		if (re && rn_re_compile(re, pn_pat_src[i], err, sizeof err) < 0) {
			fprintf(stderr, "rn-phone: pattern %d: %s\n", i, err);
			free(re);
			re = NULL;
		}
		if (re)
			__atomic_store_n(&cache[i], re, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&lock);
	return re;
}

typedef struct {
	uint32_t s[32];
	size_t n;
} num;

/* 1/0, -1 error */
static int full(int p, const num *x)
{
	if (p < 0)
		return 0;
	const rn_re *re = pat(p);
	rn_match m;
	return re ? rn_fullmatch(re, x->s, x->n, 0, &m) : -1;
}

static int prefix(int p, const num *x, rn_match *m)
{
	const rn_re *re = pat(p);
	return re ? rn_match_at(re, x->s, x->n, 0, m) : -1;
}

/* _is_number_matching_desc */
static int matching_desc(const num *x, const struct pn_desc *d)
{
	if (d->len && !(d->len >> x->n & 1))
		return 0;
	return full(d->pat, x);
}

/* _number_type_helper(...) != UNKNOWN */
static int known_type(const num *x, const struct pn_meta *md)
{
	int r = matching_desc(x, &md->d[D_GENERAL]);
	if (r <= 0)
		return r;
	for (int i = D_PREMIUM; i <= D_FIXED; i++)
		if ((r = matching_desc(x, &md->d[i])) != 0)
			return r;
	if (md->same_mobile_fixed)
		return 0;
	return matching_desc(x, &md->d[D_MOBILE]);
}

/* _test_number_length(x, md) with the general desc: 1 unless TOO_SHORT,
 * IS_POSSIBLE_LOCAL_ONLY or INVALID_LENGTH */
static int length_ok(size_t n, const struct pn_meta *md)
{
	uint32_t lens = md->d[D_GENERAL].len, local = md->d[D_GENERAL].local;
	if (local >> n & 1)
		return 0;
	int lo = __builtin_ctz(lens), hi = 31 - __builtin_clz(lens);
	if ((int)n == lo)
		return 1;
	if ((int)n < lo)
		return 0;
	if ((int)n > hi)
		return 1;   /* TOO_LONG */
	return lens >> n & 1;
}

/* the template of re.sub: \N is group N */
static int expand(rn_str *out, const char *t, const rn_match *m)
{
	for (; *t; t++) {
		if (t[0] == '\\' && t[1] >= '0' && t[1] <= '9') {
			int g = *++t - '0';
			if ((uint32_t)g <= m->ngroups && m->start[g] != RN_UNSET &&
			    rn_str_put(out, m->s + m->start[g], m->end[g] - m->start[g]) < 0)
				return -1;
		} else if (rn_str_putc(out, (unsigned char)*t) < 0) {
			return -1;
		}
	}
	return 0;
}

static int tmpl_repl(const rn_match *m, rn_str *out, void *ctx)
{
	return expand(out, ctx, m);
}

/* does _maybe_strip_national_prefix_carrier_code change x and parse() keep the change?
 * 1 yes, 0 no, -1 error */
static int strips(const num *x, const struct pn_meta *md)
{
	rn_match m;
	int r;
	if (md->npp < 0 || !x->n)
		return 0;
	if ((r = prefix(md->npp, x, &m)) <= 0)
		return r;
	int viable = full(md->d[D_GENERAL].pat, x);
	if (viable < 0)
		return -1;
	num pot = {.n = 0};
	if (!md->npt || m.start[m.ngroups] == RN_UNSET) {
		if (!m.end[0])
			return 0;
		pot.n = x->n - m.end[0];
		memcpy(pot.s, x->s + m.end[0], pot.n * 4);
	} else {
		rn_str t = {0};
		if (rn_sub(pat(md->npp), x->s, x->n, tmpl_repl, (void *)md->npt, 1, &t) < 0) {
			rn_str_free(&t);
			return -1;
		}
		pot.n = t.n < 32 ? t.n : 31;
		if (t.n)
			memcpy(pot.s, t.s, pot.n * 4);
		int same = t.n == x->n && (!t.n || !memcmp(t.s, x->s, t.n * 4));
		rn_str_free(&t);
		if (same)
			return 0;
	}
	int nm = full(md->d[D_GENERAL].pat, &pot);
	if (nm < 0)
		return -1;
	if (viable && !nm)
		return 0;
	return length_ok(pot.n, md);
}

static const struct pn_cc *find_cc(int cc)
{
	size_t lo = 0, hi = NCC;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (pn_ccs[mid].cc < cc)
			lo = mid + 1;
		else if (pn_ccs[mid].cc > cc)
			hi = mid;
		else
			return &pn_ccs[mid];
	}
	return NULL;
}

int rn_phone_international(const char *digits, int *ccp, char *groups, size_t cap)
{
	size_t n = strlen(digits);
	/* _is_viable_phone_number needs 3 digits after "+"; TOO_SHORT_AFTER_IDD */
	if (n <= 2 || n > 17 + 3 || digits[0] == '0')
		return 0;
	const struct pn_cc *c = NULL;
	size_t ccl;
	int cc = 0;
	for (ccl = 1; ccl <= 3 && ccl <= n; ccl++) {
		cc = cc * 10 + (digits[ccl - 1] - '0');
		if ((c = find_cc(cc)))
			break;
	}
	if (!c)
		return 0;
	num x = {.n = n - ccl};
	if (x.n < 2 || x.n > 17)
		return 0;
	for (size_t i = 0; i < x.n; i++)
		x.s[i] = (unsigned char)digits[ccl + i];
	const struct pn_meta *main = &pn_metas[c->meta0];
	int r = strips(&x, main);
	if (r)
		return r < 0 ? -1 : 0;   /* the national prefix went: E164 differs */
	/* region_code_for_number, is_valid_number_for_region */
	const struct pn_meta *md = NULL;
	if (c->nmeta == 1) {
		md = main;
	} else {
		for (int i = 0; i < c->nmeta && !md; i++) {
			const struct pn_meta *t = &pn_metas[c->meta0 + i];
			rn_match m;
			if (t->leading >= 0)
				r = prefix(t->leading, &x, &m);
			else
				r = known_type(&x, t);
			if (r < 0)
				return -1;
			if (r)
				md = t;
		}
		if (!md)
			return 0;
	}
	if ((r = known_type(&x, md)) <= 0)
		return r;
	*ccp = cc;
	if (!groups)
		return 1;
	/* format_number(INTERNATIONAL): "+cc " + _format_nsn, then the digit runs */
	rn_str f = {0};
	const struct pn_fmt *fm = NULL;
	for (int i = 0; i < main->nfmt && !fm; i++) {
		const struct pn_fmt *t = &pn_fmts[main->fmt0 + i];
		rn_match m;
		if (t->ld >= 0 && (r = prefix(t->ld, &x, &m)) <= 0) {
			if (r < 0)
				return -1;
			continue;
		}
		if ((r = full(t->pat, &x)) < 0)
			return -1;
		if (r)
			fm = t;
	}
	if (fm) {
		if (rn_sub(pat(fm->pat), x.s, x.n, tmpl_repl, (void *)fm->fmt, 0, &f) < 0) {
			rn_str_free(&f);
			return -1;
		}
	} else if (rn_str_put(&f, x.s, x.n) < 0) {
		return -1;
	}
	size_t o = (size_t)snprintf(groups, cap, "%d", cc);
	int ind = 0;
	for (size_t i = 0; i < f.n && o + 2 < cap; i++) {
		int d = f.s[i] >= '0' && f.s[i] <= '9';
		if (d && !ind)
			groups[o++] = ' ';
		if (d)
			groups[o++] = (char)f.s[i];
		ind = d;
	}
	groups[o < cap ? o : cap - 1] = 0;
	rn_str_free(&f);
	return 1;
}
