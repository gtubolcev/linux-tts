/* protect_windows_paths (normalizer.py) and protect_overlong_numbers (overlong_numbers.py):
 * fragments no pass may touch, hidden behind placeholders for the whole run. */
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-util.h"

static int repl_path(const rn_match *m, rn_str *out, void *ctx)
{
	rn_ph *ph = ctx;
	size_t n, k;
	const uint32_t *path = rn_group(m, 1, &n);
	for (k = n; k > 0; k--) {   /* trailing .,;:!? stays outside */
		uint32_t c = path[k - 1];
		if (c != '.' && c != ',' && c != ';' && c != ':' && c != '!' && c != '?')
			break;
	}
	long p = k ? rn_ph_alloc(ph, path, k) : 0;
	if (p < 0)
		return -1;
	if (!p) {
		size_t n0;
		const uint32_t *s = rn_group(m, 0, &n0);
		return rn_str_put(out, s, n0);
	}
	return rn_str_putc(out, (uint32_t)p) < 0 || rn_str_put(out, path + k, n - k) < 0 ? -1 : 0;
}

static int repl_overlong(const rn_match *m, rn_str *out, void *ctx)
{
	size_t n;
	const uint32_t *s = rn_group(m, 0, &n);
	long p = rn_ph_alloc(ctx, s, n);
	if (p < 0)
		return -1;
	return p ? rn_str_putc(out, (uint32_t)p) : rn_str_put(out, s, n);
}

static int protect(const rn_re *re, uint32_t start, uint32_t end, rn_repl_fn repl,
                   const rn_str *in, rn_str *out, rn_ph *ph)
{
	rn_match m;
	if (rn_ph_init(ph, start, end, in->s, in->n) < 0)
		return -1;
	int r = rn_search(re, in->s, in->n, 0, &m);
	if (r < 0)
		return -1;
	if (!r)
		return rn_str_put(out, in->s, in->n);
	return rn_sub(re, in->s, in->n, repl, ph, 0, out) < 0 ? -1 : 0;
}

int rn_protect_paths(const rn_str *in, rn_str *out, rn_ph *ph)
{
	return protect(rn_pat(RN_NORMALIZER_WINDOWS_PATH_PATTERN), RN_PH_WINDOWS_PATHS, repl_path,
	               in, out, ph);
}

int rn_protect_overlong(const rn_str *in, rn_str *out, rn_ph *ph)
{
	return protect(rn_pat(RN_OVERLONG_NUMBERS_OVERLONG_DIGIT_RUN_PATTERN),
	               RN_PH_OVERLONG_NUMBERS, repl_overlong, in, out, ph);
}

/* both, as between run_urls and run_math; for rn-stage-test, which also counts an error
 * when restoring does not give the input back */
int rn_run_protect(const rn_str *in, rn_str *out)
{
	rn_ph paths = {0}, overlong = {0};
	rn_str t = {0}, u = {0}, back = {0};
	size_t mark = out->n;
	int r = rn_protect_paths(in, &t, &paths);
	if (r == 0)
		r = rn_protect_overlong(&t, out, &overlong);
	if (r == 0 && (rn_ph_restore(&overlong, out->s + mark, out->n - mark, &u) < 0 ||
	               rn_ph_restore(&paths, u.s, u.n, &back) < 0 || !rn_str_eq(&back, in)))
		r = -1;
	rn_ph_free(&paths);
	rn_ph_free(&overlong);
	rn_str_free(&t);
	rn_str_free(&u);
	rn_str_free(&back);
	return r;
}
