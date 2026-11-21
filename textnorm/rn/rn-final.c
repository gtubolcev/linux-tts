/* normalizer.py run_finalize with the tts() options, and abbreviation_context.py's
 * ends_with_abbreviation / ends_with_plain_word it needs (caps.py
 * normalize_sentence_start_caps, preprocess_utils.py cleanup_malformed_punctuation) */
#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define P(name) rn_pat(RN_##name)
#define TAIL_LOOKBACK 16   /* _ABBREVIATION_TAIL_MAX_LOOKBACK */

static _Thread_local char *wbuf;
static _Thread_local size_t wcap;

static int all_tags(const cmorphy3_form *f, int k, const char *g)
{
	for (int i = 0; i < k; i++)
		if (!rn_tag_has(f[i].tag, g))
			return 0;
	return 1;
}

static int is_person_name(uint16_t tag)
{
	return rn_tag_has(tag, "Name") || rn_tag_has(tag, "Surn") || rn_tag_has(tag, "Patr");
}

/* is_abbreviation_fragment(token); -1 on error */
static int abbreviation_fragment(const uint32_t *t, size_t n)
{
	if (n == 1)
		return !(t[0] == 0x44F || t[0] == 0x42F);   /* token.lower() != "я" */
	if (!rn_py_str_islower(t, n))
		return 0;
	cmorphy3_arena *a = cmorphy3_arena_new();
	if (!a)
		return -1;
	cmorphy3_form *f;
	int k = rn_parse(a, t, n, &f), r = 0;
	if (k < 0) {
		r = -1;
	} else if (k > 0) {
		if (all_tags(f, k, "Abbr")) {
			r = 1;
		} else {
			int known = 0, names = 1;
			for (int i = 0; i < k; i++)
				if (cmorphy3_word_is_known(rn_morph, f[i].word)) {
					known = 1;
					names &= is_person_name(f[i].tag);
				}
			if (!known || names) {
				wbuf = rn_to_utf8(t, n, wbuf, &wcap);
				r = wbuf ? cmorphy3_has_prefix(rn_morph, wbuf) : -1;
			}
		}
	}
	cmorphy3_arena_free(a);
	return r;
}

/* _is_plain_word(token.lower()); -1 on error */
static int plain_word(const uint32_t *t, size_t n)
{
	cmorphy3_arena *a = cmorphy3_arena_new();
	if (!a)
		return -1;
	cmorphy3_form *f;
	int k = rn_parse(a, t, n, &f), r = 0;
	if (k < 0) {
		r = -1;
	} else if (k > 0) {
		int known = 0, abbr = 0;
		for (int i = 0; i < k; i++) {
			known |= cmorphy3_word_is_known(rn_morph, f[i].word);
			abbr |= rn_tag_has(f[i].tag, "Abbr");
		}
		r = known && !abbr;
	}
	cmorphy3_arena_free(a);
	return r;
}

/* _ABBREVIATION_TAIL_PATTERN.search(text, dot - 16, dot): the token, or 0 / -1 */
static int tail_token(const uint32_t *s, size_t dot, size_t *ta, size_t *tn)
{
	rn_match m;
	int r = rn_search(P(ABBREVIATION_CONTEXT_ABBREVIATION_TAIL_PATTERN), s, dot,
	                  dot > TAIL_LOOKBACK ? dot - TAIL_LOOKBACK : 0, &m);
	if (r == 1) {
		*ta = m.start[1];
		*tn = m.end[1] - m.start[1];
	}
	return r;
}

/* ends_with_abbreviation(text, dot); -1 on error */
static int ends_with_abbreviation(const uint32_t *s, size_t dot)
{
	size_t start = dot > TAIL_LOOKBACK ? dot - TAIL_LOOKBACK : 0, ta = 0, tn = 0;
	rn_match m;
	if (dot >= 2 && s[dot - 2] == '.') {
		int r = rn_search(P(ABBREVIATION_CONTEXT_DOTTED_LETTER_CHAIN_TAIL_PATTERN), s, dot,
		                  start, &m);
		if (r)
			return r;
	}
	int r = tail_token(s, dot, &ta, &tn);
	return r <= 0 ? r : abbreviation_fragment(s + ta, tn);
}

/* ends_with_plain_word(text, dot); -1 on error */
static int ends_with_plain_word(const uint32_t *s, size_t dot)
{
	size_t ta = 0, tn = 0;
	int r = tail_token(s, dot, &ta, &tn);
	return r <= 0 ? r : plain_word(s + ta, tn);
}

/* ---- caps.py normalize_sentence_start_caps ---- */

static int ellipsis_precedes(const uint32_t *s, size_t p)
{
	if (s[p] == 0x2026)
		return 1;
	if (s[p] != '.')
		return 0;
	return p > 0 && (s[p - 1] == '.' || s[p - 1] == 0x2026);
}

static const char *const ROUND_NOUNS[] = {"тысяча", "миллион", "миллиард", "триллион"};

/* _starts_with_numeral(text, pos); -1 on error */
static int starts_with_numeral(const uint32_t *s, size_t n, size_t pos)
{
	rn_match m;
	int r = rn_match_at(P(CAPS_WORD_AT_POSITION_PATTERN), s, n, pos, &m);
	if (r <= 0)
		return r;
	cmorphy3_arena *a = cmorphy3_arena_new();
	if (!a)
		return -1;
	cmorphy3_form *f;
	int k = rn_parse(a, s + m.start[0], m.end[0] - m.start[0], &f);
	r = k < 0 ? -1 : 0;
	for (int i = 0; i < k && !r; i++) {
		r = rn_tag_has(f[i].tag, "NUMR") || rn_tag_has(f[i].tag, "Anum");
		for (size_t j = 0; j < sizeof ROUND_NOUNS / sizeof *ROUND_NOUNS && !r; j++)
			r = strcmp(f[i].normal_form, ROUND_NOUNS[j]) == 0;
	}
	cmorphy3_arena_free(a);
	return r;
}

static int is_quote(uint32_t c)
{
	return c == '"' || c == '\'' || c == 0xBB || c == 0x201D;
}

static int sentence_start_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const uint32_t *s = m->s;
	size_t p = m->start[1], la = m->start[3];
	uint32_t punct = s[p], letter = s[la];
	int lower = rn_py_islower(letter), keep = 0, r;
	if (lower && (punct == '?' || punct == '!')) {
		for (size_t i = m->start[2]; i < m->end[2] && !keep; i++)
			keep = is_quote(s[i]);
		if (keep) {
			if ((r = starts_with_numeral(s, m->n, la)) < 0)
				return -1;
			keep = !r;
		}
	}
	if (!keep && lower && ellipsis_precedes(s, p)) {
		if ((r = starts_with_numeral(s, m->n, la)) < 0)
			return -1;
		keep = !r;
	}
	if (!keep && punct == '.') {
		if ((keep = ends_with_abbreviation(s, p)) < 0)
			return -1;
	}
	if (keep)
		return rn_str_put(out, s + m->start[0], m->end[0] - m->start[0]);
	if (rn_str_put(out, s + p, la - p) < 0)
		return -1;
	return rn_py_upper(&letter, 1, out);
}

/* ---- preprocess_utils.py cleanup_malformed_punctuation ---- */

static int dot_comma_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	int r = ends_with_plain_word(m->s, m->start[0]);
	if (r < 0)
		return -1;
	if (r)
		return rn_str_putc(out, '.');
	return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
}

static int step(int (*f)(const rn_str *, rn_str *), rn_str *t)
{
	rn_str u = {0};
	if (f(t, &u) < 0) {
		rn_str_free(&u);
		return -1;
	}
	if (!u.s && rn_str_put(&u, NULL, 0) < 0)
		return -1;
	rn_str_free(t);
	*t = u;
	return 0;
}

int rn_run_finalize(const rn_str *in, rn_str *out)
{
	rn_str t = {0};
	int r = -1;
	if (rn_str_put(&t, in->s, in->n) < 0 || (!t.s && rn_str_put(&t, NULL, 0) < 0))
		goto done;
	if (step(rn_pre_normalize_ascii_quote_pairs, &t) < 0 ||
	    rn_resub_tmpl(P(PREPROCESS_UTILS_ASCII_DASH_SPACE_PATTERN), &t, RN_U(" \x2014 ")) < 0 ||
	    step(rn_pre_normalize_punctuation_spacing, &t) < 0 ||
	    step(rn_pre_normalize_linebreaks, &t) < 0 ||
	    rn_resub(P(CAPS_SENTENCE_START_PATTERN), &t, sentence_start_repl, NULL) < 0 ||
	    rn_resub(P(PREPROCESS_UTILS_DOT_COMMA_PATTERN), &t, dot_comma_repl, NULL) < 0)
		goto done;
	r = rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	return r;
}
