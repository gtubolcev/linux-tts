/* the morphology for the rn passes: cmorphy3 opened once (pymorphy3's MorphAnalyzer) */
#ifndef RN_MORPH_H
#define RN_MORPH_H

#include "../cmorphy3/cmorphy3.h"
#include "rn-re.h"

extern cmorphy3 *rn_morph;
int rn_morph_open(const char *path, const char **err);   /* 0, -1 */
void rn_morph_close(void);
/* word_is_known(word) of _morph.py (lowercases) */
int rn_word_is_known(const uint32_t *s, size_t n);
/* UTF-8 copy of s[0..n), NUL-terminated, appended to buf (grown with realloc) */
char *rn_to_utf8(const uint32_t *s, size_t n, char *buf, size_t *cap);

#endif
