#include "rn-morph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cmorphy3 *rn_morph;

int rn_morph_open(const char *path, const char **err)
{
	rn_morph = cmorphy3_open(path, err);
	return rn_morph ? 0 : -1;
}

void rn_morph_close(void)
{
	cmorphy3_close(rn_morph);
	rn_morph = NULL;
}

char *rn_to_utf8(const uint32_t *s, size_t n, char *buf, size_t *cap)
{
	if (!buf || *cap < 4 * n + 1) {
		size_t c = 4 * n + 1 < 64 ? 64 : 4 * n + 1;
		char *p = realloc(buf, c);
		if (!p) {
			free(buf);
			*cap = 0;
			return NULL;
		}
		buf = p;
		*cap = c;
	}
	char *o = buf;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (c < 0x80) {
			*o++ = (char)c;
		} else if (c < 0x800) {
			*o++ = (char)(0xC0 | c >> 6);
			*o++ = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			*o++ = (char)(0xE0 | c >> 12);
			*o++ = (char)(0x80 | (c >> 6 & 0x3F));
			*o++ = (char)(0x80 | (c & 0x3F));
		} else {
			*o++ = (char)(0xF0 | c >> 18);
			*o++ = (char)(0x80 | (c >> 12 & 0x3F));
			*o++ = (char)(0x80 | (c >> 6 & 0x3F));
			*o++ = (char)(0x80 | (c & 0x3F));
		}
	}
	*o = 0;
	return buf;
}

static _Thread_local char *WBUF;
static _Thread_local size_t WCAP;

int rn_word_is_known(const uint32_t *s, size_t n)
{
	WBUF = rn_to_utf8(s, n, WBUF, &WCAP);
	return WBUF && cmorphy3_word_is_known(rn_morph, WBUF);
}

/* _reference_noun_form of references.py: the first parse of lemma with NOUN and nomn,
 * inflected to {case, sing}; the lemma when there is none. Cached per (lemma, case). */
int rn_noun_form(rn_str *out, const char *lemma, const char *gcase)
{
	static _Thread_local struct {
		const char *lemma, *gcase;
		char word[64];
	} cache[64];
	static _Thread_local int ncache;
	for (int i = 0; i < ncache; i++)
		if (cache[i].lemma == lemma && !strcmp(cache[i].gcase, gcase))
			return rn_str_utf8(out, cache[i].word);
	const char *word = lemma;
	cmorphy3_arena *a = cmorphy3_arena_new();
	if (!a)
		return -1;
	cmorphy3_form *forms, inf;
	int n = cmorphy3_parse(rn_morph, a, lemma, &forms);
	int noun = cmorphy3_grammeme(rn_morph, "NOUN"), nomn = cmorphy3_grammeme(rn_morph, "nomn");
	char names[32];
	snprintf(names, sizeof names, "%s,sing", gcase);
	cmorphy3_gset req;
	for (int i = 0; i < n; i++) {
		if (!cmorphy3_gset_has(cmorphy3_tag_gset(rn_morph, forms[i].tag), noun) ||
		    cmorphy3_tag_get(rn_morph, forms[i].tag, CMORPHY3_CASE) != nomn)
			continue;
		if (cmorphy3_grammemes(rn_morph, names, &req) &&
		    cmorphy3_inflect(rn_morph, a, &forms[i], req, &inf) == 1)
			word = inf.word;
		break;
	}
	int r = rn_str_utf8(out, word);
	if (ncache < 64 && strlen(word) < sizeof cache[0].word) {
		cache[ncache].lemma = lemma;
		cache[ncache].gcase = gcase;
		strcpy(cache[ncache].word, word);
		ncache++;
	}
	cmorphy3_arena_free(a);
	return r;
}

int rn_parse(cmorphy3_arena *a, const uint32_t *s, size_t n, cmorphy3_form **out)
{
	char small[256], *b = small;
	size_t cap = sizeof small;
	if (4 * n + 1 > cap) {
		b = NULL;
		cap = 0;
	}
	char *u = rn_to_utf8(s, n, b, &cap);
	if (!u)
		return -1;
	int r = cmorphy3_parse(rn_morph, a, u, out);
	if (u != small)
		free(u);
	return r;
}

int rn_tag_has(uint16_t tag, const char *grammeme)
{
	return cmorphy3_gset_has(cmorphy3_tag_gset(rn_morph, tag), cmorphy3_grammeme(rn_morph, grammeme));
}

int rn_tag_case_is(uint16_t tag, const char *gcase)
{
	return cmorphy3_tag_get(rn_morph, tag, CMORPHY3_CASE) == cmorphy3_grammeme(rn_morph, gcase);
}
