/* rn-re: Python re semantics on PCRE2 (32-bit code units, UTF), for the ru_normalizr port.
 *
 * Text is UTF-32, so offsets are code point indices as in Python str. Patterns are the
 * PCRE2 translations made by gen-patterns.py (rn-patterns.h) or built at run time in the
 * same dialect. Search, match, fullmatch, finditer and sub follow Python's rules, including
 * empty matches: after an empty match the next one may not be empty at the same position
 * (Python 3.7+).
 */
#ifndef RN_RE_H
#define RN_RE_H

#include <stddef.h>
#include <stdint.h>

#define PCRE2_CODE_UNIT_WIDTH 32
#include <pcre2.h>

typedef struct rn_re {
	pcre2_code *code;
	uint32_t ngroups;   /* capturing groups, as Python's pattern.groups */
} rn_re;

#define RN_UNSET ((size_t)-1)
#define RN_MAXGROUPS 64

typedef struct {
	const uint32_t *s;   /* the subject */
	size_t n;
	uint32_t ngroups;
	size_t start[RN_MAXGROUPS + 1], end[RN_MAXGROUPS + 1];   /* RN_UNSET if unmatched */
} rn_match;

extern int rn_re_errcode;   /* PCRE2 error code after a match returned -1 */

/* 0, or -1 with a message in err */
int rn_re_compile(rn_re *re, const char *pcre, char *err, size_t errlen);
void rn_re_free(rn_re *re);
int rn_re_group(const rn_re *re, const char *name);   /* number of a named group, -1 */

/* pattern.search(s, pos) / match / fullmatch: 1 match, 0 none, -1 error */
int rn_search(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m);
int rn_match_at(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m);
int rn_fullmatch(const rn_re *re, const uint32_t *s, size_t n, size_t pos, rn_match *m);

/* finditer: it = rn_iter_init(...); while (rn_iter_next(&it, &m) == 1) ... */
typedef struct {
	const rn_re *re;
	const uint32_t *s;
	size_t n, pos;
	int must_advance, done;
} rn_iter;

rn_iter rn_iter_init(const rn_re *re, const uint32_t *s, size_t n, size_t pos);
int rn_iter_next(rn_iter *it, rn_match *m);

/* growable UTF-32 string */
typedef struct {
	uint32_t *s;
	size_t n, cap;
} rn_str;

int rn_str_put(rn_str *b, const uint32_t *s, size_t n);   /* 0, -1 out of memory */
int rn_str_putc(rn_str *b, uint32_t c);
int rn_str_utf8(rn_str *b, const char *utf8);             /* append UTF-8 text */
void rn_str_free(rn_str *b);

/* pattern.sub(repl, s, count): repl appends the replacement for m to out (0, or -1 to
 * abort); count 0 = all. Returns the number of substitutions, -1 on error. out is
 * appended to, not cleared. */
typedef int (*rn_repl_fn)(const rn_match *m, rn_str *out, void *ctx);
long rn_sub(const rn_re *re, const uint32_t *s, size_t n, rn_repl_fn repl, void *ctx,
            size_t count, rn_str *out);
/* repl = fixed text */
long rn_sub_text(const rn_re *re, const uint32_t *s, size_t n, const uint32_t *t, size_t tn,
                 size_t count, rn_str *out);
/* repl = template: code points 1..9 in tmpl stand for groups 1..9 (an unmatched group
 * gives ""), as in RN_U(" \1 дробь \2 ") */
long rn_sub_tmpl(const rn_re *re, const uint32_t *s, size_t n, const uint32_t *tmpl,
                 size_t count, rn_str *out);

/* in place on *cur: s = pattern.sub(...); the number of substitutions, -1 on error */
long rn_resub(const rn_re *re, rn_str *cur, rn_repl_fn repl, void *ctx);
long rn_resub_tmpl(const rn_re *re, rn_str *cur, const uint32_t *tmpl);
int rn_str_eq(const rn_str *a, const rn_str *b);
int rn_str_copy(rn_str *dst, const rn_str *src);   /* dst = src */

#endif
