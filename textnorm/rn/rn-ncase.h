/* get_numeral_case of numerals/_helpers.py: the 33 case rules and their resolution
 * (numerals/_rules.py). Cases are cmorphy3 grammeme indices (rn_g.nomn, ...). */
#ifndef RN_NCASE_H
#define RN_NCASE_H
#include "rn-mx.h"

typedef struct {
	const uint32_t *s;
	size_t n;
} rn_tok;

/* _NumeralTokenContext over toks (kept by pointer; must outlive the context) */
typedef struct rn_ncase rn_ncase;
rn_ncase *rn_ncase_new(const rn_tok *toks, size_t n);
void rn_ncase_free(rn_ncase *c);
/* get_numeral_case(tokens, idx, _visible_length=vlen) (vlen is capped at n); *explicit_
 * (may be NULL) as get_numeral_case_ex. The case, -1 on error. */
int rn_ncase_get(rn_ncase *c, size_t idx, size_t vlen, int *explicit_);

/* simple_tokenize(s): the tokens as slices of s, malloc'ed; -1 on error */
long rn_simple_tokenize(const uint32_t *s, size_t n, rn_tok **out);

/* is_integer_token / parse_integer_token: 1 and the digits' value (saturated) and value
 * mod 1000, or 0 */
typedef struct {
	int neg;
	uint64_t v;      /* UINT64_MAX when it does not fit */
	unsigned m1000;
	char digits[48]; /* the normalized digits, "" when longer */
} rn_int;
int rn_parse_int_token(const uint32_t *s, size_t n, rn_int *out);
int rn_is_int_token(const uint32_t *s, size_t n);
#endif
