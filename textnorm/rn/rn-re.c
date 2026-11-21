/* rn-re: Python re semantics on PCRE2. See rn-re.h. */
#include "rn-re.h"
#include "rn-unidata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Python's \w and \d (Unicode of the oracle's Python, not PCRE2's) in place of the
 * symbolic \p{L}\p{N}_ and \p{Nd} that gen-patterns.py emits, always inside a class */
static const struct {
	const char *sym, *ranges;
} UNI[] = {
	{"\\p{L}\\p{N}_", RN_UNI_WORD},
	{"\\p{Nd}", RN_UNI_DIGIT},
};

int rn_re_compile(rn_re *re, const char *pcre, char *err, size_t errlen)
{
	int ec;
	PCRE2_SIZE eo;
	size_t len = 0, max = strlen(pcre) + 1;
	for (const char *q = pcre; *q; q++)
		for (size_t u = 0; u < sizeof UNI / sizeof *UNI; u++)
			if (!strncmp(q, UNI[u].sym, strlen(UNI[u].sym)))
				max += strlen(UNI[u].ranges);
	/* the pattern is ASCII: widen it to 32-bit code units */
	uint32_t *p = malloc(max * sizeof *p);
	if (!p) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	for (const char *q = pcre; *q;) {
		size_t u = 0;
		for (; u < sizeof UNI / sizeof *UNI; u++)
			if (!strncmp(q, UNI[u].sym, strlen(UNI[u].sym)))
				break;
		if (u < sizeof UNI / sizeof *UNI) {
			for (const char *x = UNI[u].ranges; *x; x++)
				p[len++] = (unsigned char)*x;
			q += strlen(UNI[u].sym);
		} else {
			p[len++] = (unsigned char)*q++;
		}
	}
	p[len] = 0;
	re->code = pcre2_compile((PCRE2_SPTR)p, len, PCRE2_UTF | PCRE2_NO_UTF_CHECK, &ec, &eo, NULL);
	free(p);
	if (!re->code) {
		PCRE2_UCHAR msg[256];
		pcre2_get_error_message(ec, msg, 256);
		size_t k = 0;
		for (; msg[k] && k + 1 < errlen && k < 255; k++)
			err[k] = (char)msg[k];
		snprintf(err + k, errlen - k, " at %zu", (size_t)eo);
		return -1;
	}
	pcre2_pattern_info(re->code, PCRE2_INFO_CAPTURECOUNT, &re->ngroups);
	if (re->ngroups > RN_MAXGROUPS) {
		snprintf(err, errlen, "%u groups, at most %d", re->ngroups, RN_MAXGROUPS);
		pcre2_code_free(re->code);
		re->code = NULL;
		return -1;
	}
	pcre2_jit_compile(re->code, PCRE2_JIT_COMPLETE);   /* falls back to the interpreter */
	return 0;
}

void rn_re_free(rn_re *re)
{
	pcre2_code_free(re->code);
	re->code = NULL;
}

int rn_re_group(const rn_re *re, const char *name)
{
	uint32_t w[64];
	size_t i = 0;
	for (; name[i] && i < 63; i++)
		w[i] = (unsigned char)name[i];
	w[i] = 0;
	int r = pcre2_substring_number_from_name(re->code, (PCRE2_SPTR)w);
	return r < 0 ? -1 : r;
}

static _Thread_local pcre2_match_data *MD;
static _Thread_local pcre2_match_context *MC;
static _Thread_local pcre2_jit_stack *JS;
int rn_re_errcode;   /* PCRE2 error of the last failed match */

/* a repeated look-ahead group over a long letter run needs more than the default 32 KiB */
static int init_thread(void)
{
	MD = pcre2_match_data_create(RN_MAXGROUPS + 1, NULL);
	MC = pcre2_match_context_create(NULL);
	JS = pcre2_jit_stack_create(32 * 1024, 64 * 1024 * 1024, NULL);
	if (!MD || !MC || !JS)
		return -1;
	pcre2_jit_stack_assign(MC, NULL, JS);
	return 0;
}

static int run(const rn_re *re, const uint32_t *s, size_t n, size_t pos, uint32_t opts,
               rn_match *m)
{
	if (!MD && init_thread() < 0)
		return -1;
	if (pos > n)
		return 0;
	int rc = pcre2_match(re->code, (PCRE2_SPTR)s, n, pos, opts | PCRE2_NO_UTF_CHECK, MD, MC);
	if (rc == PCRE2_ERROR_NOMATCH)
		return 0;
	if (rc < 0) {
		rn_re_errcode = rc;
		return -1;
	}
	PCRE2_SIZE *ov = pcre2_get_ovector_pointer(MD);
	m->s = s;
	m->n = n;
	m->ngroups = re->ngroups;
	for (uint32_t g = 0; g <= re->ngroups; g++) {
		if ((int)g < rc && ov[2 * g] != PCRE2_UNSET) {
			m->start[g] = ov[2 * g];
			m->end[g] = ov[2 * g + 1];
		} else {
			m->start[g] = m->end[g] = RN_UNSET;
		}
	}
	return 1;
}

int rn_search(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m)
{
	return run(re, s, n, pos, 0, m);
}

int rn_match_at(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m)
{
	return run(re, s, n, pos, PCRE2_ANCHORED, m);
}

int rn_fullmatch(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m)
{
	return run(re, s, n, pos, PCRE2_ANCHORED | PCRE2_ENDANCHORED, m);
}

rn_iter rn_iter_init(const rn_re *re, const uint32_t *s, size_t n, size_t pos)
{
	rn_iter it = {re, s, n, pos, 0, 0};
	return it;
}

int rn_iter_next(rn_iter *it, rn_match *m)
{
	if (it->done)
		return 0;
	/* Python's scanner: after an empty match the next may not be empty at the same place */
	int r = run(it->re, it->s, it->n, it->pos, it->must_advance ? PCRE2_NOTEMPTY_ATSTART : 0, m);
	if (r != 1) {
		it->done = 1;
		return r;
	}
	it->must_advance = m->end[0] == m->start[0];
	it->pos = m->end[0];
	return 1;
}

int rn_str_put(rn_str *b, const uint32_t *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 64;
		while (cap < b->n + n + 1)
			cap *= 2;
		uint32_t *p = realloc(b->s, cap * sizeof *p);
		if (!p)
			return -1;
		b->s = p;
		b->cap = cap;
	}
	if (n)
		memcpy(b->s + b->n, s, n * sizeof *s);
	b->n += n;
	b->s[b->n] = 0;
	return 0;
}

int rn_str_putc(rn_str *b, uint32_t c)
{
	return rn_str_put(b, &c, 1);
}

int rn_str_utf8(rn_str *b, const char *utf8)
{
	const unsigned char *s = (const unsigned char *)utf8;
	while (*s) {
		uint32_t c = *s;
		int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
		if (len > 1)
			c &= 0x3F >> (len - 1);
		for (int j = 1; j < len && s[j]; j++)
			c = c << 6 | (s[j] & 0x3F);
		if (rn_str_putc(b, c) < 0)
			return -1;
		for (int j = 0; j < len && *s; j++)
			s++;
	}
	return 0;
}

void rn_str_free(rn_str *b)
{
	free(b->s);
	b->s = NULL;
	b->n = b->cap = 0;
}

long rn_sub(const rn_re *re, const uint32_t *s, size_t n, rn_repl_fn repl, void *ctx,
            size_t count, rn_str *out)
{
	rn_iter it = rn_iter_init(re, s, n, 0);
	rn_match m;
	size_t last = 0;
	long done = 0;
	int r = 0;
	while ((count == 0 || (size_t)done < count) && (r = rn_iter_next(&it, &m)) == 1) {
		if (rn_str_put(out, s + last, m.start[0] - last) < 0 || repl(&m, out, ctx) < 0)
			return -1;
		last = m.end[0];
		done++;
	}
	if ((count == 0 || (size_t)done < count) && r < 0)
		return -1;
	return rn_str_put(out, s + last, n - last) < 0 ? -1 : done;
}

struct fixed {
	const uint32_t *t;
	size_t n;
};

static int put_fixed(const rn_match *m, rn_str *out, void *ctx)
{
	(void)m;
	const struct fixed *f = ctx;
	return rn_str_put(out, f->t, f->n);
}

long rn_sub_text(const rn_re *re, const uint32_t *s, size_t n, const uint32_t *t, size_t tn,
                 size_t count, rn_str *out)
{
	struct fixed f = {t, tn};
	return rn_sub(re, s, n, put_fixed, &f, count, out);
}

static int put_tmpl(const rn_match *m, rn_str *out, void *ctx)
{
	for (const uint32_t *t = ctx; *t; t++) {
		if (*t >= 1 && *t <= 9) {
			uint32_t g = *t;
			if (g <= m->ngroups && m->start[g] != RN_UNSET &&
			    rn_str_put(out, m->s + m->start[g], m->end[g] - m->start[g]) < 0)
				return -1;
		} else if (rn_str_putc(out, *t) < 0) {
			return -1;
		}
	}
	return 0;
}

long rn_sub_tmpl(const rn_re *re, const uint32_t *s, size_t n, const uint32_t *tmpl,
                 size_t count, rn_str *out)
{
	return rn_sub(re, s, n, put_tmpl, (void *)tmpl, count, out);
}

long rn_resub(const rn_re *re, rn_str *cur, rn_repl_fn repl, void *ctx)
{
	rn_str next = {0};
	long r = rn_sub(re, cur->s ? cur->s : (const uint32_t *)U"", cur->n, repl, ctx, 0, &next);
	if (r < 0) {
		rn_str_free(&next);
		return -1;
	}
	if (r == 0) {   /* unchanged: keep cur */
		rn_str_free(&next);
		return 0;
	}
	rn_str_free(cur);
	*cur = next;
	return r;
}

long rn_resub_tmpl(const rn_re *re, rn_str *cur, const uint32_t *tmpl)
{
	return rn_resub(re, cur, put_tmpl, (void *)tmpl);
}

int rn_str_eq(const rn_str *a, const rn_str *b)
{
	return a->n == b->n && (!a->n || !memcmp(a->s, b->s, a->n * sizeof *a->s));
}

int rn_str_copy(rn_str *dst, const rn_str *src)
{
	dst->n = 0;
	return rn_str_put(dst, src->s, src->n);
}
