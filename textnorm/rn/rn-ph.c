#include "rn-ph.h"

#include <stdlib.h>
#include <string.h>

int rn_ph_init(rn_ph *a, uint32_t start, uint32_t end, const uint32_t *text, size_t n)
{
	memset(a, 0, sizeof *a);
	a->start = a->next = start;
	a->end = end;
	size_t cap = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = text[i];
		if (c < start || c > end)
			continue;
		size_t k = 0;
		while (k < a->noccupied && a->occupied[k] != c)
			k++;
		if (k < a->noccupied)
			continue;
		if (a->noccupied == cap) {
			cap = cap ? 2 * cap : 8;
			uint32_t *p = realloc(a->occupied, cap * sizeof *p);
			if (!p)
				return -1;
			a->occupied = p;
		}
		a->occupied[a->noccupied++] = c;
	}
	return 0;
}

long rn_ph_alloc(rn_ph *a, const uint32_t *orig, size_t n)
{
	while (a->next <= a->end) {
		uint32_t c = a->next;
		size_t i = c - a->start;
		if ((i & (i - 1)) == 0) {   /* grow orig at powers of two */
			size_t cap = i ? 2 * i : 1;
			rn_str *p = realloc(a->orig, cap * sizeof *p);
			if (!p)
				return -1;
			a->orig = p;
		}
		memset(&a->orig[i], 0, sizeof a->orig[i]);
		a->next++;
		size_t k = 0;
		while (k < a->noccupied && a->occupied[k] != c)
			k++;
		if (k < a->noccupied)
			continue;   /* orig[i] stays empty: not ours */
		if (rn_str_put(&a->orig[i], orig, n) < 0)
			return -1;
		a->count++;
		return c;
	}
	return 0;
}

int rn_ph_restore(const rn_ph *a, const uint32_t *s, size_t n, rn_str *out)
{
	size_t last = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (c < a->start || c >= a->next || !a->orig[c - a->start].s)
			continue;
		const rn_str *o = &a->orig[c - a->start];
		if (rn_str_put(out, s + last, i - last) < 0 || rn_str_put(out, o->s, o->n) < 0)
			return -1;
		last = i + 1;
	}
	return rn_str_put(out, s + last, n - last);
}

void rn_ph_free(rn_ph *a)
{
	for (uint32_t c = a->start; c < a->next; c++)
		rn_str_free(&a->orig[c - a->start]);
	free(a->orig);
	free(a->occupied);
	memset(a, 0, sizeof *a);
}
