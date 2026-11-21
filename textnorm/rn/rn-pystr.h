/* Python str semantics on UTF-32: character predicates and case mappings, with the
 * oracle's Unicode version (tables in rn-pyuni.h from gen-pyuni.py). */
#ifndef RN_PYSTR_H
#define RN_PYSTR_H

#include "rn-re.h"

int rn_py_isalpha(uint32_t c);
int rn_py_isdecimal(uint32_t c);
int rn_py_isdigit(uint32_t c);
int rn_py_isnumeric(uint32_t c);
int rn_py_isalnum(uint32_t c);
int rn_py_isupper(uint32_t c);   /* of one character, as "c".isupper() */
int rn_py_islower(uint32_t c);
int rn_py_isspace(uint32_t c);
int rn_py_decimal(uint32_t c);   /* unicodedata.decimal(c), -1 if none */

/* s.lower() / s.upper() / s.casefold(), appended to out */
int rn_py_lower(const uint32_t *s, size_t n, rn_str *out);
int rn_py_upper(const uint32_t *s, size_t n, rn_str *out);
int rn_py_casefold(const uint32_t *s, size_t n, rn_str *out);
/* s.capitalize(): the first character in title case, the rest lowered */
int rn_py_capitalize(const uint32_t *s, size_t n, rn_str *out);
/* str.isupper() / islower() / isdigit() / isalpha() of a whole string */
int rn_py_str_isupper(const uint32_t *s, size_t n);
int rn_py_str_islower(const uint32_t *s, size_t n);
int rn_py_str_isdigit(const uint32_t *s, size_t n);
int rn_py_str_isalpha(const uint32_t *s, size_t n);

/* s.replace(old, new), appended to out */
int rn_py_replace(const uint32_t *s, size_t n, const uint32_t *old, size_t on,
                  const uint32_t *new, size_t nn, rn_str *out);
/* in place on *cur: s = s.replace(old, new) with NUL-terminated old/new */
int rn_py_rereplace(rn_str *cur, const uint32_t *old, const uint32_t *new);
/* s.strip() bounds: [*a, *b) */
void rn_py_strip(const uint32_t *s, size_t n, size_t *a, size_t *b);
/* the index of old in s from pos, or -1 */
long rn_py_find(const uint32_t *s, size_t n, const uint32_t *old, size_t on, size_t pos);

#endif
