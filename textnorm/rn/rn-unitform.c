/* resolve_unit_form of numerals/_constants.py: a unit by its symbol or by an inflected form
 * of its name ("долларов" → доллар). Kept apart from rn-units.c, which works without the
 * morphology dictionary. */
#include <stdlib.h>
#include <string.h>

#include "rn-mx.h"
#include "rn-pystr.h"
#include "rn-units.h"

/* form == surface for one of "case number word;..." */
static int in_paradigm(const char *par, const char *surface)
{
	size_t sl = strlen(surface);
	while (par && *par) {
		const char *end = strchr(par, ';');
		size_t len = end ? (size_t)(end - par) : strlen(par);
		const char *w = par;
		for (int sp = 0; sp < 2 && w < par + len; w++)
			if (*w == ' ')
				sp++;
		if ((size_t)(par + len - w) == sl && !memcmp(w, surface, sl))
			return 1;
		par = end ? end + 1 : NULL;
	}
	return 0;
}

const rn_unit *rn_unit_resolve_form(const uint32_t *s, size_t n)
{
	const rn_unit *u = rn_unit_resolve(s, n);
	if (u)
		return u;
	rn_str low = {0}, nf = {0};
	char *surface = NULL;
	size_t cap = 0;
	if (rn_py_lower(s, n, &low) < 0)
		goto fail;
	size_t a = 0, b = low.n;   /* .strip(".") */
	while (a < b && low.s[a] == '.')
		a++;
	while (b > a && low.s[b - 1] == '.')
		b--;
	if (!(surface = rn_to_utf8(low.s + a, b - a, NULL, &cap)))
		goto fail;
	const cmorphy3_form *f;
	int count = rn_mparse(low.s + a, b - a, &f);
	for (int i = 0; i < count && !u; i++) {
		if (rn_pos(f[i].tag) != rn_g.NOUN)
			continue;
		nf.n = 0;
		if (rn_str_utf8(&nf, f[i].normal_form) < 0)
			goto fail;
		const rn_unit *d = rn_unit_resolve(nf.s, nf.n);
		if (!d)
			continue;
		const rn_unit_morph *m = &d->m;
		if (in_paradigm(m->paradigm, surface)) {
			u = d;
			break;
		}
		const char *expected = m->expected_normal_form && *m->expected_normal_form
		                               ? m->expected_normal_form
		                               : d->lemma;
		if (strcmp(f[i].normal_form, expected))
			continue;
		if (m->animacy) {
			int g = cmorphy3_grammeme(rn_morph, m->animacy);
			if (g < 0 || !rn_has(f[i].tag, g))
				continue;
		}
		u = d;
	}
	goto done;
fail:
	rn_mx_failed = 1;
	u = NULL;
done:
	free(surface);
	rn_str_free(&low);
	rn_str_free(&nf);
	return u;
}
