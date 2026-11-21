/* The ru_normalizr passes in C. Each appends the result for in to out; 0, -1 on error. */
#ifndef RN_PASS_H
#define RN_PASS_H

#include "rn-ph.h"
#include "rn-re.h"

int rn_run_urls(const rn_str *in, rn_str *out);
int rn_run_math(const rn_str *in, rn_str *out);

/* text with the fragments hidden in out; ph is initialised here (rn_ph_free it, also on
 * error) and later restores them with rn_ph_restore */
int rn_protect_paths(const rn_str *in, rn_str *out, rn_ph *ph);
int rn_protect_overlong(const rn_str *in, rn_str *out, rn_ph *ph);
int rn_run_protect(const rn_str *in, rn_str *out);   /* both, for tests */

#endif
