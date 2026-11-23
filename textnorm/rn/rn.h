/* rn: Russian text normalization for TTS, the C port of ru_normalizr (NormalizeOptions.tts()).
 *
 * The data is loaded once per process and stays until rn_close(): the cmorphy3 dictionary
 * (mapped, shared between processes) and the English IPA table for Latin words. Calls are
 * serialized inside, so any thread may call rn_normalize().
 */
#ifndef RN_H
#define RN_H

#include <stddef.h>

/* RN_FIX_* / N2W_FIX_* masks: what rn_open() takes for "all fixes" (QUIRKS.md); 0 gives the
 * output of ru_normalizr itself */
#define RN_FIXES_ALL 0x3FFFFFFFu
#define RN_N2W_FIXES_ALL 0xFu

/* load the data (morph: cmorphy3-ru.bin, ipa: en-ipa.tsv) and compile the patterns; 0, or -1 with a message in err. Once loaded, later
 * calls only return 0 (the first fixes stay). */
int rn_open(const char *morph, const char *ipa, unsigned fixes, unsigned n2w_fixes, char *err,
            size_t errlen);
int rn_is_open(void);
/* the normalized text (malloc'd, NUL-terminated UTF-8), or NULL: not open, text not valid
 * UTF-8, out of memory, or a regex limit hit */
char *rn_normalize(const char *text, size_t len);
/* frees everything, for leak checkers at exit; rn_open() may not be called again */
void rn_close(void);

#endif
