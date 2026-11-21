/* QUIRKS.md R4 (our own pass, RN_FIX_SO): the preposition "с" becomes "со" before the
 * numerals that need it: "со ста", "со сто восьмой", "со второго", "со стотысячного".
 * Other words keep what the text has ("с стороны" in old texts). */
#include <string.h>

#include "rn-num.h"
#include "rn-pass.h"
#include "rn-pystr.h"

static int prefix(const uint32_t *w, size_t n, const uint32_t *p)
{
	size_t k = 0;
	while (p[k])
		k++;
	return n >= k && !memcmp(w, p, k * sizeof *p);
}

static int needs_so(const uint32_t *w, size_t n)
{
	static const uint32_t *const exact[] = {U"сто", U"ста"};
	static const uint32_t *const pre[] = {U"втор", U"стотысячн", U"стомиллионн",
	                                      U"стомиллиардн"};
	for (size_t i = 0; i < sizeof exact / sizeof *exact; i++)
		if (n == 3 && prefix(w, n, exact[i]))
			return 1;
	for (size_t i = 0; i < sizeof pre / sizeof *pre; i++)
		if (prefix(w, n, pre[i]))
			return 1;
	return 0;
}

int rn_run_so(const rn_str *in, rn_str *out)
{
	const uint32_t *s = in->s;
	size_t n = in->n, done = 0;
	rn_str low = {0};
	int rc = 0;
	for (size_t i = 0; i + 2 < n; i++) {
		if ((s[i] != U'с' && s[i] != U'С') || s[i + 1] != ' ' ||
		    (i && (rn_py_isalnum(s[i - 1]) || s[i - 1] == '_')))
			continue;
		size_t a = i + 2, b = a;
		while (b < n && rn_py_isalpha(s[b]))
			b++;
		if (b == a || (b < n && (rn_py_isalnum(s[b]) || s[b] == '_')))
			continue;
		low.n = 0;
		if (rn_py_lower(s + a, b - a, &low) < 0)
			goto fail;
		if (!needs_so(low.s, low.n))
			continue;
		if (rn_str_put(out, s + done, i + 1 - done) < 0 ||
		    rn_str_putc(out, rn_py_isupper(s[i]) && b - a > 1 && rn_py_isupper(s[a + 1]) ? U'О' : U'о') < 0)
			goto fail;
		done = i + 1;
	}
	if (rn_str_put(out, s + done, n - done) < 0)
		goto fail;
	goto end;
fail:
	rc = -1;
end:
	rn_str_free(&low);
	return rc;
}
