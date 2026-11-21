#define RN_PATTERNS_DEFINE
#include "rn-pat.h"

#include <stdio.h>

static rn_re PAT[RN_NPATTERNS];
static int ready;

int rn_pat_init(char *err, size_t errlen)
{
	if (ready)
		return 0;
	for (int i = 0; i < RN_NPATTERNS; i++) {
		char e[256];
		if (rn_re_compile(&PAT[i], RN_PCRE[i], e, sizeof e) < 0) {
			snprintf(err, errlen, "pattern %d: %s", i, e);
			while (i--)
				rn_re_free(&PAT[i]);
			return -1;
		}
	}
	ready = 1;
	return 0;
}

void rn_pat_free(void)
{
	if (!ready)
		return;
	for (int i = 0; i < RN_NPATTERNS; i++)
		rn_re_free(&PAT[i]);
	ready = 0;
}

const rn_re *rn_pat(enum rn_pattern id)
{
	return &PAT[id];
}
