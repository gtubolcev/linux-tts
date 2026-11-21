/* references.py: page, section, law and clause references, ISBN, the number sign */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-num.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define RF(id) rn_pat(RN_REFERENCES_##id)
#define NUMLEN 4400

static int put_group(const rn_match *m, int g, rn_str *out)
{
	size_t n;
	const uint32_t *s = rn_group(m, g, &n);
	return rn_str_put(out, s, n);
}

static int put_whole(const rn_match *m, rn_str *out)
{
	return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
}

static int group(const rn_re *re, const char *name)
{
	return rn_re_group(re, name);
}

/* text[max(0, pos - w):pos] */
static const uint32_t *left_context(const rn_match *m, size_t w, size_t *n)
{
	size_t pos = m->start[0], a = pos > w ? pos - w : 0;
	*n = pos - a;
	return m->s + a;
}

/* not context.strip() or context.rstrip().endswith((".", "!", "?", "…")) */
static int sentence_start(const uint32_t *c, size_t n)
{
	while (n && rn_py_isspace(c[n - 1]))
		n--;
	return !n || c[n - 1] == '.' || c[n - 1] == '!' || c[n - 1] == '?' || c[n - 1] == U'…';
}

/* str(int(s)) of a [\d]+ group into buf (NUMLEN); 0 where int() raises */
static int int_of(const uint32_t *s, size_t n, char *buf)
{
	return n && n <= 4300 && rn_int_str(s, n, buf, NUMLEN) > 0;
}

/* num2words(int(s)) appended; 1 if int() or num2words raises (nothing appended) */
static int put_card(rn_str *out, const uint32_t *s, size_t n)
{
	char *buf = malloc(NUMLEN);
	if (!buf)
		return -1;
	int r = int_of(s, n, buf) ? rn_put_cardinal_a(out, buf) : 1;
	free(buf);
	return r;
}

/* _render_dotted_number: 0, 1 for None (out may hold a partial result: use a scratch) */
static int dotted(rn_str *out, const uint32_t *s, size_t n)
{
	for (size_t i = 0;; i++) {
		size_t j = i;
		while (j < n && s[j] != '.')
			j++;
		if (i && rn_puts(out, RN_U(" точка ")) < 0)
			return -1;
		int r = put_card(out, s + i, j - i);
		if (r)
			return r;
		if (j >= n)
			return 0;
		i = j;
	}
}

static int dotted_group(rn_str *out, const rn_match *m, int g)
{
	size_t n;
	const uint32_t *s = rn_group(m, g, &n);
	return dotted(out, s, n);
}

/* reference_case_from_left */
static const char *case_from_left(const rn_match *m)
{
	static const struct {
		const uint32_t *w;
		const char *c;
	} CASES[] = {
		{RN_U("согласно"), "datv"}, {RN_U("по"), "datv"}, {RN_U("к"), "datv"},
		{RN_U("ко"), "datv"},       {RN_U("в"), "loct"},  {RN_U("во"), "loct"},
		{RN_U("о"), "loct"},        {RN_U("об"), "loct"}, {RN_U("из"), "gent"},
		{RN_U("с"), "gent"},        {RN_U("со"), "gent"}, {RN_U("до"), "gent"},
		{RN_U("от"), "gent"},
	};
	size_t cn;
	const uint32_t *c = left_context(m, 32, &cn);
	const rn_re *re = RF(REFERENCE_PREPOSITION_PATTERN);
	rn_match pm;
	if (rn_search(re, c, cn, 0, &pm) != 1)
		return "nomn";
	size_t wn;
	const uint32_t *w = rn_group(&pm, group(re, "word"), &wn);
	rn_str f = {0};
	const char *res = "nomn";
	if (rn_py_casefold(w, wn, &f) == 0)
		for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++)
			if (rn_u32len(CASES[i].w) == f.n && !memcmp(CASES[i].w, f.s, f.n * 4))
				res = CASES[i].c;
	rn_str_free(&f);
	return res;
}

/* _CLAUSE_REFERENCE_FORMS */
static const uint32_t *clause_form(int sub, const char *c)
{
	static const uint32_t *F[2][4] = {
		{RN_U("пункт"), RN_U("пункта"), RN_U("пункту"), RN_U("пункте")},
		{RN_U("подпункт"), RN_U("подпункта"), RN_U("подпункту"), RN_U("подпункте")},
	};
	int k = !strcmp(c, "gent") ? 1 : !strcmp(c, "datv") ? 2 : !strcmp(c, "loct") ? 3 : 0;
	return F[sub][k];
}

static int casefold_eq(const uint32_t *s, size_t n, const uint32_t *w)
{
	rn_str f = {0};
	int eq = rn_py_casefold(s, n, &f) == 0 && f.n == rn_u32len(w) &&
	         (!f.n || !memcmp(f.s, w, f.n * 4));
	rn_str_free(&f);
	return eq;
}

/* out += noun.capitalize() for these all-lowercase words: first letter upper */
static int put_capital(rn_str *out, const uint32_t *w, size_t n)
{
	return rn_put_capitalized(out, w, n);
}

/* ---- clause references ---- */

static int clause_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = RF(CLAUSE_REFERENCE_PATTERN);
	rn_str num = {0};
	int r = dotted_group(&num, m, group(re, "number"));
	if (r) {
		rn_str_free(&num);
		return r < 0 ? -1 : put_whole(m, out);
	}
	size_t mn;
	const uint32_t *mk = rn_group(m, group(re, "marker"), &mn);
	int sub = !casefold_eq(mk, mn, RN_U("п"));
	const uint32_t *noun = clause_form(sub, case_from_left(m));
	size_t cn;
	const uint32_t *c = left_context(m, 24, &cn);
	r = (sentence_start(c, cn) ? put_capital(out, noun, rn_u32len(noun)) : rn_puts(out, noun)) < 0 ||
	                    rn_str_putc(out, ' ') < 0 || rn_str_put(out, num.s, num.n) < 0
	            ? -1
	            : 0;
	rn_str_free(&num);
	return r;
}

int rn_pre_normalize_clause_references(const rn_str *in, rn_str *out)
{
	return rn_sub(RF(CLAUSE_REFERENCE_PATTERN), in->s, in->n, clause_repl, NULL, 0, out) < 0 ? -1 : 0;
}

/* ---- numbered references ---- */

static const struct {
	const uint32_t *marker;
	const char *lemma;
} MARKERS[] = {
	{RN_U("ст"), "статья"},       {RN_U("п"), "пункт"},          {RN_U("подп"), "подпункт"},
	{RN_U("пп"), "подпункт"},     {RN_U("табл"), "таблица"},     {RN_U("прил"), "приложение"},
	{RN_U("рис"), "рисунок"},     {RN_U("стр"), "страница"},     {RN_U("с"), "страница"},
	{RN_U("p"), "страница"},
};

/* _see_directive_governs */
static int see_governs(const rn_match *m, int *err)
{
	size_t cn;
	const uint32_t *c = left_context(m, 96, &cn);
	rn_str ctx = {0}, next = {0};
	rn_match dm;
	int res = 0;
	if (rn_str_put(&ctx, c, cn) < 0)
		goto fail;
	for (int i = 0; i < 6; i++) {
		if (rn_search(RF(REFERENCE_DIRECTIVE_PATTERN), ctx.s, ctx.n, 0, &dm) == 1) {
			res = 1;
			goto out;
		}
		next.n = 0;
		if (rn_sub_text(RF(REFERENCE_ENUMERATION_ITEM_PATTERN), ctx.s, ctx.n, NULL, 0, 0, &next) < 0)
			goto fail;
		if (rn_str_eq(&next, &ctx))
			goto out;
		if (rn_str_copy(&ctx, &next) < 0)
			goto fail;
	}
	res = rn_search(RF(REFERENCE_DIRECTIVE_PATTERN), ctx.s, ctx.n, 0, &dm) == 1;
	goto out;
fail:
	*err = 1;
out:
	rn_str_free(&ctx);
	rn_str_free(&next);
	return res;
}

/* _reference_label into out: 0, 1 for None */
static int reference_label(rn_str *out, const rn_match *m, int gnum, int gidx)
{
	size_t nn, in;
	const uint32_t *num = rn_group(m, gnum, &nn);
	const uint32_t *idx = rn_group(m, gidx, &in);
	int dot = 0;
	for (size_t i = 0; i < nn; i++)
		dot |= num[i] == '.';
	if (!idx && !dot)
		return rn_str_put(out, num, nn);
	int r = dot ? dotted(out, num, nn) : put_card(out, num, nn);
	if (r)
		return r;
	if (idx && (rn_str_putc(out, ' ') < 0 || rn_str_put(out, idx, in) < 0))
		return -1;
	return 0;
}

int rn_pre_normalize_numbered_references(const rn_str *in, rn_str *out)
{
	const rn_re *re = RF(NUMBERED_REFERENCE_PATTERN);
	int gm = group(re, "marker"), gn = group(re, "number"), gi = group(re, "index");
	rn_iter it = rn_iter_init(re, in->s, in->n, 0);
	rn_match m;
	size_t position = 0;
	const char *chain_case = NULL;
	size_t chain_end = 0;
	int have_chain = 0, k, err = 0;
	rn_str piece = {0};
	while ((k = rn_iter_next(&it, &m)) == 1) {
		if (m.start[0] < position)
			continue;
		size_t mn;
		const uint32_t *mk = rn_group(&m, gm, &mn);
		const char *lemma = NULL;
		for (size_t i = 0; i < sizeof MARKERS / sizeof *MARKERS && !lemma; i++)
			if (casefold_eq(mk, mn, MARKERS[i].marker))
				lemma = MARKERS[i].lemma;
		if (!lemma)
			goto fail;
		int cont = 0;
		if (have_chain) {
			size_t a, b;
			rn_py_strip(in->s + chain_end, m.start[0] - chain_end, &a, &b);
			cont = a == b;
		}
		const char *gcase;
		if (cont)
			gcase = chain_case;
		else if (see_governs(&m, &err))
			gcase = "accs";
		else
			gcase = case_from_left(&m);
		if (err)
			goto fail;
		piece.n = 0;
		if (rn_noun_form(&piece, lemma, gcase) < 0)
			goto fail;
		if (rn_py_isupper(mk[0]) && strcmp(lemma, "страница")) {
			rn_str up = {0};
			if (piece.n && rn_py_upper(piece.s, 1, &up) == 0) {
				rn_str t = {0};
				if (rn_str_put(&t, up.s, up.n) < 0 || rn_str_put(&t, piece.s + 1, piece.n - 1) < 0)
					goto fail;
				rn_str_free(&piece);
				piece = t;
			}
			rn_str_free(&up);
		}
		if (rn_str_putc(&piece, ' ') < 0)
			goto fail;
		int r = reference_label(&piece, &m, gn, gi);
		if (r < 0)
			goto fail;
		if (r == 1)
			continue;
		if (rn_str_put(out, in->s + position, m.start[0] - position) < 0 ||
		    rn_str_put(out, piece.s, piece.n) < 0)
			goto fail;
		position = m.end[0];
		chain_case = gcase;
		chain_end = m.end[0];
		have_chain = 1;
	}
	if (k < 0)
		goto fail;
	rn_str_free(&piece);
	return rn_str_put(out, in->s + position, in->n - position);
fail:
	rn_str_free(&piece);
	return -1;
}

/* ---- page references ---- */

/* _page_number into buf: 0 ok, 1 None */
static int page_number(const uint32_t *s, size_t n, char *buf)
{
	int dec = n > 0;
	for (size_t i = 0; i < n; i++)
		dec &= rn_py_isdecimal(s[i]);
	if (dec)
		return int_of(s, n, buf) ? 0 : 1;
	long v = rn_roman_value(s, n);
	if (v < 0)
		return 1;
	snprintf(buf, NUMLEN, "%ld", v);
	return 0;
}

static int page_group(const rn_match *m, const rn_re *re, const char *name, char *buf)
{
	size_t n;
	const uint32_t *s = rn_group(m, group(re, name), &n);
	return s ? page_number(s, n, buf) : 1;
}

static int volume_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = RF(VOLUME_PAGE_REFERENCE_PATTERN);
	char *vb = malloc(3 * NUMLEN), *lb = vb + NUMLEN, *rb = lb + NUMLEN;
	rn_str t = {0};
	int r = -1;
	if (!vb)
		return -1;
	if (page_group(m, re, "volume", vb) || page_group(m, re, "left", lb)) {
		r = put_whole(m, out);
		goto done;
	}
	size_t rn;
	int right = rn_group(m, group(re, "right"), &rn) != NULL;
	if (right && page_group(m, re, "right", rb)) {
		r = put_whole(m, out);
		goto done;
	}
	if (rn_puts(&t, RN_U("Том ")) < 0 || rn_put_ordinal(&t, vb, N2W_NOMN, N2W_MASC, 0, 1) < 0 ||
	    rn_puts(&t, right ? RN_U(". Страницы ") : RN_U(". Страница ")) < 0)
		goto done;
	/* num2words raising would propagate in Python; keep the match then */
	int e = rn_put_cardinal_a(&t, lb);
	if (!e && right)
		e = rn_puts(&t, RN_U(" — ")) < 0 ? -1 : rn_put_cardinal_a(&t, rb);
	if (e < 0)
		goto done;
	r = e ? put_whole(m, out) : rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	free(vb);
	return r;
}

static int page_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = RF(PAGE_REFERENCE_PATTERN);
	size_t cn;
	const uint32_t *c = left_context(m, 24, &cn);
	rn_match pm;
	const rn_re *pre = RF(L354);
	int found = rn_search(pre, c, cn, 0, &pm);
	if (found < 0)
		return -1;
	char *lb = malloc(2 * NUMLEN), *rb = lb + NUMLEN;
	rn_str t = {0};
	int r = -1;
	if (!lb)
		return -1;
	size_t ln;
	int left = rn_group(m, group(re, "left"), &ln) != NULL;
	if (!found) {
		if (!left || !rn_py_islower(m->s[m->start[0]]) || page_group(m, re, "left", lb) ||
		    page_group(m, re, "right", rb)) {
			r = put_whole(m, out);
			goto done;
		}
		int e = rn_puts(&t, RN_U("страницы ")) < 0 ? -1 : rn_put_cardinal_a(&t, lb);
		if (!e)
			e = rn_puts(&t, RN_U(" — ")) < 0 ? -1 : rn_put_cardinal_a(&t, rb);
		if (e < 0)
			goto done;
		r = e ? put_whole(m, out) : rn_str_put(out, t.s, t.n);
		goto done;
	}
	size_t wn;
	const uint32_t *w = rn_group(&pm, group(pre, "word"), &wn);
	while (wn && w[wn - 1] == '.')   /* casefold().rstrip("."): the words are ASCII-free */
		wn--;
	int see = casefold_eq(w, wn, RN_U("см")) || casefold_eq(w, wn, RN_U("смотри"));
	if (left) {
		if (page_group(m, re, "left", lb) || page_group(m, re, "right", rb)) {
			r = put_whole(m, out);
			goto done;
		}
		if (rn_puts(&t, see ? RN_U("страницы с ") : RN_U("страницах с ")) < 0 ||
		    rn_put_ordinal(&t, lb, N2W_GENT, N2W_FEMN, 0, 1) < 0 || rn_puts(&t, RN_U(" по ")) < 0 ||
		    rn_put_ordinal(&t, rb, N2W_ACCS, N2W_FEMN, 0, 1) < 0)
			goto done;
	} else {
		if (page_group(m, re, "number", lb)) {
			r = put_whole(m, out);
			goto done;
		}
		if (rn_puts(&t, see ? RN_U("страницу ") : RN_U("странице ")) < 0 ||
		    rn_put_ordinal(&t, lb, see ? N2W_ACCS : N2W_LOCT, N2W_FEMN, 0, 1) < 0)
			goto done;
	}
	r = rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	free(lb);
	return r;
}

int rn_pre_normalize_page_references(const rn_str *in, rn_str *out)
{
	rn_str t = {0};
	int r = rn_sub(RF(VOLUME_PAGE_REFERENCE_PATTERN), in->s, in->n, volume_repl, NULL, 0, &t) < 0 ||
	                        rn_sub(RF(PAGE_REFERENCE_PATTERN), t.s ? t.s : RN_U(""), t.n, page_repl,
	                               NULL, 0, out) < 0
	                ? -1
	                : 0;
	rn_str_free(&t);
	return r;
}

/* ---- legal references ---- */

static int legal_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = RF(LEGAL_COMPLEX_REFERENCE_PATTERN);
	const char *pc = case_from_left(m);
	char *pb = malloc(2 * NUMLEN), *ab = pb + NUMLEN;
	size_t n;
	const uint32_t *s;
	int r = -1;
	if (!pb)
		return -1;
	s = rn_group(m, group(re, "point"), &n);
	if (!int_of(s, n, pb))
		goto done;
	s = rn_group(m, group(re, "article"), &n);
	if (!int_of(s, n, ab))
		goto done;
	enum n2w_case c = !strcmp(pc, "gent") ? N2W_GENT : !strcmp(pc, "datv") ? N2W_DATV
	                  : !strcmp(pc, "loct") ? N2W_LOCT : N2W_NOMN;
	if (rn_puts(out, clause_form(0, pc)) < 0 || rn_str_putc(out, ' ') < 0 ||
	    rn_put_ordinal(out, pb, c, N2W_MASC, 0, 1) < 0 || rn_puts(out, RN_U(" статьи ")) < 0 ||
	    rn_put_ordinal(out, ab, N2W_GENT, N2W_FEMN, 0, 1) < 0 ||
	    rn_puts(out, RN_U(" федерального закона номер ")) < 0 ||
	    put_group(m, group(re, "code"), out) < 0)
		goto done;
	r = 0;
done:
	free(pb);
	return r;
}

int rn_pre_normalize_complex_legal_references(const rn_str *in, rn_str *out)
{
	return rn_sub(RF(LEGAL_COMPLEX_REFERENCE_PATTERN), in->s, in->n, legal_repl, NULL, 0, out) < 0
	               ? -1
	               : 0;
}

static int admin_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = RF(ADMINISTRATIVE_LAW_REFERENCE_PATTERN);
	size_t pn, n;
	const uint32_t *prep = rn_group(m, group(re, "prep"), &pn);
	const uint32_t *s = rn_group(m, group(re, "part"), &n);
	char *pb = malloc(NUMLEN);
	rn_str t = {0};
	int r = -1;
	if (!pb)
		return -1;
	if (!int_of(s, n, pb)) {
		r = put_whole(m, out);
		goto done;
	}
	if (prep && (rn_str_put(&t, prep, pn) < 0 || rn_str_putc(&t, ' ') < 0))
		goto done;
	if (rn_puts(&t, prep ? RN_U("части ") : RN_U("частью ")) < 0 ||
	    rn_put_ordinal(&t, pb, prep ? N2W_GENT : N2W_ABLT, N2W_FEMN, 0, 1) < 0 ||
	    rn_puts(&t, RN_U(" статьи ")) < 0)
		goto done;
	int e = dotted_group(&t, m, group(re, "article"));
	if (e < 0)
		goto done;
	if (e) {
		r = put_whole(m, out);
		goto done;
	}
	r = rn_puts(&t, RN_U(" Кодекса Российской Федерации об административных правонарушениях")) < 0
	            ? -1
	            : rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	free(pb);
	return r;
}

int rn_pre_normalize_administrative_law_references(const rn_str *in, rn_str *out)
{
	return rn_sub(RF(ADMINISTRATIVE_LAW_REFERENCE_PATTERN), in->s, in->n, admin_repl, NULL, 0,
	              out) < 0
	               ? -1
	               : 0;
}

static int section_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = RF(SECTION_REFERENCE_PATTERN);
	rn_str first = {0}, second = {0}, noun = {0};
	int r = -1, e;
	size_t sn;
	int has2 = rn_group(m, group(re, "second"), &sn) != NULL;
	if ((e = dotted_group(&first, m, group(re, "first"))) < 0)
		goto done;
	if (!e && has2 && (e = dotted_group(&second, m, group(re, "second"))) < 0)
		goto done;
	if (e) {
		r = put_whole(m, out);
		goto done;
	}
	size_t cn;
	const uint32_t *c = left_context(m, 24, &cn);
	rn_match vm;
	const uint32_t *w = rn_search(RF(L480), c, cn, 0, &vm) == 1
	                            ? (has2 ? RN_U("параграфах") : RN_U("параграфе"))
	                            : RN_U("параграф");
	if ((sentence_start(c, cn) ? put_capital(out, w, rn_u32len(w)) : rn_puts(out, w)) < 0 ||
	    rn_str_putc(out, ' ') < 0 || rn_str_put(out, first.s, first.n) < 0)
		goto done;
	if (has2 && (rn_puts(out, RN_U(" и ")) < 0 || rn_str_put(out, second.s, second.n) < 0))
		goto done;
	r = 0;
done:
	rn_str_free(&first);
	rn_str_free(&second);
	rn_str_free(&noun);
	return r;
}

int rn_pre_normalize_section_references(const rn_str *in, rn_str *out)
{
	return rn_sub(RF(SECTION_REFERENCE_PATTERN), in->s, in->n, section_repl, NULL, 0, out) < 0 ? -1 : 0;
}

static int isbn_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *re = RF(ISBN_PATTERN);
	size_t n;
	const uint32_t *g = rn_group(m, group(re, "groups"), &n);
	rn_str t = {0};
	int r = -1;
	if (put_group(m, group(re, "prefix"), &t) < 0 || rn_str_putc(&t, ' ') < 0)
		goto done;
	for (size_t i = 0;; i++) {
		size_t j = i;
		while (j < n && g[j] != '-' && g[j] != U'–' && g[j] != U'—')
			j++;
		if (i && rn_puts(&t, RN_U(" — ")) < 0)
			goto done;
		int e = put_card(&t, g + i, j - i);
		if (e < 0)
			goto done;
		if (e) {
			r = put_whole(m, out);
			goto done;
		}
		if (j >= n)
			break;
		i = j;
	}
	r = rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	return r;
}

int rn_pre_normalize_isbn_identifiers(const rn_str *in, rn_str *out)
{
	return rn_sub(RF(ISBN_PATTERN), in->s, in->n, isbn_repl, NULL, 0, out) < 0 ? -1 : 0;
}

static int sign_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	static const struct {
		const uint32_t *p, *e;
	} F[] = {
		{RN_U("под"), RN_U("под номером ")}, {RN_U("с"), RN_U("с номером ")},
		{RN_U("со"), RN_U("с номером ")},    {RN_U("к"), RN_U("к номеру ")},
		{RN_U("ко"), RN_U("к номеру ")},     {RN_U("по"), RN_U("по номеру ")},
		{RN_U("у"), RN_U("у номера ")},      {RN_U("из"), RN_U("из номера ")},
		{RN_U("от"), RN_U("от номера ")},    {RN_U("до"), RN_U("до номера ")},
	};
	size_t n;
	const uint32_t *p = rn_group(m, group(RF(NUMBER_SIGN_WITH_PREPOSITION_PATTERN), "prep"), &n);
	for (size_t i = 0; i < sizeof F / sizeof *F; i++)
		if (casefold_eq(p, n, F[i].p)) {
			const uint32_t *e = F[i].e;
			if (rn_py_isupper(p[0]))
				return rn_py_upper(e, 1, out) < 0 ? -1 : rn_puts(out, e + 1);
			return rn_puts(out, e);
		}
	return -1;   /* KeyError in Python */
}

int rn_pre_normalize_number_signs(const rn_str *in, rn_str *out)
{
	rn_str t = {0};
	int r = rn_sub(RF(NUMBER_SIGN_WITH_PREPOSITION_PATTERN), in->s, in->n, sign_repl, NULL, 0, &t) <
	                        0 ||
	                        rn_sub_text(RF(NUMBER_SIGN_PATTERN), t.s ? t.s : RN_U(""), t.n,
	                                    RN_U("номер "), 6, 0, out) < 0
	                ? -1
	                : 0;
	rn_str_free(&t);
	return r;
}
