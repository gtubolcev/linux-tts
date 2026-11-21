/* dictionary.py DictionaryNormalizer for one .dic file. See rn-dic.h.
 *
 * Lines "source=target"; "#" comments. Runs of simple sources (no "$" start, none of
 * *\[](){}^$.|+?) become one alternation chunk ("regex_batch"), the others regex rules
 * where "*" is \S* (a leading or trailing "*" keeps the matched rest). Patterns are built in
 * the dialect of rn_re_compile (Python's \w through [\p{L}\p{N}_], Python's \s set).
 * Targets are taken literally: the bundled dictionaries have no backslashes, which Python
 * would read as template escapes. Batches of 1000+ keys ("simple" chunks in Python, longest
 * match per start) are run as one alternation too, which gives the same result. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-dic.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define W "[\\p{L}\\p{N}_]"
#define NS "[^\\t\\n\\x{b}\\f\\r\\x{1c}-\\x{20}\\x{85}\\x{a0}\\x{1680}\\x{2000}-\\x{200a}" \
           "\\x{2028}\\x{2029}\\x{202f}\\x{205f}\\x{3000}]"

typedef struct {
	rn_str key, val;   /* key lowered */
} kv;

typedef struct {
	rn_re re;
	rn_str target, hint;   /* hint casefolded; hint.n == 0: none */
	int pre, post;         /* group put before / after the target, 0 none */
} rule;

typedef struct {
	int batch;
	rn_re re;          /* batch */
	kv *kv;
	size_t nkv;
	rule *rules;       /* regex */
	size_t nrules;
} chunk;

struct rn_dic {
	chunk *c;
	size_t n;
};

/* growable char buffer for pattern text */
typedef struct {
	char *s;
	size_t n, cap;
	int oom;
} cbuf;

static void cb_put(cbuf *b, const char *s)
{
	size_t k = strlen(s);
	if (b->n + k + 1 > b->cap) {
		size_t cap = (b->n + k + 1) * 2;
		char *p = realloc(b->s, cap);
		if (!p) {
			b->oom = 1;
			return;
		}
		b->s = p;
		b->cap = cap;
	}
	memcpy(b->s + b->n, s, k + 1);
	b->n += k;
}

/* re.escape(s) */
static void cb_esc(cbuf *b, const uint32_t *s, size_t n)
{
	char t[16];
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (c < 0x80 && (rn_is_ascii_alpha(c) || rn_is_ascii_digit(c) || c == '_'))
			snprintf(t, sizeof t, "%c", (char)c);
		else if (c > 0x20 && c < 0x7F)
			snprintf(t, sizeof t, "\\%c", (char)c);
		else
			snprintf(t, sizeof t, "\\x{%x}", (unsigned)c);
		cb_put(b, t);
	}
}

static int compile(rn_re *re, cbuf *b, char *err, size_t errlen)
{
	if (b->oom) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	return rn_re_compile(re, b->s ? b->s : "", err, errlen);
}

void rn_dic_free(rn_dic *d)
{
	if (!d)
		return;
	for (size_t i = 0; i < d->n; i++) {
		chunk *c = &d->c[i];
		if (c->batch) {
			rn_re_free(&c->re);
			for (size_t k = 0; k < c->nkv; k++) {
				rn_str_free(&c->kv[k].key);
				rn_str_free(&c->kv[k].val);
			}
		}
		for (size_t k = 0; k < c->nrules; k++) {
			rn_re_free(&c->rules[k].re);
			rn_str_free(&c->rules[k].target);
			rn_str_free(&c->rules[k].hint);
		}
		free(c->kv);
		free(c->rules);
	}
	free(d->c);
	free(d);
}

static chunk *new_chunk(rn_dic *d, int batch)
{
	chunk *c = realloc(d->c, (d->n + 1) * sizeof *c);
	if (!c)
		return NULL;
	d->c = c;
	c = &d->c[d->n++];
	memset(c, 0, sizeof *c);
	c->batch = batch;
	return c;
}

/* flush(): the pending simple keys as a batch chunk */
static int flush_simple(rn_dic *d, kv *keys, size_t nk, char *err, size_t errlen)
{
	if (!nk)
		return 0;
	/* sorted(keys, key=len, reverse=True) is stable: sort an index copy */
	kv *s = malloc(nk * sizeof *s);
	size_t *idx = malloc(nk * sizeof *idx);
	if (!s || !idx) {
		free(s);
		free(idx);
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	for (size_t i = 0; i < nk; i++)
		idx[i] = i;
	/* insertion sort by length, descending, stable */
	for (size_t i = 1; i < nk; i++) {
		size_t v = idx[i], j = i;
		while (j && keys[idx[j - 1]].key.n < keys[v].key.n) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = v;
	}
	cbuf b = {0};
	cb_put(&b, "(?i)(?<!" W ")(?:");
	for (size_t i = 0; i < nk; i++) {
		s[i] = keys[idx[i]];
		if (i)
			cb_put(&b, "|");
		cb_esc(&b, s[i].key.s, s[i].key.n);
	}
	cb_put(&b, ")(?!" W ")");
	free(idx);
	chunk *c = new_chunk(d, 1);
	if (!c || compile(&c->re, &b, err, errlen) < 0) {
		if (!c)
			snprintf(err, errlen, "out of memory");
		free(b.s);
		for (size_t i = 0; i < nk; i++) {
			rn_str_free(&s[i].key);
			rn_str_free(&s[i].val);
		}
		free(s);
		if (c)
			d->n--;
		return -1;
	}
	free(b.s);
	c->kv = s;
	c->nkv = nk;
	return 0;
}

/* _dic_pattern_to_regex and the rule record; 1 added, 0 skipped (re.error), -1 error */
static int add_rule(rn_dic *d, int *open_regex, const uint32_t *src, size_t sn,
                    const uint32_t *tgt, size_t tn, char *err, size_t errlen)
{
	int anchor = sn && src[0] == '$';
	if (anchor) {
		src++;
		sn--;
	}
	if (!sn)
		return 0;
	cbuf b = {0};
	cb_put(&b, "(?i)");
	if (anchor)
		cb_put(&b, "^");
	int pre = 0, post = 0;
	size_t stars = 0;
	for (size_t i = 0; i < sn; i++)
		stars += src[i] == '*';
	if (src[0] == '*' && src[sn - 1] == '*' && stars == 2) {
		cb_esc(&b, src + 1, sn - 2);
	} else {
		/* parts = source.split("*") as [ps[i], pe[i]) */
		size_t *ps = malloc((sn + 1) * 2 * sizeof *ps), np = 0;
		if (!ps) {
			free(b.s);
			snprintf(err, errlen, "out of memory");
			return -1;
		}
		size_t *pe = ps + sn + 1;
		for (size_t i = 0;; i++) {
			size_t j = i;
			while (j < sn && src[j] != '*')
				j++;
			ps[np] = i;
			pe[np++] = j;
			if (j >= sn)
				break;
			i = j;
		}
		size_t lo = 0, hi = np;   /* parts[lo:hi] */
		int groups = 0;
		if (pe[0] == ps[0]) {
			cb_put(&b, "(" NS "*)");
			pre = ++groups;
			lo++;
		} else if (rn_py_isalnum(src[ps[0]])) {
			cb_put(&b, "(?<!" W ")");
		}
		int suffix = 0;
		if (hi > lo && pe[hi - 1] == ps[hi - 1]) {
			suffix = 1;
			hi--;
		}
		for (size_t i = lo; i < hi; i++) {
			if (i > lo)
				cb_put(&b, NS "*");
			cb_esc(&b, src + ps[i], pe[i] - ps[i]);
		}
		if (suffix) {
			cb_put(&b, "(" NS "*)");
			post = ++groups;
		} else if (hi > lo && pe[hi - 1] > ps[hi - 1] && rn_py_isalnum(src[pe[hi - 1] - 1])) {
			cb_put(&b, "(?!" W ")");
		}
		free(ps);
	}
	rule r = {0};
	r.pre = pre;
	r.post = post;
	char cerr[300];
	if (compile(&r.re, &b, cerr, sizeof cerr) < 0) {
		int oom = b.oom;
		free(b.s);
		if (oom) {
			snprintf(err, errlen, "out of memory");
			return -1;
		}
		return 0;
	}
	free(b.s);
	/* literal_hint = the longest non-empty part (the first of equals), casefolded */
	size_t hb = 0, hl = 0;
	for (size_t i = 0; i < sn;) {
		size_t j = i;
		while (j < sn && src[j] != '*')
			j++;
		if (j - i > hl) {
			hb = i;
			hl = j - i;
		}
		i = j + 1;
	}
	if ((hl && rn_py_casefold(src + hb, hl, &r.hint) < 0) || rn_str_put(&r.target, tgt, tn) < 0) {
		rn_re_free(&r.re);
		rn_str_free(&r.hint);
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	if (!r.target.s && rn_str_put(&r.target, RN_U(""), 0) < 0)
		return -1;
	chunk *c = *open_regex ? &d->c[d->n - 1] : new_chunk(d, 0);
	rule *nr = c ? realloc(c->rules, (c->nrules + 1) * sizeof *nr) : NULL;
	if (!nr) {
		rn_re_free(&r.re);
		rn_str_free(&r.hint);
		rn_str_free(&r.target);
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	*open_regex = 1;
	c->rules = nr;
	c->rules[c->nrules++] = r;
	return 1;
}

static int is_simple(const uint32_t *s, size_t n)
{
	if (n && s[0] == '$')
		return 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] < 0x80 && strchr("*\\[](){}^$.|+?", (int)s[i]))
			return 0;
	return 1;
}

rn_dic *rn_dic_parse(const char *utf8, size_t len, char *err, size_t errlen)
{
	rn_dic *d = calloc(1, sizeof *d);
	kv *keys = NULL;
	size_t nk = 0, capk = 0;
	int open_regex = 0;
	rn_str line = {0}, low = {0};
	if (!d)
		goto oom;
	char *buf = malloc(len + 1);
	if (!buf)
		goto oom;
	for (size_t p = 0; p < len;) {
		/* universal newlines, then rstrip("\n\r") */
		size_t q = p;
		while (q < len && utf8[q] != '\n' && utf8[q] != '\r')
			q++;
		memcpy(buf, utf8 + p, q - p);
		buf[q - p] = 0;
		p = q < len && utf8[q] == '\r' && q + 1 < len && utf8[q + 1] == '\n' ? q + 2 : q + 1;
		line.n = 0;
		if (rn_str_utf8(&line, buf) < 0) {
			free(buf);
			goto oom;
		}
		if (!line.n || line.s[0] == '#')
			continue;
		size_t eq = 0;
		while (eq < line.n && line.s[eq] != '=')
			eq++;
		if (eq == line.n || eq == 0)
			continue;
		const uint32_t *src = line.s, *tgt = line.s + eq + 1;
		size_t sn = eq, tn = line.n - eq - 1;
		if (is_simple(src, sn)) {
			if (open_regex)
				open_regex = 0;   /* flush(): the regex chunk is complete */
			low.n = 0;
			if (rn_py_lower(src, sn, &low) < 0) {
				free(buf);
				goto oom;
			}
			size_t k = 0;
			while (k < nk && !(keys[k].key.n == low.n &&
			                   !memcmp(keys[k].key.s, low.s, low.n * sizeof *low.s)))
				k++;
			if (k == nk) {
				if (nk == capk) {
					capk = capk ? capk * 2 : 64;
					kv *nkeys = realloc(keys, capk * sizeof *nkeys);
					if (!nkeys) {
						free(buf);
						goto oom;
					}
					keys = nkeys;
				}
				memset(&keys[nk], 0, sizeof keys[nk]);
				if (rn_str_put(&keys[nk].key, low.s, low.n) < 0) {
					free(buf);
					goto oom;
				}
				nk++;
			}
			keys[k].val.n = 0;
			if (rn_str_put(&keys[k].val, tgt, tn) < 0 ||
			    (!keys[k].val.s && rn_str_put(&keys[k].val, RN_U(""), 0) < 0)) {
				free(buf);
				goto oom;
			}
			continue;
		}
		if (nk) {
			if (flush_simple(d, keys, nk, err, errlen) < 0) {
				free(buf);
				goto fail_keys_moved;
			}
			nk = 0;
		}
		if (add_rule(d, &open_regex, src, sn, tgt, tn, err, errlen) < 0) {
			free(buf);
			goto fail;
		}
	}
	free(buf);
	if (nk && flush_simple(d, keys, nk, err, errlen) < 0)
		goto fail_keys_moved;
	free(keys);
	rn_str_free(&line);
	rn_str_free(&low);
	return d;
oom:
	snprintf(err, errlen, "out of memory");
fail:
	for (size_t k = 0; k < nk; k++) {
		rn_str_free(&keys[k].key);
		rn_str_free(&keys[k].val);
	}
fail_keys_moved:
	free(keys);
	rn_str_free(&line);
	rn_str_free(&low);
	rn_dic_free(d);
	return NULL;
}

static int batch_repl(const rn_match *m, rn_str *out, void *ctx)
{
	const chunk *c = ctx;
	size_t n = m->end[0] - m->start[0];
	const uint32_t *s = m->s + m->start[0];
	rn_str low = {0};
	if (rn_py_lower(s, n, &low) < 0)
		return -1;
	int r = -2;
	for (size_t k = 0; k < c->nkv && r == -2; k++)
		if (c->kv[k].key.n == low.n && !memcmp(c->kv[k].key.s, low.s, low.n * sizeof *low.s))
			r = rn_str_put(out, c->kv[k].val.s, c->kv[k].val.n);
	rn_str_free(&low);
	return r == -2 ? rn_str_put(out, s, n) : r;
}

static int rule_repl(const rn_match *m, rn_str *out, void *ctx)
{
	const rule *r = ctx;
	size_t n;
	const uint32_t *g;
	if (r->pre && (g = rn_group(m, r->pre, &n)) && rn_str_put(out, g, n) < 0)
		return -1;
	if (rn_str_put(out, r->target.s, r->target.n) < 0)
		return -1;
	if (r->post && (g = rn_group(m, r->post, &n)) && rn_str_put(out, g, n) < 0)
		return -1;
	return 0;
}

int rn_dic_apply(const rn_dic *d, rn_str *t)
{
	if (!t->s && rn_str_put(t, RN_U(""), 0) < 0)
		return -1;
	rn_str fold = {0};
	int err = 0;
	for (size_t i = 0; i < d->n && !err; i++) {
		const chunk *c = &d->c[i];
		if (c->batch) {
			err = rn_resub(&c->re, t, batch_repl, (void *)c) < 0;
			continue;
		}
		fold.n = 0;
		err = rn_py_casefold(t->s, t->n, &fold) < 0;
		for (size_t k = 0; k < c->nrules && !err; k++) {
			const rule *r = &c->rules[k];
			if (r->hint.n && rn_py_find(fold.s, fold.n, r->hint.s, r->hint.n, 0) < 0)
				continue;
			long nsub = rn_resub(&r->re, t, rule_repl, (void *)r);
			if (nsub < 0)
				err = 1;
			else if (nsub > 0) {
				/* a substitution may give the same text; refolding then is harmless */
				fold.n = 0;
				err = rn_py_casefold(t->s, t->n, &fold) < 0;
			}
		}
	}
	rn_str_free(&fold);
	return err ? -1 : 0;
}
