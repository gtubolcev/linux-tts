/* tn-morph: the pymorphy3 morphological analyzer for Russian, in C.
 *
 * A port of pymorphy3 2.0.6 (MIT; Mikhail Korobov and contributors) and DAWG-Python, made
 * to give the same parses, scores and inflections as pymorphy3 with the pure-Python DAWG
 * reader. The dictionary is pymorphy3-dicts-ru converted by mkmorph.py into one file that is
 * mmapped read-only, so processes share it.
 *
 * Strings are UTF-8. Results are allocated in a tn_arena owned by the caller; they stay valid
 * until the arena is reset or freed. A tn_morph is read-only after tn_morph_open and may be
 * shared between threads, each with its own arena.
 */
#ifndef TN_MORPH_H
#define TN_MORPH_H

#include <stddef.h>
#include <stdint.h>

typedef struct tn_morph tn_morph;
typedef struct tn_arena tn_arena;
typedef struct tn_stack tn_stack;

/* a set of grammemes (bit i = grammeme index i) */
typedef struct {
	uint64_t lo, hi;
} tn_gset;

typedef struct {
	const char *word;        /* the analysed form, lower case */
	const char *normal_form;
	uint16_t tag;            /* tag id: tn_morph_tag_str() etc. */
	double score;
	const tn_stack *stack;   /* how it was found; needed for inflection */
} tn_parse;

enum tn_category {
	TN_POS, TN_ANIMACY, TN_ASPECT, TN_CASE, TN_GENDER, TN_INVOLVEMENT, TN_MOOD,
	TN_NUMBER, TN_PERSON, TN_TENSE, TN_TRANSITIVITY, TN_VOICE, TN_NCATEGORIES
};

tn_arena *tn_arena_new(void);
void tn_arena_reset(tn_arena *a);
void tn_arena_free(tn_arena *a);
void *tn_arena_alloc(tn_arena *a, size_t n);
char *tn_arena_strdup(tn_arena *a, const char *s);

/* NULL on error, with a static message in *err if err is not NULL */
tn_morph *tn_morph_open(const char *path, const char **err);
void tn_morph_close(tn_morph *m);
const char *tn_morph_revision(const tn_morph *m);

/* MorphAnalyzer.parse(word): the parses in pymorphy3's order; returns the count (>= 1
 * unless out of memory, then -1) */
int tn_morph_parse(const tn_morph *m, tn_arena *a, const char *word, tn_parse **out);
/* Parse.inflect(required): 1 and *out, or 0 when there is no such form */
int tn_morph_inflect(const tn_morph *m, tn_arena *a, const tn_parse *form, tn_gset required,
                     tn_parse *out);
/* Parse.lexeme */
int tn_morph_lexeme(const tn_morph *m, tn_arena *a, const tn_parse *form, tn_parse **out);
/* MorphAnalyzer.word_is_known(word) (е may stand for ё) */
int tn_morph_word_is_known(const tn_morph *m, const char *word);

/* grammemes and tags */
int tn_morph_grammeme(const tn_morph *m, const char *name);        /* index or -1 */
const char *tn_morph_grammeme_name(const tn_morph *m, int index);
int tn_morph_grammeme_count(const tn_morph *m);
/* "sing,gent" → set; 0 when a name is unknown */
int tn_morph_gset(const tn_morph *m, const char *names, tn_gset *out);
int tn_morph_tag_count(const tn_morph *m);
const char *tn_morph_tag_str(const tn_morph *m, uint16_t tag);
tn_gset tn_morph_tag_gset(const tn_morph *m, uint16_t tag);
/* tag.POS, tag.case, …: grammeme index or -1 (None) */
int tn_morph_tag_get(const tn_morph *m, uint16_t tag, enum tn_category c);
/* OpencorporaTag.fix_rare_cases */
tn_gset tn_morph_fix_rare_cases(const tn_morph *m, tn_gset g);

static inline int tn_gset_has(tn_gset s, int i)
{
	return i >= 0 && (i < 64 ? (s.lo >> i) & 1 : (s.hi >> (i - 64)) & 1);
}
static inline int tn_gset_subset(tn_gset a, tn_gset b)
{
	return (a.lo & ~b.lo) == 0 && (a.hi & ~b.hi) == 0;
}

#endif
