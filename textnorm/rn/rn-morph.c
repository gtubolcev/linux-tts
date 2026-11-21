#include "rn-morph.h"

#include <stdlib.h>

cmorphy3 *rn_morph;

int rn_morph_open(const char *path, const char **err)
{
	rn_morph = cmorphy3_open(path, err);
	return rn_morph ? 0 : -1;
}

void rn_morph_close(void)
{
	cmorphy3_close(rn_morph);
	rn_morph = NULL;
}

char *rn_to_utf8(const uint32_t *s, size_t n, char *buf, size_t *cap)
{
	if (!buf || *cap < 4 * n + 1) {
		size_t c = 4 * n + 1 < 64 ? 64 : 4 * n + 1;
		char *p = realloc(buf, c);
		if (!p) {
			free(buf);
			*cap = 0;
			return NULL;
		}
		buf = p;
		*cap = c;
	}
	char *o = buf;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (c < 0x80) {
			*o++ = (char)c;
		} else if (c < 0x800) {
			*o++ = (char)(0xC0 | c >> 6);
			*o++ = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			*o++ = (char)(0xE0 | c >> 12);
			*o++ = (char)(0x80 | (c >> 6 & 0x3F));
			*o++ = (char)(0x80 | (c & 0x3F));
		} else {
			*o++ = (char)(0xF0 | c >> 18);
			*o++ = (char)(0x80 | (c >> 12 & 0x3F));
			*o++ = (char)(0x80 | (c >> 6 & 0x3F));
			*o++ = (char)(0x80 | (c & 0x3F));
		}
	}
	*o = 0;
	return buf;
}

static _Thread_local char *WBUF;
static _Thread_local size_t WCAP;

int rn_word_is_known(const uint32_t *s, size_t n)
{
	WBUF = rn_to_utf8(s, n, WBUF, &WCAP);
	return WBUF && cmorphy3_word_is_known(rn_morph, WBUF);
}
