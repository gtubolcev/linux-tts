/* pystr-check: per code point the rn-pystr predicates and mappings, for comparing with
 * pystr-check.py (the oracle's Python); plus str.lower() of sigma contexts. */
#include <stdio.h>

#include "rn-pystr.h"

static void put(const rn_str *s)
{
	for (size_t i = 0; i < s->n; i++)
		printf(" %X", s->s[i]);
}

int main(void)
{
	for (uint32_t c = 0; c < 0x110000; c++) {
		if (c >= 0xD800 && c < 0xE000)
			continue;
		rn_str l = {0}, u = {0}, f = {0};
		rn_py_lower(&c, 1, &l);
		rn_py_upper(&c, 1, &u);
		rn_py_casefold(&c, 1, &f);
		printf("%X %d%d%d%d%d%d%d%d%d", c, rn_py_isalpha(c), rn_py_isdecimal(c), rn_py_isdigit(c),
		       rn_py_isnumeric(c), rn_py_isalnum(c), rn_py_isupper(c), rn_py_islower(c),
		       rn_py_isspace(c), rn_py_str_isupper(&c, 1));
		put(&l);
		printf(" |");
		put(&u);
		printf(" |");
		put(&f);
		printf("\n");
		rn_str_free(&l);
		rn_str_free(&u);
		rn_str_free(&f);
	}
	static const uint32_t ctx[][4] = {{'A', 0x3A3, 0}, {'1', 0x3A3, 0}, {'A', 0x3A3, 'B', 0},
	                                  {'A', 0xAD, 0x3A3, 0}, {0x3A3, 0}, {'A', 0x3A3, 0xAD, 0}};
	for (size_t k = 0; k < sizeof ctx / sizeof *ctx; k++) {
		size_t n = 0;
		while (ctx[k][n])
			n++;
		rn_str l = {0};
		rn_py_lower(ctx[k], n, &l);
		printf("ctx");
		put(&l);
		printf("\n");
		rn_str_free(&l);
	}
	return 0;
}
