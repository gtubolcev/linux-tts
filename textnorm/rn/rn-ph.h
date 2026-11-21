/* placeholders.py: private-use placeholders that hide text from the passes, and their
 * restoration. Ranges as in ru_normalizr (never overlapping). */
#ifndef RN_PH_H
#define RN_PH_H

#include "rn-re.h"

#define RN_PH_PREPROCESS 0xE000, 0xE0FF
#define RN_PH_GLUED_CODES 0xE100, 0xF8FD
#define RN_PH_NUMERAL_TOKENS 0xF0000, 0xFFFFD
#define RN_PH_WINDOWS_PATHS 0x100000, 0x1007FF
#define RN_PH_URLS 0x100800, 0x100FFF
#define RN_PH_OVERLONG_NUMBERS 0x101000, 0x1017FF

#define RN_LETTER_HYPHEN 0xE000
#define RN_NEGATIVE_NUMBER 0xE001
#define RN_UNIT_SLASH 0xE003
#define RN_BINARY_PLUS 0xE004
#define RN_BINARY_MINUS 0xE005
#define RN_BINARY_POWER 0xE006

typedef struct {
	uint32_t start, end, next;
	uint32_t *occupied;   /* range characters already in the text: never handed out */
	size_t noccupied;
	rn_str *orig;         /* orig[cp - start] for cp < next */
	size_t count;         /* placeholders handed out */
} rn_ph;

/* PlaceholderAllocator(range, text): text is only scanned here */
int rn_ph_init(rn_ph *a, uint32_t start, uint32_t end, const uint32_t *text, size_t n);
/* allocate(original): the placeholder, 0 if the range is exhausted, -1 out of memory */
long rn_ph_alloc(rn_ph *a, const uint32_t *orig, size_t n);
/* restore(text), appended to out; a range character without an original stays */
int rn_ph_restore(const rn_ph *a, const uint32_t *s, size_t n, rn_str *out);
void rn_ph_free(rn_ph *a);

#endif
