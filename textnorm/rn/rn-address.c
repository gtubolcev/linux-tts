/* address.py: postal address abbreviations («ул. Полевая, д. 10, корп. 2») */
#include <stdlib.h>
#include <string.h>

#include "rn-morph.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-pystr.h"
#include "rn-util.h"

#define AD(id) rn_pat(RN_ADDRESS_##id)

struct abbr {
	const uint32_t *abbr;
	const char *lemma;
	char named, ambiguous;
};

static const struct abbr ABBRS[] = {
	{RN_U("ул."), "улица", 1, 0},       {RN_U("просп."), "проспект", 1, 0},
	{RN_U("пр-т"), "проспект", 1, 0},   {RN_U("пр-кт"), "проспект", 1, 0},
	{RN_U("пер."), "переулок", 1, 0},   {RN_U("пл."), "площадь", 1, 0},
	{RN_U("наб."), "набережная", 1, 0}, {RN_U("ш."), "шоссе", 1, 0},
	{RN_U("б-р"), "бульвар", 1, 0},     {RN_U("бул."), "бульвар", 1, 0},
	{RN_U("мкр."), "микрорайон", 1, 0}, {RN_U("мкрн."), "микрорайон", 1, 0},
	{RN_U("туп."), "тупик", 1, 0},      {RN_U("кв-л"), "квартал", 1, 0},
	{RN_U("дер."), "деревня", 1, 0},    {RN_U("д."), "дом", 0, 1},
	{RN_U("корп."), "корпус", 0, 0},    {RN_U("кор."), "корпус", 0, 1},
	{RN_U("к."), "корпус", 0, 1},       {RN_U("кв."), "квартира", 0, 0},
	{RN_U("стр."), "строение", 0, 1},   {RN_U("влд."), "владение", 0, 0},
	{RN_U("вл."), "владение", 0, 1},    {RN_U("пом."), "помещение", 0, 0},
	{RN_U("оф."), "офис", 0, 0},        {RN_U("эт."), "этаж", 0, 0},
	{RN_U("лит."), "литера", 0, 1},
};

struct prep {
	const uint32_t *word;
	const char *cases[2];
};

static const struct prep PREPS[] = {
	{RN_U("в"), {"loct", "accs"}},  {RN_U("во"), {"loct", "accs"}}, {RN_U("на"), {"loct", "accs"}},
	{RN_U("за"), {"ablt", "accs"}}, {RN_U("под"), {"ablt", "accs"}}, {RN_U("по"), {"datv"}},
	{RN_U("к"), {"datv"}},          {RN_U("ко"), {"datv"}},          {RN_U("с"), {"gent"}},
	{RN_U("со"), {"gent"}},         {RN_U("из"), {"gent"}},          {RN_U("от"), {"gent"}},
	{RN_U("до"), {"gent"}},         {RN_U("у"), {"gent"}},           {RN_U("около"), {"gent"}},
	{RN_U("возле"), {"gent"}},      {RN_U("напротив"), {"gent"}},    {RN_U("вдоль"), {"gent"}},
	{RN_U("мимо"), {"gent"}},       {RN_U("вблизи"), {"gent"}},      {RN_U("при"), {"loct"}},
	{RN_U("перед"), {"ablt"}},      {RN_U("между"), {"ablt"}},       {RN_U("через"), {"accs"}},
};

static int eq32(const uint32_t *a, size_t an, const uint32_t *b)
{
	return rn_u32len(b) == an && !memcmp(a, b, an * 4);
}

/* _specification: the entry, NULL */
static const struct abbr *spec_of(const rn_match *m)
{
	const rn_re *re = AD(ADDRESS_ELEMENT_PATTERN);
	size_t an, nn;
	const uint32_t *a = rn_group(m, rn_re_group(re, "abbr"), &an);
	int named = m->start[rn_re_group(re, "name")] != RN_UNSET;
	(void)nn;
	rn_str low = {0};
	const struct abbr *r = NULL;
	if (rn_py_lower(a, an, &low) < 0)
		return NULL;
	for (size_t i = 0; i < sizeof ABBRS / sizeof *ABBRS; i++)
		if (eq32(low.s, low.n, ABBRS[i].abbr))
			r = &ABBRS[i];
	rn_str_free(&low);
	return r && r->named == named ? r : NULL;
}

/* _preposition_cases: the entry, NULL for () */
static const struct prep *prep_cases(const uint32_t *s, size_t chain_start)
{
	size_t ws = chain_start > 32 ? chain_start - 32 : 0;
	rn_match m;
	if (rn_search(AD(PRECEDING_WORD_PATTERN), s + ws, chain_start - ws, 0, &m) != 1)
		return NULL;
	if (m.start[0] == 0 && ws > 0 &&
	    rn_match_at(AD(PRECEDING_WORD_CHAR_PATTERN), s + ws - 1, 1, 0, &m) == 1)
		return NULL;
	size_t wn;
	const uint32_t *w = rn_group(&m, rn_re_group(AD(PRECEDING_WORD_PATTERN), "word"), &wn);
	rn_str low = {0};
	const struct prep *r = NULL;
	if (rn_py_lower(w, wn, &low) < 0)
		return NULL;
	for (size_t i = 0; i < sizeof PREPS / sizeof *PREPS; i++)
		if (eq32(low.s, low.n, PREPS[i].word))
			r = &PREPS[i];
	rn_str_free(&low);
	return r;
}

/* canonicalize_case(tag.case) of the parses of the name's first word: bit per grammeme
 * index in *set (indices < 128); -1 error */
static int name_cases(const uint32_t *name, size_t n, cmorphy3_gset *set)
{
	size_t a = 0, b;
	while (a < n && rn_py_isspace(name[a]))
		a++;
	b = a;
	while (b < n && !rn_py_isspace(name[b]))
		b++;
	cmorphy3_arena *ar = cmorphy3_arena_new();
	cmorphy3_form *f;
	if (!ar)
		return -1;
	int k = rn_parse(ar, name + a, b - a, &f);
	*set = (cmorphy3_gset){0, 0};
	for (int i = 0; i < k; i++) {
		int c = cmorphy3_tag_get(rn_morph, f[i].tag, CMORPHY3_CASE);
		if (c < 0)
			continue;
		cmorphy3_gset g = {0, 0};
		if (c < 64)
			g.lo = 1ull << c;
		else
			g.hi = 1ull << (c - 64);
		g = cmorphy3_fix_rare_cases(rn_morph, g);
		set->lo |= g.lo;
		set->hi |= g.hi;
	}
	cmorphy3_arena_free(ar);
	return k < 0 ? -1 : 0;
}

static const char *target_case(const uint32_t *s, size_t chain_start, const rn_match *m)
{
	const struct prep *p = prep_cases(s, chain_start);
	if (!p)
		return "nomn";
	const rn_re *re = AD(ADDRESS_ELEMENT_PATTERN);
	int g = rn_re_group(re, "name");
	if (m->start[g] != RN_UNSET) {
		cmorphy3_gset av;
		if (name_cases(m->s + m->start[g], m->end[g] - m->start[g], &av) == 0 &&
		    !cmorphy3_gset_has(av, cmorphy3_grammeme(rn_morph, "nomn")))
			for (int i = 0; i < 2 && p->cases[i]; i++)
				if (cmorphy3_gset_has(av, cmorphy3_grammeme(rn_morph, p->cases[i])))
					return p->cases[i];
	}
	return p->cases[0];
}

/* _inflect_lemma */
static int put_lemma(rn_str *out, const char *lemma, const char *gcase, int upper_first)
{
	const char *word = lemma;
	cmorphy3_arena *a = NULL;
	if (strcmp(gcase, "nomn")) {
		cmorphy3_form *f, inf;
		a = cmorphy3_arena_new();
		if (!a)
			return -1;
		int k = cmorphy3_parse(rn_morph, a, lemma, &f), pick = -1;
		int noun = cmorphy3_grammeme(rn_morph, "NOUN"), adjf = cmorphy3_grammeme(rn_morph, "ADJF");
		for (int i = 0; i < k && pick < 0; i++)
			if (cmorphy3_tag_get(rn_morph, f[i].tag, CMORPHY3_POS) == noun)
				pick = i;
		for (int i = 0; i < k && pick < 0; i++)
			if (cmorphy3_tag_get(rn_morph, f[i].tag, CMORPHY3_POS) == adjf)
				pick = i;
		cmorphy3_gset req;
		if (pick >= 0 && cmorphy3_grammemes(rn_morph, gcase, &req) &&
		    cmorphy3_inflect(rn_morph, a, &f[pick], req, &inf) == 1)
			word = inf.word;
	}
	size_t mark = out->n;
	int r = rn_str_utf8(out, word);
	if (r == 0 && upper_first && out->n > mark) {
		rn_str up = {0};
		uint32_t c = out->s[mark];
		if (rn_py_upper(&c, 1, &up) < 0) {
			r = -1;
		} else {
			rn_str tail = {0};
			if (rn_str_put(&tail, out->s + mark + 1, out->n - mark - 1) < 0) {
				r = -1;
			} else {
				out->n = mark;
				r = rn_str_put(out, up.s, up.n) < 0 || rn_str_put(out, tail.s, tail.n) < 0 ? -1 : 0;
			}
			rn_str_free(&tail);
		}
		rn_str_free(&up);
	}
	if (a)
		cmorphy3_arena_free(a);
	return r;
}

typedef struct {
	rn_match m;
	const struct abbr *spec;
} elem;

int rn_pre_expand_address_abbreviations(const rn_str *in, rn_str *out)
{
	const uint32_t *s = in->s ? in->s : RN_U("");
	size_t n = in->n;
	rn_match m;
	int r = rn_search(AD(ADDRESS_HINT_PATTERN), s, n, 0, &m);
	if (r <= 0)
		return r < 0 ? -1 : rn_str_put(out, s, n);
	elem *e = NULL;
	size_t ne = 0, cap = 0;
	rn_iter it = rn_iter_init(AD(ADDRESS_ELEMENT_PATTERN), s, n, 0);
	while ((r = rn_iter_next(&it, &m)) == 1) {
		const struct abbr *sp = spec_of(&m);
		if (!sp)
			continue;
		if (ne == cap) {
			elem *t = realloc(e, (cap = cap ? 2 * cap : 16) * sizeof *e);
			if (!t) {
				free(e);
				return -1;
			}
			e = t;
		}
		e[ne].m = m;
		e[ne++].spec = sp;
	}
	if (r < 0) {
		free(e);
		return -1;
	}
	size_t last = 0;
	r = 0;
	const rn_re *re = AD(ADDRESS_ELEMENT_PATTERN);
	int gname = rn_re_group(re, "name"), gnum = rn_re_group(re, "number"),
	    gabbr = rn_re_group(re, "abbr");
	for (size_t c0 = 0; c0 < ne && !r;) {
		size_t c1 = c0 + 1;   /* _build_chains */
		while (c1 < ne) {
			rn_match g;
			size_t a = e[c1 - 1].m.end[0], b = e[c1].m.start[0];
			int k = b >= a ? rn_fullmatch(AD(CHAIN_GAP_PATTERN), s + a, b - a, 0, &g) : 0;
			if (k < 0) {
				r = -1;
				break;
			}
			if (!k)
				break;
			c1++;
		}
		int unamb = 0;
		for (size_t i = c0; i < c1; i++)
			unamb |= !e[i].spec->ambiguous;
		size_t chain_start = e[c0].m.start[0];
		int head = 1;
		for (size_t i = c0; i < c1 && !r; i++) {
			const rn_match *x = &e[i].m;
			if (e[i].spec->ambiguous && !unamb)
				continue;
			const char *gc = head ? target_case(s, chain_start, x) : "nomn";
			head = 0;
			int up = rn_py_isupper(s[x->start[gabbr]]);
			int g = x->start[gname] != RN_UNSET ? gname : gnum;
			if (rn_str_put(out, s + last, x->start[0] - last) < 0 ||
			    put_lemma(out, e[i].spec->lemma, gc, up) < 0 || rn_str_putc(out, ' ') < 0 ||
			    rn_str_put(out, s + x->start[g], x->end[g] - x->start[g]) < 0)
				r = -1;
			last = x->end[0];
		}
		c0 = c1;
	}
	free(e);
	if (r < 0)
		return -1;
	return rn_str_put(out, s + last, n - last);
}
