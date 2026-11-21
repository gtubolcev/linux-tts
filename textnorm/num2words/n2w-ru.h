/* n2w-ru: Russian number words, a port of num2words 0.5.14 lang_RU (LGPL-2.1-or-later;
 * Taro Ogawa, Savoir-faire Linux and contributors) giving the same output as
 * num2words(number, lang="ru", to=..., case=..., gender=..., plural=..., animate=...).
 *
 * The number is passed as Python's str(number) would print it: "-?[0-9]+" for an int, or
 * "-?[0-9]+.[0-9]+" for a float (cardinal only, as in num2words). Like num2words, numbers
 * up to 10**33 - 1 are supported.
 *
 * Returns the length of the result (UTF-8, without the NUL) and writes as much as fits into
 * out (always NUL-terminated if cap > 0), like snprintf; -1 where num2words raises.
 *
 * opts.fixes selects corrections of num2words bugs (textnorm/QUIRKS.md); 0 gives num2words'
 * output exactly, which the oracle tests use.
 */
#ifndef N2W_RU_H
#define N2W_RU_H

#include <stddef.h>

enum n2w_case { N2W_NOMN, N2W_GENT, N2W_DATV, N2W_ACCS, N2W_ABLT, N2W_LOCT };
enum n2w_gender { N2W_MASC, N2W_FEMN, N2W_NEUT };

typedef struct {
	enum n2w_case gcase;     /* case= ('nominative' ...) */
	enum n2w_gender gender;  /* gender= ('m'/'masculine', 'f'/'feminine', 'n'/'neuter') */
	int plural;              /* plural= */
	int animate;             /* animate=; num2words' default is True */
	unsigned fixes;          /* N2W_FIX_* */
} n2w_opts;

/* QUIRKS.md N1: thousands, millions … are nouns: their numeral agrees with тысяча (f) or
 * миллион (m), inanimate, singular ("две тысячи" not "двух тысячи" in the animate
 * accusative, "одна миллион" -> "один миллион" for feminine, "одни тысяча" -> "одна") */
#define N2W_FIX_MAGNITUDE 1u
/* N2: floats: keep the minus of "-0.5", and "минус одна целая" (not "целых") */
#define N2W_FIX_FLOAT_SIGN 2u
#define N2W_FIX_ALL (N2W_FIX_MAGNITUDE | N2W_FIX_FLOAT_SIGN)

/* num2words' defaults: nominative, masculine, singular, animate; all fixes */
#define N2W_DEFAULTS ((n2w_opts){N2W_NOMN, N2W_MASC, 0, 1, N2W_FIX_ALL})

int n2w_ru_cardinal(const char *number, n2w_opts o, char *out, size_t cap);
int n2w_ru_ordinal(const char *number, n2w_opts o, char *out, size_t cap);

#endif
