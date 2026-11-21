/* the unit registry of ru_normalizr (numerals/unit_registry.py, _constants.py) */
#ifndef RN_UNITS_H
#define RN_UNITS_H

#include "rn-re.h"

typedef struct {
	const char *expected_normal_form;   /* NULL = None */
	const char *animacy;                /* NULL = None */
	int indeclinable;
	const char *preferred, *forbidden;  /* grammemes, space-separated, sorted */
	const char *seed;                   /* morphology_seed */
	const char *paradigm;               /* "case number word;..." */
	int plural_only, byte_family;
} rn_unit_morph;

typedef struct {
	const char *lemma, *gender, *category, *suffix;   /* UTF-8; suffix NULL = None */
	rn_unit_morph m;
} rn_unit;

/* UNIT_REGISTRY.resolve(token): the descriptor or NULL */
const rn_unit *rn_unit_resolve(const uint32_t *s, size_t n);
/* resolve_unit_expression(raw) of unit_expressions.py: the descriptor or NULL; *kind (may
 * be NULL) gets 'a'tomic, 'p'ower, 'd'ivision or 'x' product */
const rn_unit *rn_unit_resolve_expr(const uint32_t *s, size_t n, char *kind);
/* resolve_unit_form(token) (rn-unitform.c, needs the morphology): the descriptor or NULL;
 * sets rn_mx_failed when out of memory */
const rn_unit *rn_unit_resolve_form(const uint32_t *s, size_t n);
/* UNIT_REGISTRY.is_exact_symbol(token) */
int rn_unit_is_exact(const uint32_t *s, size_t n);

#endif
