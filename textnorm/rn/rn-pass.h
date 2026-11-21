/* The ru_normalizr passes in C. Each appends the result for in to out; 0, -1 on error. */
#ifndef RN_PASS_H
#define RN_PASS_H

#include "rn-ph.h"
#include "rn-re.h"

int rn_run_urls(const rn_str *in, rn_str *out);
int rn_run_math(const rn_str *in, rn_str *out);
/* _run_preprocess_steps(apply_caps_normalization=False), _run_caps_normalization, and
 * remove_numeric_footnotes with the default ignore interval */
int rn_run_preprocess(const rn_str *in, rn_str *out);
int rn_run_caps(const rn_str *in, rn_str *out);
int rn_remove_numeric_footnotes(const rn_str *in, rn_str *out);

/* text with the fragments hidden in out; ph is initialised here (rn_ph_free it, also on
 * error) and later restores them with rn_ph_restore */
int rn_protect_paths(const rn_str *in, rn_str *out, rn_ph *ph);
int rn_protect_overlong(const rn_str *in, rn_str *out, rn_ph *ph);
int rn_run_protect(const rn_str *in, rn_str *out);   /* both, for tests */
/* normalize_roman of roman_numerals.py (needs rn_morph and rn_mx_init) */
int rn_run_roman(const rn_str *in, rn_str *out);
int rn_run_dates_time(const rn_str *in, rn_str *out);
/* R4 (RN_FIX_SO): "с" → "со" before "сто", "ста", "втор…"; not in the oracle */
int rn_run_so(const rn_str *in, rn_str *out);

#endif
