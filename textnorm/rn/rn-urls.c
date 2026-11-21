/* urls.py: URLs, e-mail addresses, bare domains, @handles and #hashtags read out. */
#include <stdlib.h>
#include <string.h>

#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-util.h"

static const char *const KNOWN_TLDS[] = {
	"ru", "su", "ua", "by", "kz", "com", "net", "org", "info", "biz",
	"io", "ai", "app", "dev", "me", "co", "uk", "de", "fr", "it", "es",
	"pl", "cz", "us", "tv", "fm", "edu", "gov", "mil", "int", "online",
	"site", "store", "blog", "news", "shop", "club", "pro", "xyz",
};

static const struct {
	uint32_t c;
	const char32_t *w;
} SEPARATOR_WORDS[] = {
	{':', U"двоеточие"}, {'/', U"слэш"}, {'.', U"точка"}, {'?', U"вопрос"},
	{'&', U"амперсанд"}, {'=', U"равно"}, {'-', U"дефис"}, {'_', U"нижнее подчёркивание"},
	{'#', U"решётка"}, {'%', U"процент"}, {'+', U"плюс"}, {'@', U"собака"}, {'~', U"тильда"},
};

static const char32_t *const DIGIT_WORDS[10] = {
	U"ноль", U"один", U"два", U"три", U"четыре", U"пять", U"шесть", U"семь", U"восемь",
	U"девять",
};

/* tld.casefold() in KNOWN_TLDS. The domain pattern also lets in ı İ ſ K (IGNORECASE):
 * ſ and K fold to s and k, ı and İ to nothing ASCII */
static int known_tld(const uint32_t *s, size_t n)
{
	size_t slash = 0;
	while (slash < n && s[slash] != '/')
		slash++;
	size_t dot = slash;
	while (dot > 0 && s[dot - 1] != '.')
		dot--;
	uint32_t f[8];
	size_t fn = slash - dot;
	if (fn > 8)
		return 0;
	for (size_t i = 0; i < fn; i++)
		f[i] = s[dot + i] == 0x17F ? 's' : s[dot + i] == 0x212A ? 'k' : s[dot + i];
	for (size_t i = 0; i < sizeof KNOWN_TLDS / sizeof *KNOWN_TLDS; i++)
		if (rn_eq_ascii_ci(f, fn, KNOWN_TLDS[i]))
			return 1;
	return 0;
}

static size_t count_char(const uint32_t *s, size_t n, uint32_t c)
{
	size_t k = 0;
	for (size_t i = 0; i < n; i++)
		k += s[i] == c;
	return k;
}

/* _split_trailing_punctuation: returns the length of the url part */
static size_t split_trailing(const uint32_t *s, size_t n)
{
	while (n && s[n - 1] < 0x80 && s[n - 1] && strchr(".,;:!?", (int)s[n - 1]))
		n--;
	while (n) {
		uint32_t c = s[n - 1], o = c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : 0;
		if (!o || count_char(s, n, c) <= count_char(s, n, o))
			break;
		n--;
	}
	return n;
}

/* pieces joined by " " with empty pieces dropped: a piece is put with a leading space
 * unless it is the first */
struct join {
	rn_str *out;
	int any;
};

static int join_put(struct join *j, const uint32_t *s, size_t n)
{
	if (!n)
		return 0;
	if (j->any && rn_str_putc(j->out, ' ') < 0)
		return -1;
	j->any = 1;
	return rn_str_put(j->out, s, n);
}

static int is_ascii_digits(const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!rn_is_ascii_digit(s[i]))
			return 0;
	return n > 0;
}

static int read_digit_run(struct join *j, const uint32_t *s, size_t n)
{
	/* " ".join of digit words: one piece */
	rn_str t = {0};
	for (size_t i = 0; i < n; i++)
		if ((i && rn_str_putc(&t, ' ') < 0) ||
		    rn_puts(&t, (const uint32_t *)DIGIT_WORDS[s[i] - '0']) < 0)
			goto fail;
	int r = join_put(j, t.s, t.n);
	rn_str_free(&t);
	return r;
fail:
	rn_str_free(&t);
	return -1;
}

/* _normalize_alnum_chunk for a chunk of [A-Za-z0-9]+ */
static int alnum_chunk(struct join *j, const uint32_t *s, size_t n)
{
	if (is_ascii_digits(s, n))
		return read_digit_run(j, s, n);
	/* letters and digits mixed: runs of each, " "-joined (a single piece) */
	rn_str t = {0};
	struct join tj = {&t, 0};
	for (size_t i = 0; i < n;) {
		size_t k = i;
		int dig = rn_is_ascii_digit(s[i]);
		while (k < n && rn_is_ascii_digit(s[k]) == dig)
			k++;
		if ((dig ? read_digit_run(&tj, s + i, k - i) : join_put(&tj, s + i, k - i)) < 0) {
			rn_str_free(&t);
			return -1;
		}
		i = k;
	}
	int r = join_put(j, t.s, t.n);
	rn_str_free(&t);
	return r;
}

/* _normalize_url, appended to out */
static int normalize_url(const uint32_t *url, size_t n, rn_str *out)
{
	rn_str t = {0};
	struct join j = {&t, 0};
	int r = 0;
	/* ALNUM_CHUNK_PATTERN: [A-Za-z0-9]+ or one other character */
	for (size_t i = 0; i < n && r == 0;) {
		uint32_t c = url[i];
		if (rn_is_ascii_alpha(c) || rn_is_ascii_digit(c)) {
			size_t k = i;
			while (k < n && (rn_is_ascii_alpha(url[k]) || rn_is_ascii_digit(url[k])))
				k++;
			if (rn_eq_ascii_ci(url + i, k - i, "ru") && i > 0 && url[i - 1] == '.')
				r = rn_puts(&t, j.any ? RN_U(" ру") : RN_U("ру")), j.any = 1;
			else if (rn_eq_ascii_ci(url + i, k - i, "docs"))
				r = rn_puts(&t, j.any ? RN_U(" докс") : RN_U("докс")), j.any = 1;
			else
				r = alnum_chunk(&j, url + i, k - i);
			i = k;
			continue;
		}
		/* any other single character: an alphanumeric one stays itself (isalpha, or a
		 * digit outside ASCII that MIXED_CHUNK_PATTERN skips), the rest via the table */
		const uint32_t *w = &url[i];
		size_t wn = 1;
		for (size_t s = 0; s < sizeof SEPARATOR_WORDS / sizeof *SEPARATOR_WORDS; s++)
			if (SEPARATOR_WORDS[s].c == c) {
				w = (const uint32_t *)SEPARATOR_WORDS[s].w;
				wn = rn_u32len(w);
			}
		r = join_put(&j, w, wn);
		i++;
	}
	if (r == 0) {
		/* re.sub(r"\s+", " ", ...).strip() */
		rn_str u = {0};
		if (rn_sub_text(rn_pat(RN_ABBREVIATIONS_L156), t.s ? t.s : RN_U(""), t.n,
		                RN_U(" "), 1, 0, &u) < 0) {
			r = -1;
		} else {
			size_t a = 0, b = u.n;
			while (a < b && rn_is_space(u.s[a]))
				a++;
			while (b > a && rn_is_space(u.s[b - 1]))
				b--;
			r = rn_str_put(out, u.s + a, b - a);
		}
		rn_str_free(&u);
	}
	rn_str_free(&t);
	return r;
}

static int repl_url(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *raw = rn_group(m, 1, &n);
	size_t k = split_trailing(raw, n);
	if (!k)
		return rn_str_put(out, raw, n);   /* return raw_url */
	if (normalize_url(raw, k, out) < 0)
		return -1;
	return rn_str_put(out, raw + k, n - k);
}

static int repl_bare(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *raw = rn_group(m, 1, &n);
	size_t k = split_trailing(raw, n);
	if (!k)   /* return match.group(0) */
		return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
	if (normalize_url(raw, k, out) < 0)
		return -1;
	return rn_str_put(out, raw + k, n - k);
}

static int repl_bare_domain(const rn_match *m, rn_str *out, void *ctx)
{
	size_t n;
	const uint32_t *d = rn_group(m, 1, &n);
	if (!known_tld(d, n))
		return rn_str_put(out, m->s + m->start[0], m->end[0] - m->start[0]);
	return repl_bare(m, out, ctx);
}

static int repl_handle(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *h = rn_group(m, 1, &n);
	if (rn_puts(out, RN_U("собака")) < 0)
		return -1;
	/* re.split(r"(_)") without empty parts, "_" read as подчёркивание */
	for (size_t i = 0; i < n;) {
		size_t k = i;
		if (h[i] == '_')
			k++;
		else
			while (k < n && h[k] != '_')
				k++;
		if (rn_str_putc(out, ' ') < 0)
			return -1;
		if (h[i] == '_' ? rn_puts(out, RN_U("подчёркивание")) : rn_str_put(out, h + i, k - i))
			return -1;
		i = k;
	}
	return 0;
}

static int is_cyr(uint32_t c)
{
	return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451;
}

static int repl_hashtag(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *t = rn_group(m, 1, &n);
	if (rn_puts(out, RN_U("решётка")) < 0)
		return -1;
	rn_iter it = rn_iter_init(rn_pat(RN_URLS_TAG_CHUNK_PATTERN), t, n, 0);
	rn_match c;
	int r;
	while ((r = rn_iter_next(&it, &c)) == 1) {
		const uint32_t *p = t + c.start[0];
		size_t pn = c.end[0] - c.start[0];
		if (rn_str_putc(out, ' ') < 0)
			return -1;
		if (pn == 1 && p[0] == '_') {
			if (rn_puts(out, RN_U("подчёркивание")) < 0)
				return -1;
			continue;
		}
		int cyr = 0;
		for (size_t i = 0; i < pn; i++)
			cyr |= is_cyr(p[i]);
		for (size_t i = 0; i < pn; i++) {
			uint32_t ch = p[i];
			if (cyr)   /* casefold of [A-Za-zА-Яа-яЁё]: plain lower case */
				ch = ch >= 'A' && ch <= 'Z' ? ch | 0x20
				   : ch >= 0x410 && ch <= 0x42F ? ch + 0x20
				   : ch == 0x401 ? 0x451 : ch;
			if (rn_str_putc(out, ch) < 0)
				return -1;
		}
	}
	return r < 0 ? -1 : 0;
}

int rn_run_urls(const rn_str *in, rn_str *out)
{
	static const struct {
		enum rn_pattern id;
		rn_repl_fn fn;
	} SUBS[] = {
		{RN_URLS_URL_PATTERN, repl_url},
		{RN_URLS_EMAIL_PATTERN, repl_bare},
		{RN_URLS_BARE_DOMAIN_PATTERN, repl_bare_domain},
		{RN_URLS_SOCIAL_HANDLE_PATTERN, repl_handle},
		{RN_URLS_HASHTAG_PATTERN, repl_hashtag},
	};
	rn_match m;
	int any = 0;
	for (size_t i = 0; i < 5 && !any; i++) {
		int r = rn_search(rn_pat(SUBS[i].id), in->s, in->n, 0, &m);
		if (r < 0)
			return -1;
		any = r;
	}
	if (!any)
		return rn_str_put(out, in->s, in->n);
	rn_str cur = {0}, next = {0};
	if (rn_str_put(&cur, in->s, in->n) < 0)
		return -1;
	for (size_t i = 0; i < 5; i++) {
		next.n = 0;
		if (rn_sub(rn_pat(SUBS[i].id), cur.s, cur.n, SUBS[i].fn, NULL, 0, &next) < 0) {
			rn_str_free(&cur);
			rn_str_free(&next);
			return -1;
		}
		rn_str t = cur;
		cur = next;
		next = t;
	}
	int r = rn_str_put(out, cur.s, cur.n);
	rn_str_free(&cur);
	rn_str_free(&next);
	return r;
}
