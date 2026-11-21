/* morph-dump DICT.bin < words > out.jsonl: parses and inflections of each input word, in the
 * format of morph-dump.py (pymorphy3), for compare-dump.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "morph.h"

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

static void jparse(const tn_morph *m, const tn_parse *p)
{
	putchar('[');
	jstr(p->word);
	putchar(',');
	jstr(tn_morph_tag_str(m, p->tag));
	putchar(',');
	jstr(p->normal_form);
	printf(",%.17g]", p->score);
}

int main(int argc, char **argv)
{
	const char *err;
	if (argc != 2) {
		fprintf(stderr, "usage: morph-dump DICT.bin < words\n");
		return 2;
	}
	tn_morph *m = tn_morph_open(argv[1], &err);
	if (!m) {
		fprintf(stderr, "morph-dump: %s\n", err);
		return 1;
	}
	tn_gset req[sizeof(REQS) / sizeof(REQS[0])];
	for (size_t i = 0; i < sizeof(REQS) / sizeof(REQS[0]); i++)
		if (!tn_morph_gset(m, REQS[i], &req[i])) {
			fprintf(stderr, "bad grammemes %s\n", REQS[i]);
			return 1;
		}
	tn_arena *a = tn_arena_new();
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, stdin)) >= 0) {
		if (len && line[len - 1] == '\n')
			line[--len] = 0;
		tn_arena_reset(a);
		tn_parse *ps;
		int n = tn_morph_parse(m, a, line, &ps);
		if (n < 0) {
			fprintf(stderr, "out of memory\n");
			return 1;
		}
		printf("{\"w\":");
		jstr(line);
		printf(",\"k\":%d,\"p\":[", tn_morph_word_is_known(m, line));
		for (int i = 0; i < n; i++) {
			if (i)
				putchar(',');
			jparse(m, &ps[i]);
		}
		printf("],\"l\":[");
		for (int i = 0; i < n && i < NINFL_PARSES; i++) {
			tn_parse *lx;
			int k = tn_morph_lexeme(m, a, &ps[i], &lx);
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
				tn_parse o;
				if (r)
					putchar(',');
				if (tn_morph_inflect(m, a, &ps[i], req[r], &o) == 1)
					jparse(m, &o);
				else
					printf("null");
			}
			putchar(']');
		}
		printf("]}\n");
	}
	free(line);
	tn_arena_free(a);
	tn_morph_close(m);
	return 0;
}
