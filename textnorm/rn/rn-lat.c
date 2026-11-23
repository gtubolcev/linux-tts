/* latinization.py apply_latinization with the tts() options (backend "ipa"): Latin letter
 * labels by name, then every Latin token through eng_to_ipa's CMU transcription
 * (rn_lat_open loads the word → IPA table made by gen-ipa.py) and _ipa_to_russian; words
 * not in CMU through the bundled latinization_rules.dic (rn-dic.c). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rn-dic.h"
#include "rn-lat.h"
#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-pystr.h"
#include "rn-tables.h"
#include "rn-util.h"
#include "rn-latdic.h"

#define LAT(id) rn_pat(RN_LATINIZATION_##id)

extern int rn_letter_double;
int rn_lat_stress = 0;   /* enable_latinization_stress_marks */

/* ---- the IPA table ---- */

static char *ipa_buf;
static const char **ipa_line;   /* "word\tipa" lines, sorted */
static size_t ipa_n;

void rn_lat_close(void)
{
	free(ipa_buf);
	free(ipa_line);
	ipa_buf = NULL;
	ipa_line = NULL;
	ipa_n = 0;
}

int rn_lat_open(const char *path, const char **err)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		*err = "cannot open";
		return -1;
	}
	char *buf = NULL;
	size_t n = 0, cap = 0, k;
	char tmp[65536];
	while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) {
		if (n + k + 1 > cap) {
			cap = (n + k + 1) * 2;
			char *p = realloc(buf, cap);
			if (!p) {
				free(buf);
				fclose(f);
				*err = "out of memory";
				return -1;
			}
			buf = p;
		}
		memcpy(buf + n, tmp, k);
		n += k;
	}
	int bad = ferror(f);
	fclose(f);
	if (bad || !buf) {
		free(buf);
		*err = bad ? "read error" : "empty";
		return -1;
	}
	buf[n] = 0;
	size_t lines = 0;
	for (size_t i = 0; i < n; i++)
		lines += buf[i] == '\n';
	const char **idx = malloc((lines + 1) * sizeof *idx);
	if (!idx) {
		free(buf);
		*err = "out of memory";
		return -1;
	}
	size_t m = 0;
	for (char *p = buf; *p;) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		if (*p != '#' && strchr(p, '\t')) {
			if (m && strcmp(idx[m - 1], p) >= 0) {
				free(idx);
				free(buf);
				*err = "not sorted";
				return -1;
			}
			idx[m++] = p;
		}
		if (!e)
			break;
		p = e + 1;
	}
	rn_lat_close();
	ipa_buf = buf;
	ipa_line = idx;
	ipa_n = m;
	return 0;
}

/* the IPA of an ASCII word, or NULL (not in CMU) */
static const char *ipa_get(const char *w)
{
	size_t lo = 0, hi = ipa_n, wl = strlen(w);
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const char *l = ipa_line[mid];
		int c = strncmp(l, w, wl);
		if (!c)
			c = l[wl] == '\t' ? 0 : 1;   /* the line's word is longer */
		if (!c)
			return l + wl + 1;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* eng_to_ipa.convert(word) for a token word ([a-z][a-z'-]*): the IPA with the stripped
 * trailing "'"/"-" put back, appended to out; 0 when the word is unknown (eng_to_ipa marks
 * it with "*"), 1 found, -1 error */
static int convert(const uint32_t *w, size_t n, rn_str *out)
{
	size_t e = n;
	while (e && (w[e - 1] == '\'' || w[e - 1] == '-'))
		e--;
	char *a = malloc(e + 1);
	if (!a)
		return -1;
	for (size_t i = 0; i < e; i++)
		a[i] = (char)w[i];
	a[e] = 0;
	const char *ipa = e ? ipa_get(a) : NULL;
	free(a);
	if (!ipa)
		return 0;
	const char *t = strchr(ipa, '\n');
	size_t len = t ? (size_t)(t - ipa) : strlen(ipa);
	char *z = malloc(len + 1);
	if (!z)
		return -1;
	memcpy(z, ipa, len);
	z[len] = 0;
	int r = rn_str_utf8(out, z) < 0 || rn_str_put(out, w + e, n - e) < 0 ? -1 : 1;
	free(z);
	return r;
}

/* ---- _ipa_to_russian ---- */

static const char32_t *const IPA_MAP[][2] = {
	{U"wɝr", U"вор"}, {U"wɜːr", U"вор"}, {U"wɚr", U"вор"}, {U"wɝ", U"вор"},
	{U"wɜː", U"вор"}, {U"wɜ", U"вор"},   {U"wɚ", U"вор"},  {U"ʧ", U"ч"},
	{U"ʤ", U"дж"},   {U"tʃ", U"ч"},     {U"dʒ", U"дж"},   {U"aɪ", U"ай"},
	{U"eɪ", U"ей"},  {U"ɔɪ", U"ой"},    {U"aʊ", U"ау"},   {U"oʊ", U"оу"},
	{U"əʊ", U"оу"},  {U"ju", U"ю"},     {U"jʊ", U"ю"},    {U"ɪə", U"иэ"},
	{U"eə", U"эа"},  {U"ʊə", U"уэ"},    {U"ɝr", U"эр"},   {U"ɝ", U"эр"},
	{U"ɜːr", U"эр"}, {U"ɜː", U"эр"},    {U"ɚ", U"эр"},    {U"ɾ", U"т"},
	{U"ŋ", U"н"},    {U"ʃ", U"ш"},      {U"ʒ", U"ж"},     {U"θ", U"с"},
	{U"ð", U"з"},    {U"ə", U"э"},      {U"ɜ", U"э"},     {U"e", U"е"},
	{U"æ", U"э"},    {U"ɪ", U"и"},      {U"ɛ", U"э"},     {U"i", U"и"},
	{U"ɒ", U"о"},    {U"ɔ", U"о"},      {U"ʊ", U"у"},     {U"u", U"у"},
	{U"ʌ", U"а"},    {U"a", U"а"},      {U"ɑ", U"а"},     {U"p", U"п"},
	{U"b", U"б"},    {U"t", U"т"},      {U"d", U"д"},     {U"k", U"к"},
	{U"g", U"г"},    {U"f", U"ф"},      {U"v", U"в"},     {U"s", U"с"},
	{U"z", U"з"},    {U"h", U"х"},      {U"m", U"м"},     {U"n", U"н"},
	{U"l", U"л"},    {U"r", U"р"},      {U"j", U"й"},     {U"w", U"в"},
	{U"ʔ", U"т"},    {U"ˌ", U""},
};

static const char32_t *const LONG_VOWELS[][2] = {
	{U"iː", U"ii"}, {U"uː", U"uu"}, {U"ɑː", U"aa"}, {U"ɔː", U"oo"},
};

#define STRESS 0x2C8u   /* ˈ */

static int one_word(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const uint32_t *s = m->s + m->start[0];
	size_t n = m->end[0] - m->start[0], i = 0;
	while (i < n && s[i] != STRESS)
		i++;
	if (i == n)
		return rn_str_put(out, s, n);
	if (rn_str_put(out, s, i) < 0 || rn_str_putc(out, '+') < 0)
		return -1;
	for (i++; i < n; i++)
		if (s[i] != STRESS && rn_str_putc(out, s[i]) < 0)
			return -1;
	return 0;
}

/* _ipa_to_russian(t) in place */
static int ipa_to_russian(rn_str *t)
{
	if (rn_py_rereplace(t, RN_U("ˌ"), RN_U("")) < 0)
		return -1;
	for (size_t i = 0; i < sizeof LONG_VOWELS / sizeof *LONG_VOWELS; i++)
		if (rn_py_rereplace(t, (const uint32_t *)LONG_VOWELS[i][0],
		                    (const uint32_t *)LONG_VOWELS[i][1]) < 0)
			return -1;
	for (size_t i = 0; i < sizeof IPA_MAP / sizeof *IPA_MAP; i++)
		if (rn_py_rereplace(t, (const uint32_t *)IPA_MAP[i][0], (const uint32_t *)IPA_MAP[i][1]) < 0)
			return -1;
	if (rn_py_rereplace(t, RN_U("ː"), RN_U("")) < 0)
		return -1;
	/* move_stress_marker_ru */
	int pending = 0;
	rn_str r = {0};
	for (size_t i = 0; i < t->n; i++) {
		uint32_t c = t->s[i];
		if (c == STRESS) {
			pending = 1;
			continue;
		}
		if (pending) {
			const uint32_t *v = RN_U("аеёиоуыэюя");
			while (*v && *v != c)
				v++;
			if (*v) {
				if (rn_str_putc(&r, STRESS) < 0)
					goto oom;
				pending = 0;
			}
		}
		if (rn_str_putc(&r, c) < 0)
			goto oom;
	}
	t->n = 0;
	if (rn_str_put(t, r.s ? r.s : RN_U(""), r.n) < 0)
		goto oom;
	rn_str_free(&r);
	if (!rn_lat_stress)
		return rn_py_rereplace(t, (const uint32_t *)U"ˈ", RN_U(""));
	/* _single_stress_per_word */
	if (rn_resub(LAT(WORD_WITH_STRESS_RE), t, one_word, NULL) < 0)
		return -1;
	return rn_py_rereplace(t, (const uint32_t *)U"ˈ", RN_U("+"));
oom:
	rn_str_free(&r);
	return -1;
}

/* ---- the dictionary fallback ---- */

static rn_dic *lat_dic;
static pthread_once_t lat_once = PTHREAD_ONCE_INIT;

static void lat_dic_init(void)
{
	char err[300];
	lat_dic = rn_dic_parse(RN_LATIN_DIC, sizeof RN_LATIN_DIC - 1, err, sizeof err);
	if (!lat_dic)
		fprintf(stderr, "latinization_rules.dic: %s\n", err);
}

/* _resolve_unknown_latin_fallback(word) in place: up to six passes of the rules */
static int dic_fallback(rn_str *w)
{
	pthread_once(&lat_once, lat_dic_init);
	if (!lat_dic)
		return -1;
	rn_str prev = {0};
	int r = 0;
	for (int i = 0; i < 6 && !r; i++) {
		if (rn_str_copy(&prev, w) < 0 || rn_dic_apply(lat_dic, w) < 0) {
			r = -1;
			break;
		}
		if (rn_str_eq(&prev, w))
			break;
	}
	rn_str_free(&prev);
	return r;
}

/* ---- apply_latinization ---- */

static int neighbour_letter(uint32_t c)
{
	return rn_is_ascii_alpha(c) || (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451;
}

/* _reads_as_letter_label(text, start, end) */
static int reads_as_label(const uint32_t *s, size_t n, size_t start, size_t end)
{
	size_t a = start > 48 ? start - 48 : 0, b = end + 48 < n ? end + 48 : n;
	int have = 0;
	/* left[-1:]: the last word of s[a:start] */
	size_t i = start;
	while (i > a && !neighbour_letter(s[i - 1]))
		i--;
	if (i > a) {
		size_t j = i;
		while (j > a && neighbour_letter(s[j - 1]))
			j--;
		if (s[j] < 0x80)
			return 0;
		have = 1;
	}
	/* right[:1] */
	i = end;
	while (i < b && !neighbour_letter(s[i]))
		i++;
	if (i < b) {
		if (s[i] < 0x80)
			return 0;
		have = 1;
	}
	return have;
}

static int letter_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	const rn_re *p = LAT(ALPHANUMERIC_SUFFIX_LETTER_PATTERN);
	size_t n;
	const uint32_t *l = rn_group(m, rn_re_group(p, "letter"), &n);
	if (!l) {
		l = rn_group(m, rn_re_group(p, "label"), &n);
		if (!reads_as_label(m->s, m->n, m->start[0], m->end[0]))
			return rn_str_put(out, l, n);
	}
	const char *name = rn_letter_name(l[0], rn_letter_double);
	return name ? rn_str_utf8(out, name) : rn_str_put(out, l, n);
}

/* the reading of one lowered token ("’" already "'") */
static int word_reading(const rn_str *w, rn_str *out)
{
	rn_str t = {0};
	int k = convert(w->s, w->n, &t);
	if (k == 0) {
		/* elision ("d'orge"): retry without the apostrophes, then the dictionary */
		rn_str fused = {0};
		for (size_t i = 0; i < w->n && k >= 0; i++)
			if (w->s[i] != '\'' && rn_str_putc(&fused, w->s[i]) < 0)
				k = -1;
		if (k == 0 && fused.n != w->n)
			k = convert(fused.s, fused.n, &t);
		if (k == 0) {
			if (!fused.s && rn_str_put(&fused, RN_U(""), 0) < 0)
				k = -1;
			else if (dic_fallback(&fused) < 0 || rn_str_put(out, fused.s, fused.n) < 0)
				k = -1;
			rn_str_free(&fused);
			rn_str_free(&t);
			return k < 0 ? -1 : 0;
		}
		rn_str_free(&fused);
	}
	if (k < 0 || ipa_to_russian(&t) < 0 || rn_str_put(out, t.s, t.n) < 0) {
		rn_str_free(&t);
		return -1;
	}
	rn_str_free(&t);
	return 0;
}

static int token_repl(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	rn_str w = {0};
	int r = rn_py_lower(m->s + m->start[0], m->end[0] - m->start[0], &w);
	for (size_t i = 0; i < w.n; i++)
		if (w.s[i] == 0x2019)
			w.s[i] = '\'';
	const uint32_t *own = r >= 0 ? rn_tech_word(w.s, w.n) : NULL;   /* R30 */
	if (own)
		r = rn_puts(out, own);
	else if (r >= 0)
		r = word_reading(&w, out);
	rn_str_free(&w);
	return r;
}

int rn_run_latinization(const rn_str *in, rn_str *out)
{
	size_t i = 0;
	while (i < in->n && !rn_is_ascii_alpha(in->s[i]))
		i++;
	if (i == in->n)
		return rn_str_put(out, in->s ? in->s : RN_U(""), in->n);
	if (!ipa_line)
		return -1;   /* rn_lat_open was not called */
	rn_str t = {0};
	int err = rn_sub(LAT(ALPHANUMERIC_SUFFIX_LETTER_PATTERN), in->s, in->n, letter_repl, NULL, 0,
	                 &t) < 0;
	if (!err && !t.s)
		err = rn_str_put(&t, RN_U(""), 0) < 0;
	if (!err)
		err = rn_sub(LAT(LATIN_TOKEN_PATTERN), t.s, t.n, token_repl, NULL, 0, out) < 0;
	rn_str_free(&t);
	return err ? -1 : 0;
}
