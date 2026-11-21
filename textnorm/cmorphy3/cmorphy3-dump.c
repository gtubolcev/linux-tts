/* cmorphy3-dump DICT.bin < words > out.jsonl: parses and inflections of each input word, in the
 * format of pymorphy3-dump.py (pymorphy3), for compare-dump.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmorphy3.h"

#define NINFL_PARSES 3

static const char *const REQS[] = {
	"nomn,sing", "gent,sing", "datv,sing", "accs,sing", "ablt,sing", "loct,sing",
	"nomn,plur", "gent,plur", "datv,plur", "accs,plur", "ablt,plur", "loct,plur",
	"nomn", "gent", "datv", "accs", "ablt", "loct", "gen2", "loc2", "voct",
	"masc", "femn", "neut", "plur", "masc,ablt", "femn,gent",
};

static void jstr(const char *s)
{
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			printf("\\%c", *p);
		else if (*p < 0x20)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

static void jparse(const cmorphy3 *m, const cmorphy3_form *p)
{
	putchar('[');
	jstr(p->word);
	putchar(',');
	jstr(cmorphy3_tag_str(m, p->tag));
	putchar(',');
	jstr(p->normal_form);
	printf(",%.17g]", p->score);
}

int main(int argc, char **argv)
{
	const char *err;
	if (argc != 2) {
		fprintf(stderr, "usage: cmorphy3-dump DICT.bin < words\n");
		return 2;
	}
	cmorphy3 *m = cmorphy3_open(argv[1], &err);
	if (!m) {
		fprintf(stderr, "cmorphy3-dump: %s\n", err);
		return 1;
	}
	cmorphy3_gset req[sizeof(REQS) / sizeof(REQS[0])];
	for (size_t i = 0; i < sizeof(REQS) / sizeof(REQS[0]); i++)
		if (!cmorphy3_grammemes(m, REQS[i], &req[i])) {
			fprintf(stderr, "bad grammemes %s\n", REQS[i]);
			return 1;
		}
	cmorphy3_arena *a = cmorphy3_arena_new();
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, stdin)) >= 0) {
		if (len && line[len - 1] == '\n')
			line[--len] = 0;
		cmorphy3_arena_reset(a);
		cmorphy3_form *ps;
		int n = cmorphy3_parse(m, a, line, &ps);
		if (n < 0) {
			fprintf(stderr, "out of memory\n");
			return 1;
		}
		printf("{\"w\":");
		jstr(line);
		printf(",\"k\":%d,\"p\":[", cmorphy3_word_is_known(m, line));
		for (int i = 0; i < n; i++) {
			if (i)
				putchar(',');
			jparse(m, &ps[i]);
		}
		printf("],\"l\":[");
		for (int i = 0; i < n && i < NINFL_PARSES; i++) {
			cmorphy3_form *lx;
			int k = cmorphy3_lexeme(m, a, &ps[i], &lx);
			if (i)
				putchar(',');
			putchar('[');
			for (int j = 0; j < k; j++) {
				if (j)
					putchar(',');
				jparse(m, &lx[j]);
			}
			putchar(']');
		}
		printf("],\"i\":[");
		for (int i = 0; i < n && i < NINFL_PARSES; i++) {
			if (i)
				putchar(',');
			putchar('[');
			for (size_t r = 0; r < sizeof(REQS) / sizeof(REQS[0]); r++) {
				cmorphy3_form o;
				if (r)
					putchar(',');
				if (cmorphy3_inflect(m, a, &ps[i], req[r], &o) == 1)
					jparse(m, &o);
				else
					printf("null");
			}
			putchar(']');
		}
		printf("]}\n");
	}
	free(line);
	cmorphy3_arena_free(a);
	cmorphy3_close(m);
	return 0;
}
