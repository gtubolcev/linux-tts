/* cmorphy3: the pymorphy3 morphological analyzer for Russian, in C.
 *
 * A port of pymorphy3 2.0.6 (MIT; Mikhail Korobov and contributors) and DAWG-Python, made
 * to give the same parses, scores and inflections as pymorphy3 with the pure-Python DAWG
 * reader. The dictionary is pymorphy3-dicts-ru converted by mkdict.py into one file that is
 * mmapped read-only, so processes share it.
 *
 * Strings are UTF-8. Results are allocated in a cmorphy3_arena owned by the caller; they
 * stay valid until the arena is reset or freed. A cmorphy3 is read-only after cmorphy3_open
 * and may be shared between threads, each with its own arena.
 */
#ifndef CMORPHY3_H
#define CMORPHY3_H

#include <stddef.h>
#include <stdint.h>

typedef struct cmorphy3 cmorphy3;
typedef struct cmorphy3_arena cmorphy3_arena;
typedef struct cmorphy3_stack cmorphy3_stack;

/* a set of grammemes (bit i = grammeme index i) */
typedef struct {
	uint64_t lo, hi;
} cmorphy3_gset;

typedef struct {
	const char *word;        /* the analysed form, lower case */
	const char *normal_form;
	uint16_t tag;            /* tag id: cmorphy3_tag_str() etc. */
	double score;
	const cmorphy3_stack *stack;   /* how it was found; needed for inflection */
} cmorphy3_form;

enum cmorphy3_category {
	CMORPHY3_POS, CMORPHY3_ANIMACY, CMORPHY3_ASPECT, CMORPHY3_CASE, CMORPHY3_GENDER, CMORPHY3_INVOLVEMENT, CMORPHY3_MOOD,
	CMORPHY3_NUMBER, CMORPHY3_PERSON, CMORPHY3_TENSE, CMORPHY3_TRANSITIVITY, CMORPHY3_VOICE, CMORPHY3_NCATEGORIES
};

cmorphy3_arena *cmorphy3_arena_new(void);
void cmorphy3_arena_reset(cmorphy3_arena *a);
void cmorphy3_arena_free(cmorphy3_arena *a);
void *cmorphy3_arena_alloc(cmorphy3_arena *a, size_t n);
char *cmorphy3_arena_strdup(cmorphy3_arena *a, const char *s);

/* NULL on error, with a static message in *err if err is not NULL */
cmorphy3 *cmorphy3_open(const char *path, const char **err);
void cmorphy3_close(cmorphy3 *m);
const char *cmorphy3_revision(const cmorphy3 *m);

/* MorphAnalyzer.parse(word): the parses in pymorphy3's order; returns the count (>= 1
 * unless out of memory, then -1) */
int cmorphy3_parse(const cmorphy3 *m, cmorphy3_arena *a, const char *word, cmorphy3_form **out);
/* Parse.inflect(required): 1 and *out, or 0 when there is no such form */
int cmorphy3_inflect(const cmorphy3 *m, cmorphy3_arena *a, const cmorphy3_form *form, cmorphy3_gset required,
                     cmorphy3_form *out);
/* Parse.lexeme */
int cmorphy3_lexeme(const cmorphy3 *m, cmorphy3_arena *a, const cmorphy3_form *form, cmorphy3_form **out);
/* MorphAnalyzer.word_is_known(word) (е may stand for ё) */
int cmorphy3_word_is_known(const cmorphy3 *m, const char *word);
/* next(iter(dictionary.words.iterkeys(prefix)), None) is not None: some dictionary word
 * starts with the UTF-8 bytes of prefix (no lowercasing, no ё fuzziness) */
int cmorphy3_has_prefix(const cmorphy3 *m, const char *prefix);

/* grammemes and tags */
int cmorphy3_grammeme(const cmorphy3 *m, const char *name);        /* index or -1 */
const char *cmorphy3_grammeme_name(const cmorphy3 *m, int index);
int cmorphy3_grammeme_count(const cmorphy3 *m);
/* "sing,gent" → set; 0 when a name is unknown */
int cmorphy3_grammemes(const cmorphy3 *m, const char *names, cmorphy3_gset *out);
int cmorphy3_tag_count(const cmorphy3 *m);
const char *cmorphy3_tag_str(const cmorphy3 *m, uint16_t tag);
cmorphy3_gset cmorphy3_tag_gset(const cmorphy3 *m, uint16_t tag);
/* tag.POS, tag.case, …: grammeme index or -1 (None) */
int cmorphy3_tag_get(const cmorphy3 *m, uint16_t tag, enum cmorphy3_category c);
/* OpencorporaTag.fix_rare_cases */
cmorphy3_gset cmorphy3_fix_rare_cases(const cmorphy3 *m, cmorphy3_gset g);

static inline int cmorphy3_gset_has(cmorphy3_gset s, int i)
{
	return i >= 0 && (i < 64 ? (s.lo >> i) & 1 : (s.hi >> (i - 64)) & 1);
}
static inline int cmorphy3_gset_subset(cmorphy3_gset a, cmorphy3_gset b)
{
	return (a.lo & ~b.lo) == 0 && (a.hi & ~b.hi) == 0;
}

#endif
