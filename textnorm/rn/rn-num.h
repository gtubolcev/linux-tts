/* numbers for the rn passes: num2words (../num2words) and roman, as ru_normalizr calls them */
#ifndef RN_NUM_H
#define RN_NUM_H

#include "../num2words/n2w-ru.h"
#include "rn-re.h"

/* N2W_FIX_* used by every num2words call; 0 (the default) = the oracle's output */
extern unsigned rn_n2w_fixes;
/* RN_FIX_* (QUIRKS.md, R…) used by the rn passes; 0 (the default) = the oracle's output */
extern unsigned rn_fixes;
#define RN_FIX_ROMAN_RANGE_NOUN 0x1u   /* R5 */
#define RN_FIX_TIME_NO_COMMA 0x2u      /* R6 */
#define RN_FIX_SECONDS_FEMN 0x4u       /* R7 */
#define RN_FIX_TIME_GENT_FEMN 0x8u     /* R8 */
#define RN_FIX_TIME_GENT_ZERO 0x10u    /* R9 */
#define RN_FIX_DATE_PREP_CASE 0x20u    /* R10 */
#define RN_FIX_COUNTED_ONE 0x40u       /* R11 */

#define RN_NO_GENDER (-1)

/* str(int(s)) of decimal digits (any script) into buf: its length, -1 if cap is too small */
int rn_int_str(const uint32_t *s, size_t n, char *buf, size_t cap);
/* num_to_word(int(s)) of numbering.py: num2words cardinal, str(value) where it raises */
int rn_put_cardinal(rn_str *out, const uint32_t *s, size_t n);
/* num2words(int) of an ASCII int string; 1 if num2words raises (nothing appended) */
int rn_put_cardinal_a(rn_str *out, const char *number);
/* render_ordinal(number, case, gender, plural, inanimate) of ordinal_utils.py; number as
 * str(int); gender an n2w_gender or RN_NO_GENDER */
int rn_put_ordinal(rn_str *out, const char *number, enum n2w_case c, int gender, int plural,
                   int inanimate);
/* roman.fromRoman(s.upper()): the value, -1 if invalid */
long rn_roman_value(const uint32_t *s, size_t n);
/* out += s.capitalize() */
int rn_put_capitalized(rn_str *out, const uint32_t *s, size_t n);

#endif
