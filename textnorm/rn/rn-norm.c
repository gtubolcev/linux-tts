/* normalizer.py PipelineNormalizer._normalize_text with NormalizeOptions.tts(): the whole
 * pipeline. The URL stage runs (enable_url_normalization), numeric footnotes are removed
 * (remove_links), the dictionary stage has no rules (it is the identity with the bundled
 * dictionaries). R4 (RN_FIX_SO) runs last, after the protected fragments are back; our
 * rn_run_tech (R27–R30) first. */
#include "rn-lat.h"
#include "rn-num.h"
#include "rn-pass.h"
#include "rn-ph.h"

typedef int (*stage_fn)(const rn_str *, rn_str *);

/* *t = f(*t) */
static int step(stage_fn f, rn_str *t)
{
	rn_str u = {0};
	if (f(t, &u) < 0) {
		rn_str_free(&u);
		return -1;
	}
	if (!u.s && rn_str_put(&u, NULL, 0) < 0)
		return -1;
	rn_str_free(t);
	*t = u;
	return 0;
}

static int restore(const rn_ph *ph, rn_str *t)
{
	rn_str u = {0};
	if (rn_ph_restore(ph, t->s, t->n, &u) < 0) {
		rn_str_free(&u);
		return -1;
	}
	if (!u.s && rn_str_put(&u, NULL, 0) < 0)
		return -1;
	rn_str_free(t);
	*t = u;
	return 0;
}

int rn_run_pipeline(const rn_str *in, rn_str *out)
{
	static const stage_fn middle[] = {
		rn_run_math, rn_run_preprocess, rn_run_roman, rn_run_caps,
		rn_remove_numeric_footnotes, rn_run_dates_time, rn_run_years, rn_run_numerals,
		rn_run_abbreviations, rn_run_latinization, rn_run_finalize,
	};
	rn_ph paths = {0}, overlong = {0};
	rn_str t = {0}, u = {0};
	int r = -1;
	if (rn_str_put(&t, in->s, in->n) < 0 || (!t.s && rn_str_put(&t, NULL, 0) < 0) ||
	    ((rn_fixes & (RN_FIX_TECH_NUM | RN_FIX_NO_SIGN | RN_FIX_DIV_SLASH | RN_FIX_LAT_WORDS |
	                  RN_FIX_ACRONYMS | RN_FIX_ABBR_READ)) &&
	     step(rn_run_tech, &t) < 0) ||
	    step(rn_run_urls, &t) < 0 ||
	    ((rn_fixes & RN_FIX_NO_SCHEME) && step(rn_run_noscheme, &t) < 0))
		goto done;
	if (rn_protect_paths(&t, &u, &paths) < 0 || (!u.s && rn_str_put(&u, NULL, 0) < 0))
		goto done;
	rn_str_free(&t);
	t = u;
	u = (rn_str){0};
	if (rn_protect_overlong(&t, &u, &overlong) < 0 || (!u.s && rn_str_put(&u, NULL, 0) < 0))
		goto done;
	rn_str_free(&t);
	t = u;
	u = (rn_str){0};
	for (size_t i = 0; i < sizeof middle / sizeof *middle; i++)
		if (step(middle[i], &t) < 0)
			goto done;
	if (restore(&overlong, &t) < 0 || restore(&paths, &t) < 0)
		goto done;
	if ((rn_fixes & RN_FIX_SO) && step(rn_run_so, &t) < 0)
		goto done;
	r = rn_str_put(out, t.s, t.n);
done:
	rn_ph_free(&paths);
	rn_ph_free(&overlong);
	rn_str_free(&t);
	rn_str_free(&u);
	return r;
}
