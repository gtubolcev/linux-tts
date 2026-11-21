#define _POSIX_C_SOURCE 200809L
/* rn-stage-test STAGE PAIRS.bin [SHOW]: runs one C pass over the inputs of
 * oracle/stage-pairs.py and compares with the oracle's outputs. Prints the count of
 * mismatches and the first SHOW (default 5) of them; exit 1 on any mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rn-lat.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pre.h"
#include "rn-morph.h"
#include "rn-mx.h"
#include "rn-num.h"
#include "rn-npass.h"

#define RN_NPASS_WRAP(py, c)                                       \
	static int nw_##c(const rn_str *in, rn_str *out)                 \
	{                                                                \
		out->n = 0;                                                  \
		rn_mx_failed = 0;                                            \
		if (rn_str_put(out, in->s, in->n) < 0)                       \
			return -1;                                               \
		return rn_n_##c(out);                                        \
	}
RN_NPASS_FUNCS(RN_NPASS_WRAP)

static const struct {
	const char *name;
	int (*fn)(const rn_str *, rn_str *);
} STAGES[] = {
	{"run_urls", rn_run_urls},
	{"run_math", rn_run_math},
	{"protect", rn_run_protect},
	{"_run_preprocess_steps", rn_run_preprocess},
	{"_run_caps_normalization", rn_run_caps},
	{"remove_numeric_footnotes", rn_remove_numeric_footnotes},
	{"run_roman", rn_run_roman},
	{"run_dates_time", rn_run_dates_time},
	{"run_years", rn_run_years},
	{"run_so", rn_run_so},
	{"run_numerals", rn_run_numerals},
	{"run_abbreviations", rn_run_abbreviations},
	{"run_latinization", rn_run_latinization},
#define RN_PRE_ROW(name) {#name, rn_pre_##name},
	RN_PRE_FUNCS(RN_PRE_ROW)
#define RN_NPASS_ROW(py, c) {"normalize_" #py, nw_##c},
	RN_NPASS_FUNCS(RN_NPASS_ROW)
};

static void put_utf8(FILE *f, const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (c == '\n') {
			fputs("\\n", f);
		} else if (c < 0x80) {
			fputc((int)c, f);
		} else if (c < 0x800) {
			fputc(0xC0 | c >> 6, f);
			fputc(0x80 | (c & 0x3F), f);
		} else if (c < 0x10000) {
			fputc(0xE0 | c >> 12, f);
			fputc(0x80 | (c >> 6 & 0x3F), f);
			fputc(0x80 | (c & 0x3F), f);
		} else {
			fputc(0xF0 | c >> 18, f);
			fputc(0x80 | (c >> 12 & 0x3F), f);
			fputc(0x80 | (c >> 6 & 0x3F), f);
			fputc(0x80 | (c & 0x3F), f);
		}
	}
}

static int read_rec(FILE *f, rn_str *out)
{
	size_t n;
	if (fscanf(f, "%zu", &n) != 1)
		return 0;
	fgetc(f);
	char *b = malloc(n + 1);
	if (!b || fread(b, 1, n, f) != n)
		return -1;
	b[n] = 0;
	out->n = 0;
	int r = rn_str_utf8(out, b) < 0 ? -1 : 1;
	if (!out->s)
		rn_str_put(out, NULL, 0);
	free(b);
	return r;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	int (*fn)(const rn_str *, rn_str *) = NULL;
	for (size_t i = 0; i < sizeof STAGES / sizeof *STAGES; i++)
		if (!strcmp(argv[1], STAGES[i].name))
			fn = STAGES[i].fn;
	if (!fn) {
		fprintf(stderr, "unknown stage %s\n", argv[1]);
		return 2;
	}
	long show = argc > 3 ? atol(argv[3]) : 5;
	char err[300];
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (getenv("RN_FIXES"))   /* RN_FIX_* mask: the fixed mode instead of the oracle's */
		rn_fixes = (unsigned)strtoul(getenv("RN_FIXES"), NULL, 0);
	if (getenv("RN_N2W_FIXES"))   /* N2W_FIX_* mask for every num2words call */
		rn_n2w_fixes = (unsigned)strtoul(getenv("RN_N2W_FIXES"), NULL, 0);
	const char *dict = getenv("RN_MORPH_DICT");
	if (dict) {
		const char *merr = NULL;
		if (rn_morph_open(dict, &merr) < 0) {
			fprintf(stderr, "%s: %s\n", dict, merr ? merr : "cannot open");
			return 2;
		}
		if (rn_mx_init() < 0) {
			fprintf(stderr, "%s: grammemes missing\n", dict);
			return 2;
		}
	}
	const char *ipa = getenv("RN_IPA_DICT");
	if (ipa) {
		const char *lerr = "";
		if (rn_lat_open(ipa, &lerr) < 0) {
			fprintf(stderr, "%s: %s\n", ipa, lerr);
			return 2;
		}
	}
	if (rn_pat_init(err, sizeof err) < 0) {
		fprintf(stderr, "%s\n", err);
		return 2;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fprintf(stderr, "patterns compiled in %.2f s\n",
	        t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9);
	FILE *f = fopen(argv[2], "rb");
	if (!f)
		return 2;
	rn_str in = {0}, want = {0}, got = {0};
	long total = 0, bad = 0, errors = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (read_rec(f, &in) == 1 && read_rec(f, &want) == 1) {
		total++;
		got.n = 0;
		if (fn(&in, &got) < 0) {
			errors++;
			continue;
		}
		if (got.n != want.n || (got.n && memcmp(got.s, want.s, got.n * 4))) {
			if (bad++ < show) {
				printf("--- #%ld\nin:   ", total - 1);
				put_utf8(stdout, in.s, in.n);
				printf("\nwant: ");
				put_utf8(stdout, want.s, want.n);
				printf("\ngot:  ");
				put_utf8(stdout, got.s, got.n);
				putchar('\n');
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("%s: %ld pairs, %ld mismatches, %ld errors, %.2f s\n", argv[1], total, bad, errors,
	       t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9);
	fflush(stdout);
	fclose(f);
	rn_str_free(&in);
	rn_str_free(&want);
	rn_str_free(&got);
	return bad || errors;
}
