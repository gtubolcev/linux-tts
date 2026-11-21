/* re-charsets PY-CHARSETS.txt: compare PCRE2's classes used for Python's \w, \d, \s with the
 * sets Python itself has (gen-patterns.py writes py-charsets.txt). Prints code points in
 * one set only; exit 1 if any. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-re.h"

#define MAXCP 0x110000

static const char *CLS[] = {
	"w", "[\\p{L}\\p{N}_]",
	"d", "[\\p{Nd}]",
	"s", "[\\t\\n\\x{b}\\f\\r\\x{1c}-\\x{20}\\x{85}\\x{a0}\\x{1680}\\x{2000}-\\x{200a}"
	     "\\x{2028}\\x{2029}\\x{202f}\\x{205f}\\x{3000}]",
};

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	FILE *f = fopen(argv[1], "r");
	if (!f)
		return 2;
	static char line[1 << 20];
	static unsigned char py[MAXCP];
	int bad = 0;
	while (fgets(line, sizeof line, f)) {
		char *tok = strtok(line, " \n");
		const char *pat = NULL;
		for (size_t i = 0; i < sizeof CLS / sizeof *CLS; i += 2)
			if (!strcmp(tok, CLS[i]))
				pat = CLS[i + 1];
		if (!pat)
			continue;
		char name = tok[0];
		memset(py, 0, sizeof py);
		while ((tok = strtok(NULL, " \n"))) {
			unsigned long a, b;
			if (sscanf(tok, "%lx-%lx", &a, &b) == 2)
				for (unsigned long c = a; c <= b && c < MAXCP; c++)
					py[c] = 1;
		}
		rn_re re;
		char err[300];
		if (rn_re_compile(&re, pat, err, sizeof err) < 0) {
			printf("%c: %s\n", name, err);
			return 2;
		}
		long only_py = 0, only_pcre = 0;
		for (uint32_t c = 0; c < MAXCP; c++) {
			if (c >= 0xD800 && c < 0xE000)
				continue;
			rn_match m;
			int r = rn_fullmatch(&re, &c, 1, 0, &m);
			if (r != py[c]) {
				printf("%c U+%04X %s\n", name, c, py[c] ? "python only" : "pcre2 only");
				py[c] ? only_py++ : only_pcre++;
			}
		}
		printf("%c: %ld python only, %ld pcre2 only\n", name, only_py, only_pcre);
		bad |= only_py || only_pcre;
		rn_re_free(&re);
	}
	return bad;
}
