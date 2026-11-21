/* re-parity PCRE.txt TEXTS.bin [PATTERN_NO]: finditer of every pattern over every text.
 *
 * PCRE.txt: one translated pattern per line. TEXTS.bin: records "<bytes>\n<utf-8>".
 * Prints per pattern an FNV-1a digest of all match and group spans and the match count;
 * with PATTERN_NO, the spans themselves for that pattern. py-re-parity.py prints the same
 * from Python's re. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-re.h"

static uint64_t H;

static void hs(const char *s)
{
	for (; *s; s++)
		H = (H ^ (unsigned char)*s) * 0x100000001b3ULL;
}

static size_t utf8_to_u32(const unsigned char *s, size_t n, uint32_t *out)
{
	size_t k = 0;
	for (size_t i = 0; i < n;) {
		uint32_t c = s[i];
		int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
		if (len > 1)
			c &= 0x3F >> (len - 1);
		for (int j = 1; j < len && i + j < n; j++)
			c = c << 6 | (s[i + j] & 0x3F);
		out[k++] = c;
		i += len;
	}
	return k;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	FILE *fp = fopen(argv[1], "r"), *ft = fopen(argv[2], "rb");
	if (!fp || !ft)
		return 2;
	long only = argc > 3 ? atol(argv[3]) : -1;
	/* texts */
	size_t nt = 0, cap = 1024;
	uint32_t **txt = malloc(cap * sizeof *txt);
	size_t *tlen = malloc(cap * sizeof *tlen);
	size_t blen;
	while (fscanf(ft, "%zu", &blen) == 1) {
		fgetc(ft);
		unsigned char *b = malloc(blen + 1);
		if (fread(b, 1, blen, ft) != blen)
			return 2;
		if (nt == cap) {
			cap *= 2;
			txt = realloc(txt, cap * sizeof *txt);
			tlen = realloc(tlen, cap * sizeof *tlen);
		}
		txt[nt] = malloc((blen + 1) * sizeof(uint32_t));
		tlen[nt] = utf8_to_u32(b, blen, txt[nt]);
		free(b);
		nt++;
	}
	static char line[1 << 20];
	long pno = -1;
	char buf[64];
	while (fgets(line, sizeof line, fp)) {
		pno++;
		line[strcspn(line, "\n")] = 0;
		if (only >= 0 && pno != only)
			continue;
		rn_re re;
		char err[300];
		if (rn_re_compile(&re, line, err, sizeof err) < 0) {
			printf("%ld ERROR %s\n", pno, err);
			continue;
		}
		H = 0xcbf29ce484222325ULL;
		size_t count = 0;
		for (size_t t = 0; t < nt; t++) {
			rn_iter it = rn_iter_init(&re, txt[t], tlen[t], 0);
			rn_match m;
			int r;
			while ((r = rn_iter_next(&it, &m)) == 1) {
				count++;
				snprintf(buf, sizeof buf, "%zu:", t);
				hs(buf);
				if (only >= 0)
					printf("%zu:", t);
				for (uint32_t g = 0; g <= m.ngroups; g++) {
					if (m.start[g] == RN_UNSET)
						snprintf(buf, sizeof buf, " -1,-1");
					else
						snprintf(buf, sizeof buf, " %zu,%zu", m.start[g], m.end[g]);
					hs(buf);
					if (only >= 0)
						fputs(buf, stdout);
				}
				hs(";");
				if (only >= 0)
					putchar('\n');
			}
			if (r < 0)
				printf("%ld MATCHERROR %d text %zu\n", pno, rn_re_errcode, t);
		}
		if (only < 0)
			printf("%ld %zu %016llx\n", pno, count, (unsigned long long)H);
		rn_re_free(&re);
	}
	return 0;
}
