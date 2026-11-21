/* units-check TOKENS.txt: rn_unit_resolve of each line, as units-check.py prints it */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-units.h"

int main(int argc, char **argv)
{
	FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
	if (!f)
		return 2;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	rn_str s = {0};
	while ((len = getline(&line, &cap, f)) >= 0) {
		if (len && line[len - 1] == '\n')
			line[--len] = 0;
		s.n = 0;
		rn_str_utf8(&s, line);
		const rn_unit *u = rn_unit_resolve(s.s ? s.s : (const uint32_t *)U"", s.n);
		if (!u)
			puts("-");
		else
			printf("%s|%s|%s|%s\n", u->lemma, u->gender, u->category,
			       u->suffix ? u->suffix : "None");
	}
	return 0;
}
