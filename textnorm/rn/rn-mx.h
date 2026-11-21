/* morphology helpers for the case rules and roman numerals: parse_word with a cache (as
 * _morph.py's lru_cache), grammeme indices, canonicalize_case. Thread-local; the forms stay
 * valid until rn_mx_trim() (called by passes at their start, never inside one). */
#ifndef RN_MX_H
#define RN_MX_H
#include "rn-morph.h"

typedef struct {
	int NOUN, VERB, INFN, GRND, PREP, CONJ, NPRO, ADJF, PRTF, PRTS, NUMR;
	int nomn, gent, datv, accs, ablt, loct, loc2, voct, sing, plur;
	int masc, femn, neut, msf, anim, inan;
	int Fixd, Name, Surn, Patr, Geox, Orgn, Abbr, pssv, tran, past;
} rn_gram;
extern rn_gram rn_g;

int rn_mx_init(void);   /* after rn_morph_open; 0, -1 */
/* out of memory inside a helper: set, the helper returns a stand-in (the parse of "") */
extern _Thread_local int rn_mx_failed;
void rn_mx_trim(void);   /* drops the cache when it has grown large */

/* parse_word(s): the parses (count >= 1) */
int rn_mparse(const uint32_t *s, size_t n, const cmorphy3_form **out);
int rn_mparse8(const char *utf8, const cmorphy3_form **out);
/* first_parse_required(s) */
const cmorphy3_form *rn_mfirst(const uint32_t *s, size_t n);
/* parse.inflect(g): 1 and *out (word valid until rn_mx_trim), 0 */
int rn_minflect(const cmorphy3_form *f, cmorphy3_gset g, cmorphy3_form *out);

static inline cmorphy3_gset rn_gs(uint16_t tag)
{
	return cmorphy3_tag_gset(rn_morph, tag);
}
static inline int rn_has(uint16_t tag, int g)   /* "g" in tag */
{
	return cmorphy3_gset_has(cmorphy3_tag_gset(rn_morph, tag), g);
}
static inline int rn_pos(uint16_t tag)
{
	return cmorphy3_tag_get(rn_morph, tag, CMORPHY3_POS);
}
static inline int rn_case(uint16_t tag)
{
	return cmorphy3_tag_get(rn_morph, tag, CMORPHY3_CASE);
}
static inline int rn_number(uint16_t tag)
{
	return cmorphy3_tag_get(rn_morph, tag, CMORPHY3_NUMBER);
}
static inline int rn_gender(uint16_t tag)
{
	return cmorphy3_tag_get(rn_morph, tag, CMORPHY3_GENDER);
}
static inline int rn_animacy(uint16_t tag)
{
	return cmorphy3_tag_get(rn_morph, tag, CMORPHY3_ANIMACY);
}
static inline cmorphy3_gset rn_gset1(int g)
{
	cmorphy3_gset s = {0, 0};
	if (g >= 64)
		s.hi = 1ull << (g - 64);
	else if (g >= 0)
		s.lo = 1ull << g;
	return s;
}
static inline cmorphy3_gset rn_gset_or(cmorphy3_gset a, cmorphy3_gset b)
{
	return (cmorphy3_gset){a.lo | b.lo, a.hi | b.hi};
}
/* canonicalize_case(case): -1 (None) gives nomn */
int rn_canon_case(int c);
/* canonicalize_grammemes */
static inline cmorphy3_gset rn_canon(cmorphy3_gset g)
{
	return cmorphy3_fix_rare_cases(rn_morph, g);
}
/* is_case_reliable_noun */
static inline int rn_reliable(const cmorphy3_form *f)
{
	return f && rn_has(f->tag, rn_g.NOUN) && !rn_has(f->tag, rn_g.Fixd);
}
/* a case grammeme as num2words' case (resolve_num2words_case: others → nominative) */
int rn_n2w_case(int c);
const char *rn_gname(int g);   /* grammeme name, "" for -1 */
#endif
