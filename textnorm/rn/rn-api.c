/* rn.h: the public entry points around rn_run_pipeline (rn-norm.c) */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn.h"
#include "rn-lat.h"
#include "rn-morph.h"
#include "rn-mx.h"
#include "rn-num.h"
#include "rn-pass.h"
#include "rn-pat.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int state;   /* 0 never opened, 1 open, 2 closed */

int rn_open(const char *morph, const char *ipa, unsigned fixes, unsigned n2w_fixes, char *err,
            size_t errlen)
{
	int r = -1;
	const char *why = NULL;
	pthread_mutex_lock(&lock);
	if (state == 1) {
		r = 0;
		goto done;
	}
	if (!morph || !ipa) {
		snprintf(err, errlen, "no data paths");
		goto done;
	}
	if (state == 2) {
		snprintf(err, errlen, "rn_open after rn_close");
		goto done;
	}
	if (rn_morph_open(morph, &why) < 0) {
		snprintf(err, errlen, "%s: %s", morph, why ? why : "cannot open");
		goto done;
	}
	if (rn_mx_init() < 0) {
		snprintf(err, errlen, "%s: grammemes missing", morph);
		rn_morph_close();
		goto done;
	}
	why = "cannot open";
	if (rn_lat_open(ipa, &why) < 0) {
		snprintf(err, errlen, "%s: %s", ipa, why);
		rn_morph_close();
		goto done;
	}
	if (rn_pat_init(err, errlen) < 0) {
		rn_lat_close();
		rn_morph_close();
		goto done;
	}
	rn_fixes = fixes;
	rn_n2w_fixes = n2w_fixes;
	state = 1;
	r = 0;
done:
	pthread_mutex_unlock(&lock);
	return r;
}

int rn_is_open(void)
{
	pthread_mutex_lock(&lock);
	int r = state == 1;
	pthread_mutex_unlock(&lock);
	return r;
}

/* strict UTF-8: no overlongs, surrogates or code points above U+10FFFF */
static int valid_utf8(const unsigned char *s, size_t n)
{
	size_t i = 0;
	while (i < n) {
		unsigned c = s[i];
		size_t k;
		unsigned lo = 0x80, hi = 0xBF;
		if (c < 0x80) {
			if (c == 0)
				return 0;
			i++;
			continue;
		}
		if (c >= 0xC2 && c <= 0xDF)
			k = 1;
		else if (c >= 0xE0 && c <= 0xEF) {
			k = 2;
			if (c == 0xE0)
				lo = 0xA0;
			else if (c == 0xED)
				hi = 0x9F;
		} else if (c >= 0xF0 && c <= 0xF4) {
			k = 3;
			if (c == 0xF0)
				lo = 0x90;
			else if (c == 0xF4)
				hi = 0x8F;
		} else {
			return 0;
		}
		if (n - i <= k)
			return 0;
		for (size_t j = 1; j <= k; j++) {
			unsigned b = s[i + j];
			if (b < (j == 1 ? lo : 0x80) || b > (j == 1 ? hi : 0xBF))
				return 0;
		}
		i += k + 1;
	}
	return 1;
}

char *rn_normalize(const char *text, size_t len)
{
	char *res = NULL, *u8;
	rn_str in = {0}, out = {0};
	if (!text || !valid_utf8((const unsigned char *)text, len))
		return NULL;
	char *z = malloc(len + 1);
	if (!z)
		return NULL;
	memcpy(z, text, len);
	z[len] = 0;
	pthread_mutex_lock(&lock);
	if (state == 1 && rn_str_utf8(&in, z) == 0 && (in.s || rn_str_put(&in, NULL, 0) == 0) &&
	    rn_run_pipeline(&in, &out) == 0) {
		size_t cap = 0;
		u8 = rn_to_utf8(out.s, out.n, NULL, &cap);
		res = u8;
	}
	pthread_mutex_unlock(&lock);
	free(z);
	rn_str_free(&in);
	rn_str_free(&out);
	return res;
}

void rn_close(void)
{
	pthread_mutex_lock(&lock);
	if (state == 1) {
		rn_pat_free();
		rn_lat_close();
		rn_morph_close();
	}
	state = 2;
	pthread_mutex_unlock(&lock);
}
